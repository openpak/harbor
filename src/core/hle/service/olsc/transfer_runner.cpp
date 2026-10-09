// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include "core/hle/service/olsc/transfer_runner.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <filesystem>

#include <fmt/format.h>
#include <openssl/sha.h>

#include "common/logging.h"
#include "common/fs/path_util.h"
#include "common/redact.h"
#include "core/core.h"
#include "core/file_sys/fs_save_data_types.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/hle/service/acc/profile_manager.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/hle/service/filesystem/fsp/fs_i_save_data_info_reader.h"
#include "core/hle/service/filesystem/fsp/save_transfer/transfer_service.h"
#include "core/hle/service/filesystem/save_data_controller.h"
#include "core/openpak/async_worker.h"
#include "core/openpak/olsc.h"
#include "core/openpak/session.h"

namespace Service::OLSC {

namespace {

namespace tr = OpenPak::Olsc::Transfer;
namespace st = Service::FileSystem::SaveTransfer;

/// olsc's dauth client id (0x235c00 is called with 0x146c8ac7b8a0db52).
constexpr const char* OlscDauthClientId = "146c8ac7b8a0db52";

/// The console protocol through Harbor's pinned client, as the task's user, with the headers
/// every olsc request carries (0x1e7d10): the bearer (the session's), X-Device-Token and
/// X-NSA-ID.
class SessionRest final : public tr::Rest {
public:
    explicit SessionRest(std::string uid_) : uid{std::move(uid_)} {}

    tr::HttpResponse Request(const std::string& method, const std::string& host,
                             const std::string& path, const std::string& body,
                             const tr::Headers& extra) override {
        // olsc POSTs JSON, some with an empty body; GETs carry none.
        const std::string type = method == "GET" ? "" : "application/json";
        OpenPak::Session::Headers headers{
            {"X-Device-Token", DeviceToken()},
            {"X-NSA-ID", fmt::format("{:016x}", OpenPak::Session::NetworkServiceAccountId(uid))},
        };
        headers.insert(headers.end(), extra.begin(), extra.end());
        const auto response =
            OpenPak::Session::SendAsUser(uid, method, host, path, type, body, headers);
        return {response.status, response.body};
    }

    std::string DeviceToken() override {
        // ponytail: one dauth fetch per task; a task outliving the token's day is not a case.
        if (device_token.empty()) {
            device_token = OpenPak::Session::DauthToken(OlscDauthClientId, {});
        }
        return device_token;
    }

    tr::HttpResponse Blob(const std::string& method, const std::string& url,
                          std::span<const u8> body) override {
        // https://host/path?query; every host goes to the OpenPak server through the pinned client.
        constexpr std::string_view scheme = "https://";
        if (!url.starts_with(scheme)) {
            return {};
        }
        const auto slash = url.find('/', scheme.size());
        const std::string host = url.substr(scheme.size(), slash - scheme.size());
        const std::string path = slash == std::string::npos ? "/" : url.substr(slash);
        const auto response = OpenPak::Session::Send(
            method, host, path, {}, method == "PUT" ? "application/octet-stream" : "",
            std::string(body.begin(), body.end()));
        return {response.status, response.body};
    }

private:
    std::string uid;
    std::string device_token;
};

/// avm's launch version of an installed application >> 16, as olsc reads it (0x1e5670). Harbor
/// has no avm: the version that runs, which is what the console's last pushed launch version is.
u32 LaunchVersion(Core::System& system, u64 application_id) {
    const FileSys::PatchManager pm{application_id, system.GetFileSystemController(),
                                   system.GetContentProviderUnion()};
    return pm.GetGameVersion().value_or(0) >> 16;
}

class ExporterAdapter final : public tr::Exporter {
public:
    explicit ExporterAdapter(std::shared_ptr<st::Exporter> exporter_)
        : exporter{std::move(exporter_)} {}
    bool SetDivisionCount(int count) override {
        return exporter->SetDivisionCount(count).IsSuccess();
    }
    std::vector<u16> ChunkIds() override {
        return exporter->ChunkIds();
    }
    std::optional<tr::Bytes> Chunk(u16 id) override {
        std::vector<u8> bytes;
        if (exporter->ReadChunk(id, &bytes).IsError()) {
            return std::nullopt;
        }
        return bytes;
    }
    void SetAad(const tr::Aad& aad) override {
        exporter->SetAad(aad);
    }
    tr::Aad ImportAad() override {
        return exporter->Aad();
    }
    tr::Key KeySeed() override {
        return exporter->KeySeed();
    }
    std::optional<tr::Key> InitialDataMac() override {
        st::Key128 mac{};
        if (exporter->GetInitialDataMac(&mac).IsError()) {
            return std::nullopt;
        }
        return mac;
    }
    bool Finalize() override {
        return exporter->Finalize().IsSuccess();
    }

private:
    std::shared_ptr<st::Exporter> exporter;
};

class ImporterAdapter final : public tr::Importer {
public:
    explicit ImporterAdapter(std::shared_ptr<st::Importer> importer_)
        : importer{std::move(importer_)} {}
    tr::Aad ImportAad() override {
        return importer->Aad();
    }
    std::vector<u16> ChunkIds() override {
        return importer->ChunkIds();
    }
    bool Push(u16 id, std::span<const u8> bytes) override {
        return importer->Push(id, bytes).IsSuccess();
    }
    bool Finalize() override {
        return importer->Finalize().IsSuccess();
    }

private:
    std::shared_ptr<st::Importer> importer;
};

/// fs's transfer manager for one task (one challenge, one seed, as fs opens one per task).
class ManagerFs final : public tr::Fs {
public:
    ManagerFs(Core::System& system, const Common::UUID& user_, u64 save_data_id_)
        : manager{system}, user{user_}, save_data_id{save_data_id_} {}

    tr::Key Challenge() override {
        return manager.Challenge();
    }
    bool SetKeySeedPackage(std::span<const u8> package) override {
        const Result result = manager.SetKeySeedPackage(package);
        if (result.IsError()) {
            LOG_WARNING(Service_OLSC, "save data cloud: fs refused the key seed package ({:#x})",
                        result.raw);
        }
        return result.IsSuccess();
    }
    std::unique_ptr<tr::Exporter> OpenFullExporter() override {
        std::shared_ptr<st::Exporter> exporter;
        const Result result =
            manager.OpenFullExporter(&exporter, FileSys::SaveDataSpaceId::User, save_data_id);
        if (result.IsError()) {
            LOG_WARNING(Service_OLSC, "save data cloud: fs could not export the save ({:#x})",
                        result.raw);
            return nullptr;
        }
        return std::make_unique<ExporterAdapter>(std::move(exporter));
    }
    std::unique_ptr<tr::Exporter> OpenDiffExporter(std::span<const u8> meta) override {
        std::shared_ptr<st::Exporter> exporter;
        const Result result = manager.OpenDiffExporter(&exporter, meta,
                                                       FileSys::SaveDataSpaceId::User, save_data_id);
        if (result.IsError()) {
            LOG_WARNING(Service_OLSC, "save data cloud: fs could not diff-export the save ({:#x})",
                        result.raw);
            return nullptr;
        }
        return std::make_unique<ExporterAdapter>(std::move(exporter));
    }
    std::unique_ptr<tr::Importer> OpenImporter(std::span<const u8> meta) override {
        FileSys::UserId user_id{};
        std::memcpy(user_id.data(), user.uuid.data(), sizeof(user_id));
        std::shared_ptr<st::Importer> importer;
        const Result result =
            manager.OpenImporter(&importer, meta, user_id, FileSys::SaveDataSpaceId::User);
        if (result.IsError()) {
            LOG_WARNING(Service_OLSC, "save data cloud: fs could not open the import ({:#x})",
                        result.raw);
            return nullptr;
        }
        return std::make_unique<ImporterAdapter>(std::move(importer));
    }

private:
    st::TransferManager manager;
    Common::UUID user;
    u64 save_data_id;
};

/// The user's save of the title in the user space, as olsc finds it (fsp 61 + reader 0).
std::optional<FileSystem::ISaveDataInfoReader::SaveDataInfo> FindLocalSave(
    Core::System& system, const Common::UUID& user, u64 application_id) {
    const auto controller = system.GetFileSystemController().OpenSaveDataController();
    for (const auto& info :
         FileSystem::ISaveDataInfoReader::FindAllSaves(*controller, FileSys::SaveDataSpaceId::User)) {
        if (info.type == FileSys::SaveDataType::Account && info.title_id == application_id &&
            std::memcmp(info.user_id.data(), user.uuid.data(), 16) == 0) {
            return info;
        }
    }
    return std::nullopt;
}

} // namespace

TransferTaskInfo PackTaskInfo(const TransferTask& task) {
    TransferTaskInfo out{};
    std::memcpy(out.data(), &task.id, 8);
    out[0x08] = task.type;
    out[0x09] = task.running ? u8{1} : task.state;
    out[0x0a] = task.rank;
    std::memcpy(out.data() + 0x18, task.user.uuid.data(), 16);
    std::memcpy(out.data() + 0x28, &task.application_id, 8);
    return out;
}

bool MatchesFilter(const TransferTask& task, u8 filter) {
    switch (filter) {
    case 0:
        return task.type <= 1;
    case 1:
        return task.type >= 2;
    default:
        return true;
    }
}

std::optional<LocalSaveState> ReadLocalSave(Core::System& system, const Common::UUID& user,
                                            u64 application_id) {
    const auto controller = system.GetFileSystemController().OpenSaveDataController();
    FileSys::UserId user_id{};
    std::memcpy(user_id.data(), user.uuid.data(), sizeof(user_id));
    const auto attribute = FileSys::SaveDataAttribute::Make(
        application_id, FileSys::SaveDataType::Account, user_id, FileSys::InvalidSystemSaveDataId,
        0, FileSys::SaveDataRank::Primary);
    if (!controller->SaveDataExists(FileSys::SaveDataSpaceId::User, attribute)) {
        return std::nullopt;
    }
    FileSys::SaveDataExtraData extra{};
    if (controller->ReadSaveDataExtraData(&extra, FileSys::SaveDataSpaceId::User, attribute)
            .IsError()) {
        return std::nullopt;
    }
    LocalSaveState state{static_cast<u64>(extra.commit_id), extra.timestamp};
    if (state.commit_id == 0) {
        // As fs exports a save without a commit id (0x8d3f0): its extra data's digest. A console's
        // extra data never changes under a save, but Harbor makes up the extra data of a save it
        // holds none for, so the derived id is kept: newness compares it with the cloud's later.
        std::array<u8, 32> digest{};
        SHA256(reinterpret_cast<const u8*>(&extra), sizeof(extra), digest.data());
        std::memcpy(&state.commit_id, digest.data(), 8);
        extra.commit_id = static_cast<s64>(state.commit_id);
        if (extra.timestamp == 0) {
            // fs stamps every save it creates (and commits); one Harbor never stamped gets its
            // first time now, so its backups carry a saved time the HOME menu can show.
            extra.timestamp = static_cast<s64>(std::time(nullptr));
            state.timestamp = extra.timestamp;
        }
        controller->WriteSaveDataExtraData(FileSys::SaveDataSpaceId::User, attribute, extra);
    }
    return state;
}

namespace {

std::mutex queue_mutex;
std::weak_ptr<TransferQueue> current_queue;
Core::System* queue_owner = nullptr;

/// olsc!autonomy_registration_interval_seconds and
/// olsc!postponed_transfer_task_processing_interval_seconds (olsc-autonomy.md §7; the
/// firmware's settings data has the same 46800).
constexpr std::chrono::seconds AutonomyInterval{46800};
constexpr std::chrono::seconds PostponedInterval{46800};

/// The title's account saves of `user` in the user space (fsp 61 + reader 0).
std::vector<u64> AccountSaveTitles(Core::System& system, const Common::UUID& user) {
    std::vector<u64> titles;
    const auto controller = system.GetFileSystemController().OpenSaveDataController();
    for (const auto& info :
         FileSystem::ISaveDataInfoReader::FindAllSaves(*controller, FileSys::SaveDataSpaceId::User)) {
        if (info.type == FileSys::SaveDataType::Account &&
            std::memcmp(info.user_id.data(), user.uuid.data(), 16) == 0) {
            titles.push_back(info.title_id);
        }
    }
    return titles;
}

Result OlscResult(u32 description) {
    return Result{ErrorModule::OLSC, description};
}

} // namespace

std::shared_ptr<TransferQueue> TransferQueue::For(Core::System& system) {
    std::lock_guard lock{queue_mutex};
    auto queue = current_queue.lock();
    if (!queue || queue_owner != &system) {
        queue = std::make_shared<TransferQueue>(system);
        current_queue = queue;
        queue_owner = &system;
    }
    return queue;
}

std::shared_ptr<TransferQueue> TransferQueue::Existing(Core::System& system) {
    std::lock_guard lock{queue_mutex};
    return queue_owner == &system ? current_queue.lock() : nullptr;
}

void TransferQueue::OnApplicationStarted(Core::System& system, u64 application_id) {
    if (const auto queue = Existing(system)) {
        std::lock_guard lock{queue->mutex};
        queue->running_applications.insert(application_id);
    }
}

void TransferQueue::OnApplicationExited(Core::System& system, u64 application_id) {
    if (const auto queue = Existing(system)) {
        std::lock_guard lock{queue->mutex};
        queue->running_applications.erase(application_id);
        queue->exited_applications.push_back(application_id);
        queue->wake.notify_all();
    }
}

TransferQueue::TransferQueue(Core::System& system_) : system{system_} {
    thread = std::jthread([this](std::stop_token stop) { Run(stop); });
}

TransferQueue::~TransferQueue() {
    thread.request_stop();
    wake.notify_all();
}

std::shared_ptr<OpenPak::Olsc::DaemonSettings> TransferQueue::Settings() {
    std::lock_guard lock{mutex};
    if (!settings) {
        // A console keeps these in olsc's system save; Harbor keeps them beside the NAND saves.
        // Read on first use, once the data directory is the session's.
        settings = std::make_shared<OpenPak::Olsc::DaemonSettings>(
            Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir) / "system" / "harbor" /
            "olsc-settings.json");
    }
    return settings;
}

u64 TransferQueue::RegisterUpload(const Common::UUID& user, u64 application_id, bool flag,
                                  u8 rank) {
    std::lock_guard lock{mutex};
    TransferTask task;
    task.id = next_id++;
    task.user = user;
    task.application_id = application_id;
    task.type = 0;
    task.flag = flag ? 1 : 0;
    task.rank = rank;
    tasks.push_back(task);
    LOG_INFO(Service_OLSC, "save data cloud: upload of {:016X} queued (task {}, rank {:#x})",
             application_id, task.id, rank);
    wake.notify_all();
    return task.id;
}

std::optional<u64> TransferQueue::RegisterDownload(const Common::UUID& user, u64 archive_id,
                                                   u8 rank) {
    const auto archives = OpenPak::Olsc::ArchiveCache::Instance().Get(user.RawString());
    if (!archives) {
        return std::nullopt;
    }
    const auto archive = std::find_if(archives->begin(), archives->end(),
                                      [archive_id](const auto& a) { return a.id == archive_id; });
    if (archive == archives->end()) {
        return std::nullopt;
    }
    std::lock_guard lock{mutex};
    TransferTask task;
    task.id = next_id++;
    task.user = user;
    task.application_id = archive->application_id;
    task.type = 1;
    task.flag = 1;
    task.rank = rank;
    task.archive_id = archive_id;
    tasks.push_back(task);
    LOG_INFO(Service_OLSC, "save data cloud: download of {:016X} queued (task {}, rank {:#x})",
             task.application_id, task.id, rank);
    wake.notify_all();
    return task.id;
}

u8 TransferQueue::Newness(const Common::UUID& user, u64 application_id) {
    const std::string uid = user.RawString();
    const auto archives = OpenPak::Olsc::ArchiveCache::Instance().Get(uid);
    const auto cloud =
        archives ? OpenPak::Olsc::LatestFor(*archives, application_id) : std::nullopt;
    const auto local = ReadLocalSave(system, user, application_id);
    return OpenPak::Olsc::Newness(Settings()->Series(uid, application_id),
                                  local ? std::optional{local->commit_id} : std::nullopt, cloud);
}

void TransferQueue::Repair(const Common::UUID& user) {
    const std::string uid = user.RawString();
    const auto archives = OpenPak::Olsc::ArchiveCache::Instance().Get(uid);
    if (!archives) {
        return;
    }
    const auto daemon = Settings();
    for (const u64 title : AccountSaveTitles(system, user)) {
        const auto cloud = OpenPak::Olsc::LatestFor(*archives, title);
        const auto local = ReadLocalSave(system, user, title);
        const auto synced = daemon->Series(uid, title);
        if (!cloud || !local || cloud->commit_id != local->commit_id ||
            cloud->saved_at != static_cast<u64>(local->timestamp) ||
            (synced && synced->series_id != cloud->series_id) ||
            OpenPak::Olsc::Newness(synced, local->commit_id, cloud) != 3) {
            continue;
        }
        daemon->SetSeries(uid, title, {cloud->series_id, cloud->commit_id});
        LOG_INFO(Service_OLSC, "save data cloud: {:016X} is the cloud's archive, marked in sync",
                 title);
    }
}

Result TransferQueue::TryAutoUpload(const Common::UUID& user, u64 application_id, u8 rank) {
    const std::string uid = user.RawString();
    const auto daemon = Settings();
    if (!daemon->GlobalAutoUpload(uid)) {
        return OlscResult(1600);
    }
    if (!daemon->ApplicationAutoTransfer(uid, application_id)) {
        return OlscResult(1601);
    }
    // ponytail: the data transfer policy (2179-1602) is asked by the task itself (its policy
    // GET refuses with 2179-2001); add the policy cache when a title's policy can say no.
    const u8 newness = Newness(user, application_id);
    {
        std::lock_guard lock{mutex};
        for (auto it = tasks.begin(); it != tasks.end();) {
            const bool same = it->user == user && it->application_id == application_id &&
                              it->type == 0;
            if (same && (it->running || it->rank <= rank)) {
                return OlscResult(1603);
            }
            // A lower-priority task for the title gives way (0x1b7060).
            it = same && it->state == 0 ? tasks.erase(it) : std::next(it);
        }
    }
    switch (newness) {
    case 0:
        return OlscResult(1610);
    case 1:
        return OlscResult(1611);
    case 3:
        return OlscResult(1612);
    default:
        break;
    }
    RegisterUpload(user, application_id, false, rank);
    return ResultSuccess;
}

Result TransferQueue::TryAutoDownload(const Common::UUID& user, u64 application_id, u8 rank) {
    const std::string uid = user.RawString();
    const auto daemon = Settings();
    if (!daemon->GlobalAutoDownload(uid)) {
        return OlscResult(1600);
    }
    if (!daemon->ApplicationAutoTransfer(uid, application_id)) {
        return OlscResult(1601);
    }
    const auto archives = OpenPak::Olsc::ArchiveCache::Instance().Get(uid);
    const auto cloud =
        archives ? OpenPak::Olsc::LatestFor(*archives, application_id) : std::nullopt;
    if (!cloud) {
        return OlscResult(1200);
    }
    const u8 newness = Newness(user, application_id);
    {
        std::lock_guard lock{mutex};
        for (auto it = tasks.begin(); it != tasks.end();) {
            const bool same = it->user == user && it->application_id == application_id &&
                              it->type == 1;
            if (same && (it->running || it->rank <= rank)) {
                return OlscResult(1603);
            }
            it = same && it->state == 0 ? tasks.erase(it) : std::next(it);
        }
    }
    switch (newness) {
    case 0:
        return OlscResult(1610);
    case 2:
        return OlscResult(1613);
    case 3:
        return OlscResult(1612);
    default:
        break;
    }
    return RegisterDownload(user, cloud->id, rank) ? ResultSuccess : OlscResult(1200);
}

void TransferQueue::MakeDue(const Common::UUID& user) {
    std::lock_guard lock{mutex};
    next_sweep[user.RawString()] = Clock::now();
    wake.notify_all();
}

void TransferQueue::RemoveQueued(const Common::UUID& user, std::optional<u64> application_id,
                                 u8 type) {
    std::lock_guard lock{mutex};
    std::erase_if(tasks, [&](const TransferTask& t) {
        return !t.running && t.user == user && t.type == type &&
               (!application_id || t.application_id == *application_id);
    });
}

std::vector<TransferTask> TransferQueue::Tasks() const {
    std::lock_guard lock{mutex};
    return {tasks.begin(), tasks.end()};
}

void TransferQueue::Delete(u64 task_id) {
    std::lock_guard lock{mutex};
    std::erase_if(tasks, [task_id](const TransferTask& t) { return t.id == task_id && !t.running; });
}

void TransferQueue::Raise(u64 task_id) {
    std::lock_guard lock{mutex};
    const auto it = std::find_if(tasks.begin(), tasks.end(), [task_id](const TransferTask& t) {
        return t.id == task_id && !t.running;
    });
    if (it != tasks.end()) {
        TransferTask task = *it;
        tasks.erase(it);
        tasks.push_front(task);
    }
}

std::optional<TransferTask> TransferQueue::Current() const {
    std::lock_guard lock{mutex};
    for (const auto& task : tasks) {
        if (task.running) {
            return task;
        }
    }
    return std::nullopt;
}

void TransferQueue::AddStartEvent(std::shared_ptr<OpenPak::HostEvent> event) {
    std::lock_guard lock{mutex};
    start_events.push_back(event);
}

void TransferQueue::AddEndEvent(std::shared_ptr<OpenPak::HostEvent> event) {
    std::lock_guard lock{mutex};
    end_events.push_back(event);
}

void TransferQueue::Raise(std::vector<std::weak_ptr<OpenPak::HostEvent>>& events) {
    for (auto it = events.begin(); it != events.end();) {
        if (const auto event = it->lock()) {
            event->Raise();
            ++it;
        } else {
            it = events.erase(it);
        }
    }
}

std::deque<TransferTask>::iterator TransferQueue::NextLocked(Clock::time_point now) {
    // The basic queue first, then the postponed one at most once per interval (0x1b1c00); never
    // a title that is running (fs's transfer prohibiter, fsp 83).
    const auto runnable = [this](const TransferTask& t) {
        return t.state == 0 && !t.running && !running_applications.contains(t.application_id);
    };
    const auto basic = std::find_if(tasks.begin(), tasks.end(), [&](const TransferTask& t) {
        return runnable(t) && t.rank < RankPostponed;
    });
    if (basic != tasks.end() || now < postponed_gate) {
        return basic;
    }
    return std::find_if(tasks.begin(), tasks.end(), runnable);
}

TransferQueue::Clock::time_point TransferQueue::NextWakeLocked() const {
    auto wake_at = retry_at;
    for (const auto& [uid, due] : next_sweep) {
        wake_at = std::min(wake_at, due);
    }
    if (std::any_of(tasks.begin(), tasks.end(),
                    [](const TransferTask& t) { return t.state == 0 && t.rank >= RankPostponed; })) {
        wake_at = std::min(wake_at, postponed_gate);
    }
    return wake_at;
}

void TransferQueue::Autonomy() {
    std::vector<u64> exited;
    std::vector<Common::UUID> due;
    {
        std::lock_guard lock{mutex};
        exited.assign(exited_applications.begin(), exited_applications.end());
        exited_applications.clear();
    }
    const auto now = Clock::now();
    if (!OpenPak::Session::Enabled()) {
        // No console server (yet: the boot connection may still be under way). Look again later.
        std::lock_guard lock{mutex};
        retry_at = now + std::chrono::seconds{30};
        return;
    }
    const auto users = system.GetProfileManager().GetAllUsers();
    {
        std::lock_guard lock{mutex};
        retry_at = Clock::time_point::max();
        for (const auto& user : users) {
            if (!user.IsValid()) {
                continue;
            }
            // Every user is due at boot and when added (olsc-autonomy.md §1.2).
            const auto [it, added] = next_sweep.try_emplace(user.RawString(), now);
            if (it->second <= now) {
                due.push_back(user);
                it->second = now + AutonomyInterval;
            }
        }
    }
    const auto log_refusal = [](const char* what, u64 title, Result result) {
        if (result.IsError()) {
            LOG_DEBUG(Service_OLSC, "save data cloud: no automatic {} of {:016X} (2179-{:04})",
                      what, title, result.GetDescription());
        }
    };

    // An application's exit: an upload attempt for every user with that save, basic rank.
    for (const u64 title : exited) {
        for (const auto& user : users) {
            if (!user.IsValid() || !ReadLocalSave(system, user, title)) {
                continue;
            }
            const std::string uid = user.RawString();
            if (!OpenPak::Olsc::ArchiveCache::Instance().RefreshNow(uid)) {
                continue; // not linked or offline: the console's pre-step fails the same way
            }
            Repair(user);
            log_refusal("backup", title, TryAutoUpload(user, title, RankBasic));
        }
    }

    // The periodic sweep (0x1c8300): uploads, then downloads where the cloud has the title.
    for (const auto& user : due) {
        const std::string uid = user.RawString();
        if (!OpenPak::Olsc::ArchiveCache::Instance().RefreshNow(uid)) {
            continue;
        }
        Repair(user);
        const auto archives = OpenPak::Olsc::ArchiveCache::Instance().Get(uid);
        for (const u64 title : AccountSaveTitles(system, user)) {
            log_refusal("backup", title, TryAutoUpload(user, title, RankPostponed));
            if (archives && OpenPak::Olsc::LatestFor(*archives, title)) {
                log_refusal("download", title, TryAutoDownload(user, title, RankPostponed));
            }
        }
    }
}

void TransferQueue::Run(std::stop_token stop) {
    while (!stop.stop_requested()) {
        Autonomy();
        TransferTask task;
        {
            std::unique_lock lock{mutex};
            const auto next = NextLocked(Clock::now());
            if (next == tasks.end()) {
                wake.wait_until(lock, stop, NextWakeLocked(), [this] {
                    return !exited_applications.empty() || NextLocked(Clock::now()) != tasks.end();
                });
                continue;
            }
            if (next->rank >= RankPostponed) {
                postponed_gate = Clock::now() + PostponedInterval;
            }
            next->running = true;
            task = *next;
            Raise(start_events);
        }
        Execute(task);
        {
            std::lock_guard lock{mutex};
            auto it = std::find_if(tasks.begin(), tasks.end(),
                                   [&task](const TransferTask& t) { return t.id == task.id; });
            if (it != tasks.end()) {
                if (task.last_result == 0) {
                    tasks.erase(it);
                } else {
                    *it = task;
                    it->running = false;
                    it->state = 3;
                }
            }
            Raise(end_events);
        }
    }
}

void TransferQueue::Execute(TransferTask& task) {
    const std::string uid = task.user.RawString();
    SessionRest rest{uid};
    tr::Outcome outcome;
    if (task.type == 0) {
        const auto info = FindLocalSave(system, task.user, task.application_id);
        const auto local = ReadLocalSave(system, task.user, task.application_id);
        if (!info || !local) {
            outcome = {tr::Error::LocalSaveNotFound, 0, "no local save"};
        } else {
            const auto controller = system.GetFileSystemController().OpenSaveDataController();
            FileSys::UserId user_id{};
            std::memcpy(user_id.data(), task.user.uuid.data(), sizeof(user_id));
            const auto attribute = FileSys::SaveDataAttribute::Make(
                task.application_id, FileSys::SaveDataType::Account, user_id,
                FileSys::InvalidSystemSaveDataId, 0, FileSys::SaveDataRank::Primary);
            FileSys::SaveDataExtraData extra{};
            controller->ReadSaveDataExtraData(&extra, FileSys::SaveDataSpaceId::User, attribute);
            tr::LocalSave save;
            save.application_id = task.application_id;
            save.data_size = static_cast<s64>(info->save_image_size);
            LOG_DEBUG(Service_OLSC, "save data cloud: {:016X} data size {:#x}", task.application_id,
                      save.data_size);
            save.saved_at = local->timestamp;
            save.commit_id = local->commit_id;
            // GetSaveDataAcdIndex: extra data byte +0x55 (olsc-transfer.md §2.2 step 4).
            save.acd_index = reinterpret_cast<const u8*>(&extra)[0x55];
            save.launch_required_version = LaunchVersion(system, task.application_id);
            ManagerFs fs{system, task.user, info->save_id_unknown};
            outcome = tr::Upload(rest, fs, save, 0, &task.progress);
        }
    } else {
        const auto info = FindLocalSave(system, task.user, task.application_id);
        ManagerFs fs{system, task.user, info ? u64{info->save_id_unknown} : 0};
        outcome = tr::Download(rest, fs, task.application_id, task.archive_id,
                               LaunchVersion(system, task.application_id), &task.progress);
    }
    // The task's result as olsc keeps it: 2179-<description>; an HTTP failure without a server
    // code is 2155-<status> on a console (olsc-transfer.md §5). Fs failures carry fs's own
    // result on a console; Harbor reports them as 2179-9100 (logged with fs's result above).
    task.last_result = outcome.error == tr::Error::None
                           ? 0
                           : static_cast<u32>(Result{ErrorModule::OLSC,
                                                     static_cast<u32>(outcome.error)}.raw);
    // The ErrorHistoryDatabase (0x1b0530): a failure is the title's last error, a success clears it.
    if (outcome.error == tr::Error::None) {
        Settings()->RemoveLastError(uid, task.application_id);
    } else {
        Settings()->SetLastError(uid, task.application_id,
                                 {task.type, static_cast<s64>(std::time(nullptr)), task.last_result});
    }
    if (outcome.error == tr::Error::None) {
        LOG_INFO(Service_OLSC, "save data cloud: task {} done", task.id);
    } else {
        LOG_WARNING(Service_OLSC, "save data cloud: task {} failed, 2179-{:04}: {}", task.id,
                    static_cast<u32>(outcome.error), outcome.detail);
    }
    // The cloud changed: the archive list follows, and a finished transfer is the title's
    // SeriesInfo, the archive's (series, commit) (olsc-autonomy.md §4.2).
    OpenPak::Olsc::ArchiveCache::Instance().RefreshNow(uid);
    if (outcome.error == tr::Error::None) {
        bool listed = false;
        for (const auto& archive :
             OpenPak::Olsc::ArchiveCache::Instance().Get(uid).value_or(
                 std::vector<OpenPak::Olsc::ArchiveInfo>{})) {
            if (archive.id == outcome.archive_id) {
                Settings()->SetSeries(uid, task.application_id,
                                      {archive.series_id, archive.commit_id});
                listed = true;
            }
        }
        if (!listed) {
            LOG_WARNING(Service_OLSC, "save data cloud: archive {} not listed after the transfer",
                        outcome.archive_id);
        }
    }
    Repair(task.user);
}

} // namespace Service::OLSC

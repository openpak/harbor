// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include "core/hle/service/olsc/transfer_runner.h"

#include <algorithm>
#include <cstring>

#include "common/logging.h"
#include "common/redact.h"
#include "core/core.h"
#include "core/file_sys/fs_save_data_types.h"
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

/// The console protocol through Harbor's pinned client, as the task's user.
class SessionRest final : public tr::Rest {
public:
    explicit SessionRest(std::string uid_) : uid{std::move(uid_)} {}

    tr::HttpResponse Request(const std::string& method, const std::string& host,
                             const std::string& path, const std::string& body) override {
        // olsc POSTs JSON, some with an empty body; GETs carry none.
        const std::string type = method == "GET" ? "" : "application/json";
        const auto response = OpenPak::Session::SendAsUser(uid, method, host, path, type, body);
        return {response.status, response.body};
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
};

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

std::shared_ptr<TransferQueue> TransferQueue::For(Core::System& system) {
    static std::mutex mutex;
    static std::weak_ptr<TransferQueue> current;
    static Core::System* owner = nullptr;
    std::lock_guard lock{mutex};
    auto queue = current.lock();
    if (!queue || owner != &system) {
        queue = std::make_shared<TransferQueue>(system);
        current = queue;
        owner = &system;
    }
    return queue;
}

TransferQueue::TransferQueue(Core::System& system_) : system{system_} {
    thread = std::jthread([this](std::stop_token stop) { Run(stop); });
}

TransferQueue::~TransferQueue() {
    thread.request_stop();
    wake.notify_all();
}

u64 TransferQueue::RegisterUpload(const Common::UUID& user, u64 application_id, bool flag) {
    std::lock_guard lock{mutex};
    TransferTask task;
    task.id = next_id++;
    task.user = user;
    task.application_id = application_id;
    task.type = 0;
    task.flag = flag ? 1 : 0;
    tasks.push_back(task);
    LOG_INFO(Service_OLSC, "save data cloud: upload of {:016X} queued (task {})", application_id,
             task.id);
    wake.notify_all();
    return task.id;
}

std::optional<u64> TransferQueue::RegisterDownload(const Common::UUID& user, u64 archive_id) {
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
    task.archive_id = archive_id;
    tasks.push_back(task);
    LOG_INFO(Service_OLSC, "save data cloud: download of {:016X} queued (task {})",
             task.application_id, task.id);
    wake.notify_all();
    return task.id;
}

std::vector<TransferTask> TransferQueue::Tasks() const {
    std::lock_guard lock{mutex};
    return {tasks.begin(), tasks.end()};
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

void TransferQueue::Run(std::stop_token stop) {
    while (!stop.stop_requested()) {
        TransferTask task;
        {
            std::unique_lock lock{mutex};
            wake.wait(lock, stop, [this] {
                return std::any_of(tasks.begin(), tasks.end(),
                                   [](const TransferTask& t) { return t.state == 0; });
            });
            if (stop.stop_requested()) {
                return;
            }
            auto next = std::find_if(tasks.begin(), tasks.end(),
                                     [](const TransferTask& t) { return t.state == 0; });
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
        if (!info) {
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
            save.saved_at = extra.timestamp;
            save.commit_id = static_cast<u64>(extra.commit_id);
            save.acd_index = 0;
            save.launch_required_version = 0;
            ManagerFs fs{system, task.user, info->save_id_unknown};
            outcome = tr::Upload(rest, fs, save, 0, &task.progress);
        }
    } else {
        const auto info = FindLocalSave(system, task.user, task.application_id);
        ManagerFs fs{system, task.user, info ? u64{info->save_id_unknown} : 0};
        outcome = tr::Download(rest, fs, task.application_id, task.archive_id, &task.progress);
    }
    task.last_result = static_cast<u32>(outcome.error);
    if (outcome.error == tr::Error::None) {
        LOG_INFO(Service_OLSC, "save data cloud: task {} done", task.id);
    } else {
        LOG_WARNING(Service_OLSC, "save data cloud: task {} failed, 2179-{:04}: {}", task.id,
                    task.last_result, outcome.detail);
    }
    // The cloud changed: the archive list and newness follow.
    OpenPak::Olsc::ArchiveCache::Instance().RefreshNow(uid);
}

} // namespace Service::OLSC

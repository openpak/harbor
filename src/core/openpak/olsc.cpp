// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include "core/openpak/olsc.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstring>
#include <thread>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "common/logging.h"
#include "core/openpak/olsc_transfer.h"
#include "core/openpak/platform.h"
#include "core/openpak/session.h"

namespace OpenPak::Olsc {

namespace {

constexpr const char* StorageHost = "storage.hac.lp1.scsi.srv.nintendo.net";
constexpr const char* ArchivesPath = "/api/console/v3/save_data_archives";

std::int64_t Now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// A number the way the sysmodule reads it: a JSON integer, or (for the ids) a decimal string.
std::optional<std::uint64_t> Number(const nlohmann::json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end() || it->is_null()) {
        return std::nullopt;
    }
    if (it->is_number_unsigned()) {
        return it->get<std::uint64_t>();
    }
    if (it->is_number_integer()) {
        const auto value = it->get<std::int64_t>();
        return value < 0 ? std::nullopt : std::optional<std::uint64_t>(value);
    }
    if (it->is_string()) {
        const std::string text = it->get<std::string>();
        if (text.empty() || text.size() > 20 ||
            !std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            return std::nullopt;
        }
        std::uint64_t value{};
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        return parsed.ec == std::errc{} ? std::optional{value} : std::nullopt;
    }
    return std::nullopt;
}

/// The hex ids (application_id, nsa_id): 16 hex digits as a string, as the sysmodule parses them
/// (strtoull base 16).
std::optional<std::uint64_t> Hex(const nlohmann::json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string()) {
        return std::nullopt;
    }
    const std::string text = it->get<std::string>();
    if (text.empty() || text.size() > 16 ||
        !std::all_of(text.begin(), text.end(), [](char c) { return std::isxdigit(c) != 0; })) {
        return std::nullopt;
    }
    std::uint64_t value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    return parsed.ec == std::errc{} ? std::optional{value} : std::nullopt;
}

/// encoded_digest as the record check takes it: base64url without padding of exactly 32 bytes
/// (43 characters; 0x1de410 marks the key present only then).
bool IsDigest(const std::string& text) {
    return text.size() == 43 && std::all_of(text.begin(), text.end(), [](char c) {
               return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_';
           });
}

template <typename T>
void Put(DataInfo& out, std::size_t offset, T value) {
    std::memcpy(out.data() + offset, &value, sizeof(value));
}

} // namespace

static std::optional<ArchiveInfo> ParseArchive(const nlohmann::json& item);

BackupInfo PackBackupInfo(const ArchiveInfo& archive) {
    BackupInfo out{};
    std::memcpy(out.data(), &archive.id, 8);
    std::memcpy(out.data() + 8, &archive.finished_at, 8);
    std::memcpy(out.data() + 0x10, archive.desc.data(),
                std::min(archive.desc.size(), std::size_t{0x3ff}));
    return out;
}

std::optional<std::vector<BackupPermission>> ParseBackupPermissions(std::string_view body) {
    const auto json = nlohmann::json::parse(body, nullptr, false);
    if (!json.is_object() || !json.contains("permissions") || !json["permissions"].is_array())
        return std::nullopt;
    std::vector<BackupPermission> out;
    for (const auto& item : json["permissions"]) {
        if (!item.is_object())
            return std::nullopt;
        const auto id = Number(item, "id"), archive = Number(item, "save_data_archive_id");
        const auto nsa = Hex(item, "nsa_id"), app = Hex(item, "application_id");
        const auto created = Number(item, "created_at_as_unixtime");
        if (!id || !archive || !nsa || !app || !created || !*id || !*archive || !*nsa || !*app)
            return std::nullopt;
        out.push_back({*id, *archive, *nsa, *app, *created});
    }
    return out;
}

std::vector<ArchiveInfo> DownloadableBackups(const std::vector<ArchiveInfo>& archives,
                                             const std::vector<BackupPermission>& permissions,
                                             std::uint64_t application_id, std::uint64_t nsa_id) {
    std::vector<ArchiveInfo> out;
    for (const auto& archive : archives) {
        if (archive.datatype != 3 || archive.status != StatusFixed ||
            archive.application_id != application_id || archive.nsa_id != nsa_id)
            continue;
        const bool granted =
            std::any_of(permissions.begin(), permissions.end(), [&](const auto& p) {
                return p.archive_id == archive.id && p.application_id == application_id &&
                       p.nsa_id == nsa_id;
            });
        if (granted)
            out.push_back(archive);
    }
    return out;
}

std::optional<std::vector<ArchiveInfo>> ParseArchiveList(std::string_view body) {
    const auto json = nlohmann::json::parse(body, nullptr, false);
    if (!json.is_object()) {
        return std::nullopt;
    }
    const auto list = json.find("save_data_archives");
    if (list == json.end() || !list->is_array()) {
        return std::nullopt;
    }

    std::vector<ArchiveInfo> archives;
    for (const auto& item : *list) {
        auto archive = ParseArchive(item);
        if (!archive) {
            LOG_WARNING(Service_OLSC,
                        "save data cloud: archive record {} is incomplete; the list is refused "
                        "(2179-3201 on a console)",
                        archives.size());
            return std::nullopt;
        }
        archives.push_back(*archive);
    }
    return archives;
}

static std::optional<ArchiveInfo> ParseArchive(const nlohmann::json& item) {
    if (!item.is_object()) {
        return std::nullopt;
    }
    // Keys the record check (0x1dd580) requires; a JSON null counts only for the three the null
    // handler (0x1dd760) accepts.
    static constexpr std::array required{"id",
                                         "nsa_id",
                                         "application_id",
                                         "device_id",
                                         "datatype",
                                         "data_size",
                                         "status",
                                         "device_serial",
                                         "num_of_partitions",
                                         "launch_required_version",
                                         "series_id",
                                         "encoded_digest",
                                         "saved_at_as_unixtime"};
    static constexpr std::array nullable{"desc", "previous_save_data_archive_id",
                                         "finished_at_as_unixtime"};
    for (const char* key : required) {
        const auto it = item.find(key);
        if (it == item.end() || it->is_null()) {
            return std::nullopt;
        }
    }
    for (const char* key : nullable) {
        if (!item.contains(key)) {
            return std::nullopt;
        }
    }
    const auto id = Number(item, "id");
    const auto application_id = Hex(item, "application_id");
    const auto status = item.find("status");
    const auto digest = item.find("encoded_digest");
    if (!id || !application_id || !status->is_string() || !digest->is_string() ||
        !IsDigest(digest->get<std::string>())) {
        return std::nullopt;
    }
    ArchiveInfo archive;
    if (*status == "fixed") {
        archive.status = StatusFixed;
    } else if (*status == "uploading") {
        archive.status = StatusUploading;
    } else {
        return std::nullopt;
    }
    archive.id = *id;
    archive.application_id = *application_id;
    archive.nsa_id = Hex(item, "nsa_id").value_or(0);
    archive.data_size = Number(item, "data_size").value_or(0);
    archive.datatype = static_cast<std::uint8_t>(Number(item, "datatype").value_or(0));
    archive.num_of_partitions =
        static_cast<std::uint32_t>(Number(item, "num_of_partitions").value_or(0));
    archive.launch_required_version =
        static_cast<std::uint32_t>(Number(item, "launch_required_version").value_or(0));
    archive.previous_id = Number(item, "previous_save_data_archive_id").value_or(0);
    archive.series_id = Number(item, "series_id").value_or(0);
    if (const auto digest_bytes = Transfer::FromBase64Url(digest->get<std::string>());
        digest_bytes && digest_bytes->size() >= 8) {
        std::memcpy(&archive.commit_id, digest_bytes->data(), 8);
    }
    archive.saved_at = Number(item, "saved_at_as_unixtime").value_or(0);
    archive.finished_at = Number(item, "finished_at_as_unixtime").value_or(0);
    archive.platform_key_generation =
        static_cast<std::uint8_t>(Number(item, "platform_key_generation").value_or(0));
    archive.acd_index = static_cast<std::uint8_t>(Number(item, "acd_index").value_or(0));
    if (const auto desc = item.find("desc"); desc != item.end() && desc->is_string())
        archive.desc = desc->get<std::string>();
    return archive;
}

DataInfo PackDataInfo(const ArchiveInfo& archive) {
    DataInfo out{};
    Put(out, 0x00, archive.id);
    Put(out, 0x08, archive.application_id);
    Put(out, 0x10, archive.status);
    Put(out, 0x18, archive.data_size);
    Put(out, 0x20, archive.saved_at);
    Put(out, 0x28, archive.finished_at);
    Put(out, 0x30, archive.acd_index);
    return out;
}

std::optional<ArchiveInfo> LatestFor(const std::vector<ArchiveInfo>& archives,
                                     std::uint64_t application_id) {
    std::optional<ArchiveInfo> latest;
    for (const auto& archive : archives) {
        if (archive.application_id != application_id || archive.status != StatusFixed) {
            continue;
        }
        if (!latest || archive.saved_at > latest->saved_at ||
            (archive.saved_at == latest->saved_at && archive.id > latest->id)) {
            latest = archive;
        }
    }
    return latest;
}

std::vector<ArchiveInfo> LatestPerApplication(const std::vector<ArchiveInfo>& archives) {
    std::map<std::uint64_t, ArchiveInfo> latest;
    for (const auto& archive : archives) {
        if (latest.contains(archive.application_id)) {
            continue;
        }
        if (auto found = LatestFor(archives, archive.application_id)) {
            latest.emplace(archive.application_id, *found);
        }
    }
    std::vector<ArchiveInfo> out;
    for (auto& [application_id, archive] : latest) {
        out.push_back(archive);
    }
    std::stable_sort(out.begin(), out.end(), [](const ArchiveInfo& a, const ArchiveInfo& b) {
        return a.saved_at > b.saved_at;
    });
    return out;
}

ArchiveCache& ArchiveCache::Instance() {
    static ArchiveCache cache;
    return cache;
}

void ArchiveCache::Refresh(const std::string& uid, int max_age_s, std::uint8_t datatype) {
    const auto key = datatype == 0 ? uid : uid + "#" + std::to_string(datatype);
    if (uid.empty() || !Session::Enabled() || !Session::HasDeviceAccount(uid)) {
        return;
    }
    {
        std::lock_guard lock{mutex};
        Entry& entry = entries[key];
        if (entry.in_flight || (entry.fetched && Now() - entry.fetched_at < max_age_s)) {
            return;
        }
        entry.in_flight = true;
    }
    std::thread([this, uid, key, datatype] {
        auto archives = FetchArchives(uid, datatype);
        std::lock_guard lock{mutex};
        Entry& entry = entries[key];
        entry.in_flight = false;
        if (archives) {
            entry.archives = std::move(*archives);
            entry.fetched = true;
            entry.fetched_at = Now();
            SaveLocked();
        }
    }).detach();
}

bool ArchiveCache::RefreshNow(const std::string& uid, std::uint8_t datatype) {
    if (uid.empty() || !Session::Enabled() || !Session::HasDeviceAccount(uid)) {
        return false;
    }
    auto archives = FetchArchives(uid, datatype);
    if (!archives) {
        return false;
    }
    Put(uid, std::move(*archives), datatype);
    return true;
}

std::optional<std::vector<ArchiveInfo>> ArchiveCache::Get(const std::string& uid,
                                                          std::uint8_t datatype) const {
    const auto key = datatype == 0 ? uid : uid + "#" + std::to_string(datatype);
    std::lock_guard lock{mutex};
    const auto it = entries.find(key);
    if (it == entries.end() || !it->second.fetched) {
        return std::nullopt;
    }
    return it->second.archives;
}

void ArchiveCache::Put(const std::string& uid, std::vector<ArchiveInfo> archives,
                       std::uint8_t datatype) {
    const auto key = datatype == 0 ? uid : uid + "#" + std::to_string(datatype);
    std::lock_guard lock{mutex};
    Entry& entry = entries[key];
    entry.archives = std::move(archives);
    entry.fetched = true;
    entry.fetched_at = Now();
    SaveLocked();
}

void ArchiveCache::Clear() {
    std::lock_guard lock{mutex};
    entries.clear();
    store.clear();
}

void ArchiveCache::SetStore(const std::filesystem::path& file) {
    std::lock_guard lock{mutex};
    if (file == store) {
        return;
    }
    entries.clear();
    store = file;
    LoadLocked();
}

void ArchiveCache::LoadLocked() {
    const std::string text = Platform::ReadFile(store);
    if (text.empty()) {
        return;
    }
    const auto json = nlohmann::json::parse(text, nullptr, false);
    const auto users = json.is_object() ? json.find("users") : json.end();
    if (users == json.end() || !users->is_object()) {
        LOG_WARNING(Service_OLSC, "save data cloud: the stored archive lists are unreadable");
        return;
    }
    for (const auto& [uid, value] : users->items()) {
        if (!value.is_object()) {
            continue;
        }
        const auto archives = value.find("archives");
        auto list = archives != value.end() ? DeserializeArchives(archives->dump()) : std::nullopt;
        if (!list) {
            continue;
        }
        Entry& entry = entries[uid];
        entry.archives = std::move(*list);
        entry.fetched = true;
        // Kept, not fetched in this session: the next Refresh fetches again.
        entry.fetched_at = 0;
    }
    LOG_DEBUG(Service_OLSC, "save data cloud: stored archive lists for {} user(s)", entries.size());
}

void ArchiveCache::SaveLocked() const {
    if (store.empty()) {
        return;
    }
    nlohmann::json users = nlohmann::json::object();
    for (const auto& [uid, entry] : entries) {
        if (entry.fetched) {
            users[uid] = {{"archives", nlohmann::json::parse(SerializeArchives(entry.archives))}};
        }
    }
    const nlohmann::json json = {{"version", 1}, {"users", users}};
    if (!Platform::CreateParentDirs(store) || !Platform::WriteAtomically(store, json.dump(2))) {
        LOG_WARNING(Service_OLSC, "save data cloud: the archive lists could not be stored");
    }
}

std::string SerializeArchives(const std::vector<ArchiveInfo>& archives) {
    nlohmann::json list = nlohmann::json::array();
    for (const auto& a : archives) {
        list.push_back({{"id", a.id},
                        {"nsa_id", a.nsa_id},
                        {"application_id", a.application_id},
                        {"data_size", a.data_size},
                        {"status", a.status},
                        {"datatype", a.datatype},
                        {"num_of_partitions", a.num_of_partitions},
                        {"launch_required_version", a.launch_required_version},
                        {"previous_id", a.previous_id},
                        {"series_id", a.series_id},
                        {"commit_id", a.commit_id},
                        {"saved_at", a.saved_at},
                        {"finished_at", a.finished_at},
                        {"platform_key_generation", a.platform_key_generation},
                        {"acd_index", a.acd_index},
                        {"desc", a.desc}});
    }
    return list.dump();
}

std::optional<std::vector<ArchiveInfo>> DeserializeArchives(std::string_view text) {
    const auto list = nlohmann::json::parse(text, nullptr, false);
    if (!list.is_array()) {
        return std::nullopt;
    }
    std::vector<ArchiveInfo> archives;
    for (const auto& item : list) {
        if (!item.is_object()) {
            return std::nullopt;
        }
        const auto u64 = [&item](const char* key) -> std::uint64_t {
            const auto it = item.find(key);
            return it != item.end() && it->is_number_unsigned() ? it->get<std::uint64_t>() : 0;
        };
        ArchiveInfo a;
        a.id = u64("id");
        a.nsa_id = u64("nsa_id");
        a.application_id = u64("application_id");
        a.data_size = u64("data_size");
        a.status = static_cast<std::uint8_t>(u64("status"));
        a.datatype = static_cast<std::uint8_t>(u64("datatype"));
        a.num_of_partitions = static_cast<std::uint32_t>(u64("num_of_partitions"));
        a.launch_required_version = static_cast<std::uint32_t>(u64("launch_required_version"));
        a.previous_id = u64("previous_id");
        a.series_id = u64("series_id");
        a.commit_id = u64("commit_id");
        a.saved_at = u64("saved_at");
        a.finished_at = u64("finished_at");
        a.platform_key_generation = static_cast<std::uint8_t>(u64("platform_key_generation"));
        a.acd_index = static_cast<std::uint8_t>(u64("acd_index"));
        if (const auto desc = item.find("desc"); desc != item.end() && desc->is_string())
            a.desc = desc->get<std::string>();
        if (a.id == 0 || a.application_id == 0 ||
            (a.status != StatusFixed && a.status != StatusUploading)) {
            continue;
        }
        archives.push_back(a);
    }
    return archives;
}

std::optional<std::vector<ArchiveInfo>> FetchArchives(const std::string& uid,
                                                      std::uint8_t datatype) {
    const Session::Response response =
        Session::SendAsUser(uid, "GET", StorageHost,
                            std::string{ArchivesPath} + "?datatype=" + std::to_string(datatype) +
                                (datatype == 3 ? "&device_id=me" : ""),
                            {}, {});
    if (response.status != 200) {
        LOG_INFO(Service_OLSC, "save data cloud: archive list not fetched (HTTP {})",
                 response.status);
        return std::nullopt;
    }
    auto archives = ParseArchiveList(response.body);
    if (!archives) {
        LOG_WARNING(Service_OLSC, "save data cloud: archive list refused; the cached list stays");
        return std::nullopt;
    }
    LOG_INFO(Service_OLSC, "save data cloud: {} archive(s) listed", archives->size());
    return archives;
}

DaemonSettings::DaemonSettings(std::filesystem::path file_) : file{std::move(file_)} {
    Load();
}

std::uint8_t Newness(const std::optional<SeriesInfo>& synced,
                     const std::optional<std::uint64_t>& local_commit,
                     const std::optional<ArchiveInfo>& cloud) {
    if (!cloud) {
        return local_commit ? 2 : 3;
    }
    if (!local_commit) {
        return 1;
    }
    const SeriesInfo si = synced.value_or(SeriesInfo{});
    if (cloud->series_id == si.series_id && cloud->commit_id == si.commit_id) {
        return *local_commit == si.commit_id ? 0 : 2;
    }
    return *local_commit == si.commit_id && cloud->series_id == si.series_id ? 1 : 3;
}

bool DaemonSettings::GlobalAutoUpload(const std::string& uid) const {
    std::lock_guard lock{mutex};
    const auto it = users.find(uid);
    return it != users.end() ? it->second.auto_upload.value_or(DefaultAutoUpload)
                             : DefaultAutoUpload;
}

std::optional<DaemonSettings::BackupSetting> DaemonSettings::Backup(const std::string& uid,
                                                                    std::uint64_t app) const {
    std::lock_guard lock{mutex};
    const auto user = users.find(uid);
    if (user == users.end())
        return std::nullopt;
    const auto entry = user->second.backups.find(app);
    return entry == user->second.backups.end() ? std::nullopt : std::optional{entry->second};
}

void DaemonSettings::SetBackup(const std::string& uid, std::uint64_t app, bool enabled) {
    std::lock_guard lock{mutex};
    auto& entries = users[uid].backups;
    std::uint64_t touched = 0;
    for (const auto& [id, value] : entries)
        touched = std::max(touched, value.touched);
    if (!entries.contains(app) && entries.size() >= 16) {
        const auto oldest =
            std::min_element(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
                return a.second.touched < b.second.touched;
            });
        entries.erase(oldest);
    }
    auto& entry = entries[app];
    entry.enabled = enabled;
    entry.touched = touched + 1;
    Save();
}

bool DaemonSettings::SetBackupCustomData(const std::string& uid, std::uint64_t app,
                                         std::string desc) {
    std::lock_guard lock{mutex};
    auto& entries = users[uid].backups;
    const auto entry = entries.find(app);
    if (entry == entries.end() || desc.size() >= 0x400)
        return false;
    entry->second.desc = std::move(desc);
    Save();
    return true;
}

void DaemonSettings::RemoveBackup(const std::string& uid, std::uint64_t app) {
    std::lock_guard lock{mutex};
    const auto user = users.find(uid);
    if (user != users.end() && user->second.backups.erase(app))
        Save();
}

std::vector<std::uint64_t> DaemonSettings::EnabledBackups(const std::string& uid) const {
    std::lock_guard lock{mutex};
    std::vector<std::uint64_t> out;
    const auto user = users.find(uid);
    if (user != users.end()) {
        for (const auto& [app, record] : user->second.backups)
            if (record.enabled)
                out.push_back(app);
    }
    return out;
}

void DaemonSettings::SetGlobalAutoUpload(const std::string& uid, bool enabled) {
    std::lock_guard lock{mutex};
    users[uid].auto_upload = enabled;
    Save();
}

bool DaemonSettings::GlobalAutoDownload(const std::string& uid) const {
    std::lock_guard lock{mutex};
    const auto it = users.find(uid);
    return it != users.end() ? it->second.auto_download.value_or(DefaultAutoDownload)
                             : DefaultAutoDownload;
}

void DaemonSettings::SetGlobalAutoDownload(const std::string& uid, bool enabled) {
    std::lock_guard lock{mutex};
    users[uid].auto_download = enabled;
    Save();
}

bool DaemonSettings::ApplicationAutoTransfer(const std::string& uid,
                                             std::uint64_t application_id) const {
    {
        std::lock_guard lock{mutex};
        const auto it = users.find(uid);
        if (it != users.end()) {
            const auto app = it->second.auto_transfer.find(application_id);
            if (app != it->second.auto_transfer.end()) {
                return app->second;
            }
        }
    }
    // A title without its own entry follows the user's global settings (0x1ce4c0).
    return GlobalAutoUpload(uid) || GlobalAutoDownload(uid);
}

void DaemonSettings::SetApplicationAutoTransfer(const std::string& uid,
                                                std::uint64_t application_id, bool enabled) {
    std::lock_guard lock{mutex};
    users[uid].auto_transfer[application_id] = enabled;
    Save();
}

std::optional<SeriesInfo> DaemonSettings::Series(const std::string& uid,
                                                 std::uint64_t application_id) const {
    std::lock_guard lock{mutex};
    const auto it = users.find(uid);
    if (it == users.end()) {
        return std::nullopt;
    }
    const auto app = it->second.series.find(application_id);
    return app != it->second.series.end() ? std::optional{app->second} : std::nullopt;
}

void DaemonSettings::SetSeries(const std::string& uid, std::uint64_t application_id,
                               SeriesInfo info) {
    std::lock_guard lock{mutex};
    users[uid].series[application_id] = info;
    Save();
}

std::optional<DaemonSettings::LastError> DaemonSettings::LastErrorOf(
    const std::string& uid, std::uint64_t application_id) const {
    std::lock_guard lock{mutex};
    const auto it = users.find(uid);
    if (it == users.end()) {
        return std::nullopt;
    }
    const auto app = it->second.errors.find(application_id);
    return app != it->second.errors.end() ? std::optional{app->second} : std::nullopt;
}

void DaemonSettings::SetLastError(const std::string& uid, std::uint64_t application_id,
                                  LastError error) {
    std::lock_guard lock{mutex};
    users[uid].errors[application_id] = error;
    Save();
}

void DaemonSettings::RemoveLastError(const std::string& uid, std::uint64_t application_id) {
    std::lock_guard lock{mutex};
    const auto it = users.find(uid);
    if (it != users.end() && it->second.errors.erase(application_id) != 0) {
        Save();
    }
}

void DaemonSettings::Load() {
    const std::string text = Platform::ReadFile(file);
    if (text.empty()) {
        LOG_DEBUG(Service_OLSC, "olsc settings: none stored yet");
        return;
    }
    const auto json = nlohmann::json::parse(text, nullptr, false);
    const auto list = json.is_object() ? json.find("users") : json.end();
    if (list == json.end() || !list->is_object()) {
        LOG_WARNING(Service_OLSC, "olsc settings unreadable, starting from defaults");
        return;
    }
    for (const auto& [uid, value] : list->items()) {
        if (!value.is_object()) {
            continue;
        }
        User user;
        if (const auto apps = value.find("backups"); apps != value.end() && apps->is_object()) {
            for (const auto& [app, entry] : apps->items()) {
                if (user.backups.size() >= 16)
                    break;
                if (!entry.is_object() || app.empty() || app.size() > 16 ||
                    !std::all_of(app.begin(), app.end(), [](char c) { return std::isxdigit(c); }))
                    continue;
                const auto desc = entry.value("desc", std::string{});
                if (desc.size() >= 0x400)
                    continue;
                user.backups[std::stoull(app, nullptr, 16)] = {
                    entry.value("enabled", false), desc, entry.value("touched", std::uint64_t{0})};
            }
        }
        if (const auto v = value.find("auto_upload"); v != value.end() && v->is_boolean()) {
            user.auto_upload = v->get<bool>();
        }
        if (const auto v = value.find("auto_download"); v != value.end() && v->is_boolean()) {
            user.auto_download = v->get<bool>();
        }
        if (const auto apps = value.find("errors"); apps != value.end() && apps->is_object()) {
            for (const auto& [app, entry] : apps->items()) {
                if (entry.is_object() && !app.empty() && app.size() <= 16 &&
                    std::all_of(app.begin(), app.end(),
                                [](char c) { return std::isxdigit(c) != 0; })) {
                    user.errors[std::stoull(app, nullptr, 16)] = {
                        entry.value("task_type", std::uint8_t{0}),
                        entry.value("time", std::int64_t{0}),
                        entry.value("result", std::uint32_t{0})};
                }
            }
        }
        if (const auto apps = value.find("series"); apps != value.end() && apps->is_object()) {
            for (const auto& [app, pair] : apps->items()) {
                if (pair.is_array() && pair.size() == 2 && pair[0].is_number_unsigned() &&
                    pair[1].is_number_unsigned() && !app.empty() && app.size() <= 16 &&
                    std::all_of(app.begin(), app.end(),
                                [](char c) { return std::isxdigit(c) != 0; })) {
                    user.series[std::stoull(app, nullptr, 16)] = {pair[0].get<std::uint64_t>(),
                                                                  pair[1].get<std::uint64_t>()};
                }
            }
        }
        if (const auto apps = value.find("auto_transfer");
            apps != value.end() && apps->is_object()) {
            for (const auto& [app, enabled] : apps->items()) {
                if (enabled.is_boolean() && !app.empty() && app.size() <= 16 &&
                    std::all_of(app.begin(), app.end(),
                                [](char c) { return std::isxdigit(c) != 0; })) {
                    user.auto_transfer[std::stoull(app, nullptr, 16)] = enabled.get<bool>();
                }
            }
        }
        users[uid] = std::move(user);
    }
    LOG_DEBUG(Service_OLSC, "olsc settings: {} user(s) loaded", users.size());
}

void DaemonSettings::Save() const {
    nlohmann::json list = nlohmann::json::object();
    for (const auto& [uid, user] : users) {
        nlohmann::json apps = nlohmann::json::object();
        for (const auto& [app, enabled] : user.auto_transfer) {
            apps[fmt::format("{:016x}", app)] = enabled;
        }
        nlohmann::json series = nlohmann::json::object();
        for (const auto& [app, info] : user.series) {
            series[fmt::format("{:016x}", app)] = {info.series_id, info.commit_id};
        }
        nlohmann::json errors = nlohmann::json::object();
        for (const auto& [app, error] : user.errors) {
            errors[fmt::format("{:016x}", app)] = {
                {"task_type", error.task_type}, {"time", error.time}, {"result", error.result}};
        }
        nlohmann::json entry = {{"auto_transfer", apps}, {"series", series}, {"errors", errors}};
        nlohmann::json backups = nlohmann::json::object();
        for (const auto& [app, record] : user.backups) {
            backups[fmt::format("{:016x}", app)] = {
                {"enabled", record.enabled}, {"desc", record.desc}, {"touched", record.touched}};
        }
        entry["backups"] = std::move(backups);
        if (user.auto_upload) {
            entry["auto_upload"] = *user.auto_upload;
        }
        if (user.auto_download) {
            entry["auto_download"] = *user.auto_download;
        }
        list[uid] = entry;
    }
    const nlohmann::json json = {{"version", 1}, {"users", list}};
    if (!Platform::CreateParentDirs(file) || !Platform::WriteAtomically(file, json.dump(2))) {
        LOG_WARNING(Service_OLSC, "olsc settings could not be written");
    }
}

} // namespace OpenPak::Olsc

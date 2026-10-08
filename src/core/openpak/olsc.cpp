// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include "core/openpak/olsc.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <thread>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "common/logging.h"
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
        return std::stoull(text);
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
    return std::stoull(text, nullptr, 16);
}

template <typename T>
void Put(DataInfo& out, std::size_t offset, T value) {
    std::memcpy(out.data() + offset, &value, sizeof(value));
}

} // namespace

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
        if (!item.is_object()) {
            continue;
        }
        const auto id = Number(item, "id");
        const auto application_id = Hex(item, "application_id");
        const auto status = item.find("status");
        if (!id || !application_id || status == item.end() || !status->is_string()) {
            continue;
        }
        ArchiveInfo archive;
        if (*status == "fixed") {
            archive.status = StatusFixed;
        } else if (*status == "uploading") {
            archive.status = StatusUploading;
        } else {
            continue;
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
        archive.saved_at = Number(item, "saved_at_as_unixtime").value_or(0);
        archive.finished_at = Number(item, "finished_at_as_unixtime").value_or(0);
        archive.platform_key_generation =
            static_cast<std::uint8_t>(Number(item, "platform_key_generation").value_or(0));
        archive.acd_index = static_cast<std::uint8_t>(Number(item, "acd_index").value_or(0));
        archives.push_back(archive);
    }
    return archives;
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

void ArchiveCache::Refresh(const std::string& uid, int max_age_s) {
    if (uid.empty() || !Session::Enabled() || !Session::HasDeviceAccount(uid)) {
        return;
    }
    {
        std::lock_guard lock{mutex};
        Entry& entry = entries[uid];
        if (entry.in_flight || (entry.fetched && Now() - entry.fetched_at < max_age_s)) {
            return;
        }
        entry.in_flight = true;
    }
    std::thread([this, uid] {
        auto archives = FetchArchives(uid);
        std::lock_guard lock{mutex};
        Entry& entry = entries[uid];
        entry.in_flight = false;
        if (archives) {
            entry.archives = std::move(*archives);
            entry.fetched = true;
            entry.fetched_at = Now();
            SaveLocked();
        }
    }).detach();
}

bool ArchiveCache::RefreshNow(const std::string& uid) {
    if (uid.empty() || !Session::Enabled() || !Session::HasDeviceAccount(uid)) {
        return false;
    }
    auto archives = FetchArchives(uid);
    if (!archives) {
        return false;
    }
    Put(uid, std::move(*archives));
    return true;
}

std::optional<std::vector<ArchiveInfo>> ArchiveCache::Get(const std::string& uid) const {
    std::lock_guard lock{mutex};
    const auto it = entries.find(uid);
    if (it == entries.end() || !it->second.fetched) {
        return std::nullopt;
    }
    return it->second.archives;
}

void ArchiveCache::Put(const std::string& uid, std::vector<ArchiveInfo> archives) {
    std::lock_guard lock{mutex};
    Entry& entry = entries[uid];
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
                        {"saved_at", a.saved_at},
                        {"finished_at", a.finished_at},
                        {"platform_key_generation", a.platform_key_generation},
                        {"acd_index", a.acd_index}});
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
        a.saved_at = u64("saved_at");
        a.finished_at = u64("finished_at");
        a.platform_key_generation = static_cast<std::uint8_t>(u64("platform_key_generation"));
        a.acd_index = static_cast<std::uint8_t>(u64("acd_index"));
        if (a.id == 0 || a.application_id == 0 ||
            (a.status != StatusFixed && a.status != StatusUploading)) {
            continue;
        }
        archives.push_back(a);
    }
    return archives;
}

std::optional<std::vector<ArchiveInfo>> FetchArchives(const std::string& uid) {
    const Session::Response response =
        Session::SendAsUser(uid, "GET", StorageHost, ArchivesPath, {}, {});
    if (response.status != 200) {
        LOG_INFO(Service_OLSC, "save data cloud: archive list not fetched (HTTP {})",
                 response.status);
        return std::nullopt;
    }
    auto archives = ParseArchiveList(response.body);
    if (!archives) {
        LOG_WARNING(Service_OLSC, "save data cloud: the archive list reply is not a list");
        return std::nullopt;
    }
    LOG_INFO(Service_OLSC, "save data cloud: {} archive(s) listed", archives->size());
    return archives;
}

DaemonSettings::DaemonSettings(std::filesystem::path file_) : file{std::move(file_)} {
    Load();
}

bool DaemonSettings::GlobalAutoUpload(const std::string& uid) const {
    std::lock_guard lock{mutex};
    const auto it = users.find(uid);
    return it != users.end() && it->second.auto_upload;
}

void DaemonSettings::SetGlobalAutoUpload(const std::string& uid, bool enabled) {
    std::lock_guard lock{mutex};
    users[uid].auto_upload = enabled;
    Save();
}

bool DaemonSettings::GlobalAutoDownload(const std::string& uid) const {
    std::lock_guard lock{mutex};
    const auto it = users.find(uid);
    return it != users.end() && it->second.auto_download;
}

void DaemonSettings::SetGlobalAutoDownload(const std::string& uid, bool enabled) {
    std::lock_guard lock{mutex};
    users[uid].auto_download = enabled;
    Save();
}

bool DaemonSettings::ApplicationAutoTransfer(const std::string& uid,
                                             std::uint64_t application_id) const {
    std::lock_guard lock{mutex};
    const auto it = users.find(uid);
    if (it == users.end()) {
        return false;
    }
    const auto app = it->second.auto_transfer.find(application_id);
    return app != it->second.auto_transfer.end() && app->second;
}

void DaemonSettings::SetApplicationAutoTransfer(const std::string& uid,
                                                std::uint64_t application_id, bool enabled) {
    std::lock_guard lock{mutex};
    users[uid].auto_transfer[application_id] = enabled;
    Save();
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
        user.auto_upload = value.value("auto_upload", false);
        user.auto_download = value.value("auto_download", false);
        if (const auto apps = value.find("auto_transfer"); apps != value.end() && apps->is_object()) {
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
        list[uid] = {{"auto_upload", user.auto_upload},
                     {"auto_download", user.auto_download},
                     {"auto_transfer", apps}};
    }
    const nlohmann::json json = {{"version", 1}, {"users", list}};
    if (!Platform::CreateParentDirs(file) || !Platform::WriteAtomically(file, json.dump(2))) {
        LOG_WARNING(Service_OLSC, "olsc settings could not be written");
    }
}

} // namespace OpenPak::Olsc

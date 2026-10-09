// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Save Data Cloud for the olsc service: what the olsc sysmodule keeps and asks on a console.
//
// - The daemon's settings (automatic upload and download per user, automatic transfer per user
//   and title), which a console keeps in olsc's own system save, kept in one file.
// - The user's archive list from the console protocol's storage host
//   (storage.hac.lp1.scsi.srv.nintendo.net, served by OpenPak), as the sysmodule's
//   SaveDataArchiveInfo cache, and the DataInfo record IRemoteStorageController hands out.
//
// Read-only: nothing here uploads, downloads or deletes an archive.
// Layouts are from the 23.0.1 sysmodule (010000000000003E): the archive JSON parser at 0x1dd360
// (handlers 0x1ddb80, 0x1de100, 0x1de410) and GetDataInfo's copy at 0x1d6170.

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace OpenPak::Olsc {

/// One save_data_archive as the sysmodule caches it (its 0x68-byte record, the fields it keeps).
struct ArchiveInfo {
    std::uint64_t id = 0;
    std::uint64_t nsa_id = 0;
    std::uint64_t application_id = 0;
    std::uint64_t data_size = 0;
    std::uint8_t status = 0; ///< "fixed" 0, "uploading" 2 (the parser knows no other)
    std::uint8_t datatype = 0;
    std::uint32_t num_of_partitions = 0;
    std::uint32_t launch_required_version = 0;
    std::uint64_t previous_id = 0;
    std::uint64_t series_id = 0;
    std::uint64_t commit_id = 0;   ///< encoded_digest's first 8 bytes: the save's commit id
    std::uint64_t saved_at = 0;    ///< saved_at_as_unixtime
    std::uint64_t finished_at = 0; ///< finished_at_as_unixtime
    std::uint8_t platform_key_generation = 0;
    std::uint8_t acd_index = 0;
};

inline constexpr std::uint8_t StatusFixed = 0;
inline constexpr std::uint8_t StatusUploading = 2;

/// A `{"save_data_archives": [...]}` reply, read as the 23.0.1 sysmodule reads it (parser
/// 0x1dd360, record check 0x1dd580, list 0x1eaf30): every record needs every key but
/// platform_key_generation and acd_index; only desc, previous_save_data_archive_id and
/// finished_at_as_unixtime may be null (0x1dd760); status is "fixed" or "uploading";
/// encoded_digest is base64url of exactly 32 bytes. One record that fails refuses the whole
/// list (the record's parser stays open and the list ends in 2179-3201), so nullopt: the
/// cached list stays as it was. nullopt too when the body is not that shape.
std::optional<std::vector<ArchiveInfo>> ParseArchiveList(std::string_view body);

/// IRemoteStorageController 27 GetDataInfo's 0x38 bytes for an archive:
/// +0x00 id, +0x08 application id, +0x10 status (u8), +0x18 data size, +0x20 saved at,
/// +0x28 finished at, +0x30 acd index (u8); the rest zero.
using DataInfo = std::array<std::uint8_t, 0x38>;
DataInfo PackDataInfo(const ArchiveInfo& archive);

/// The archive GetDataInfo answers for a title: its newest fixed one, or none.
std::optional<ArchiveInfo> LatestFor(const std::vector<ArchiveInfo>& archives,
                                     std::uint64_t application_id);

/// What IRemoteStorageController 26 ListDataInfo lists: one archive per title, as LatestFor picks
/// it, newest first.
std::vector<ArchiveInfo> LatestPerApplication(const std::vector<ArchiveInfo>& archives);

/// The archive cache for firmware users (uid: the 32 hex digits acc prints a UserId as).
/// Lookup never blocks; Refresh fetches on a thread of its own and fills the cache when the
/// server answers. A failed fetch keeps what was there. With a store file set, the lists are
/// kept across restarts, as the sysmodule keeps its SaveDataArchiveInfo cache in its system
/// save, so a page opened before the next fetch lands shows the last known list.
class ArchiveCache {
public:
    static ArchiveCache& Instance();

    /// Keep the lists in `file` (read now; written on every change). A different file than the
    /// current one (another data directory) drops what is cached in memory first.
    void SetStore(const std::filesystem::path& file);

    /// Fetch `uid`'s list unless one was fetched in the last `max_age_s` seconds or a fetch is
    /// under way. Needs a configured console server and a linked user; otherwise does nothing.
    void Refresh(const std::string& uid, int max_age_s = 30);

    /// Fetch `uid`'s list now (blocking: an async task's work, never an IPC thread). True when
    /// the server answered with a list, which then replaces the cached one.
    bool RefreshNow(const std::string& uid);

    /// The cached list for `uid`, or nullopt when none has been fetched.
    std::optional<std::vector<ArchiveInfo>> Get(const std::string& uid) const;

    /// Sets a list as if fetched (the self-check, and a fetch's own result).
    void Put(const std::string& uid, std::vector<ArchiveInfo> archives);

    void Clear();

private:
    struct Entry {
        std::vector<ArchiveInfo> archives;
        std::int64_t fetched_at = 0;
        bool fetched = false;
        bool in_flight = false;
    };

    void LoadLocked();
    void SaveLocked() const;

    mutable std::mutex mutex;
    std::map<std::string, Entry> entries;
    std::filesystem::path store;
};

/// The stored form of an archive list (the store file's per-user entry), and back.
std::string SerializeArchives(const std::vector<ArchiveInfo>& archives);
std::optional<std::vector<ArchiveInfo>> DeserializeArchives(std::string_view text);

/// GET save_data_archives for `uid` (blocking; run it off the guest's IPC thread).
std::optional<std::vector<ArchiveInfo>> FetchArchives(const std::string& uid);

/// The olsc daemon's settings, persisted in one JSON file. Unknown users read as off, as the
/// sysmodule answers before anything was set.
class DaemonSettings {
public:
    explicit DaemonSettings(std::filesystem::path file);

    bool GlobalAutoUpload(const std::string& uid) const;
    void SetGlobalAutoUpload(const std::string& uid, bool enabled);
    bool GlobalAutoDownload(const std::string& uid) const;
    void SetGlobalAutoDownload(const std::string& uid, bool enabled);
    bool ApplicationAutoTransfer(const std::string& uid, std::uint64_t application_id) const;
    void SetApplicationAutoTransfer(const std::string& uid, std::uint64_t application_id,
                                    bool enabled);

private:
    struct User {
        bool auto_upload = false;
        bool auto_download = false;
        std::map<std::uint64_t, bool> auto_transfer;
    };

    void Load();
    void Save() const;

    std::filesystem::path file;
    mutable std::mutex mutex;
    std::map<std::string, User> users;
};

} // namespace OpenPak::Olsc

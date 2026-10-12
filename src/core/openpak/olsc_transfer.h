// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// The olsc sysmodule's upload and download of one save, as the 23.0.1 olsc does it
// (research/olsc-re/olsc-transfer.md §2-§3): the console protocol's REST calls in its order with
// its bodies, driving fs's transfer exporter/importer. Core-free: the REST transport and the fs
// side come in as interfaces, so the self-check can play the server.

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>
#include "core/openpak/olsc.h"

namespace OpenPak::Olsc::Transfer {

using Bytes = std::vector<std::uint8_t>;
using Key = std::array<std::uint8_t, 0x10>;
using Aad = std::array<std::uint8_t, 0x20>;

/// olsc results, 2179-<description> (olsc-transfer.md §5).
enum class Error : std::uint32_t {
    None = 0,
    Cancelled = 2801,
    PartitionCountDiffers = 301,
    SeriesBaseMissing = 2123,
    LocalSaveNotFound = 2000,
    PolicyForbidden = 2001,
    LocalSaveChanged = 2021,
    ArchiveGone = 2100,
    ChunkWithoutComponent = 2101,
    AlreadyUploading = 2102,
    ComponentAlreadyFixed = 2312,
    ArchiveMissingAfterFinish = 2400,
    ArchiveIncomplete = 3201,
    BackupListOverflow = 3200,
    PermissionIncomplete = 3320,
    ApplicationTooOld = 2802,
    AadMismatch = 3202,
    MetaWithoutUrl = 3221,
    MetaWrongSize = 3222,
    Server = 4000, ///< "code" 0000 / 0019-1000 (other codes map 4001-4299)
    Http = 9000,   ///< no "code": 2155-<status>; reported here as Http
    Fs = 9100,     ///< the fs side refused (its own result is logged)
};

struct HttpResponse {
    int status = 0;
    std::string body;
};

using Headers = std::vector<std::pair<std::string, std::string>>;

/// The console protocol's REST transport, as one user. The bearer, X-Device-Token and X-NSA-ID
/// every request carries (0x1e7d10) are its business; `headers` are the per-request ones
/// (X-SDA-ID, X-Ignorable).
class Rest {
public:
    virtual ~Rest() = default;
    /// A request to `host` (e.g. storage.hac.lp1.scsi.srv.nintendo.net); an empty `body` on a
    /// POST is sent as the console sends it (an empty body).
    virtual HttpResponse Request(const std::string& method, const std::string& host,
                                 const std::string& path, const std::string& body,
                                 const Headers& headers = {}) = 0;
    /// Account identifier bound into initial data, also sent as X-NSA-ID.
    virtual std::uint64_t NetworkServiceAccountId() = 0;
    /// The dauth device token for olsc's client (the policy request's `dtoken`); empty when dauth
    /// refused.
    virtual std::string DeviceToken() = 0;
    /// A blob GET or PUT to a signed URL from a component (https://host/path?query).
    virtual HttpResponse Blob(const std::string& method, const std::string& url,
                              std::span<const std::uint8_t> body) = 0;
};

/// fs's exporter (ISaveDataDivisionExporter) for one save.
class Exporter {
public:
    virtual ~Exporter() = default;
    virtual bool SetDivisionCount(int count) = 0;
    virtual std::vector<std::uint16_t> ChunkIds() = 0;
    virtual std::optional<Bytes> Chunk(std::uint16_t id) = 0;
    virtual void SetAad(const Aad& aad) = 0;
    /// GetImportInitialDataAad (80): a diff exporter's base archive AAD until SetAad.
    virtual Aad ImportAad() = 0;
    virtual Key KeySeed() = 0;
    virtual std::optional<Key> InitialDataMac() = 0;
    virtual bool Finalize() = 0;
};

/// fs's importer (ISaveDataDivisionImporter) for one save.
class Importer {
public:
    virtual ~Importer() = default;
    virtual Aad ImportAad() = 0;
    virtual std::vector<std::uint16_t> ChunkIds() = 0;
    virtual bool Push(std::uint16_t id, std::span<const std::uint8_t> bytes) = 0;
    virtual bool Finalize() = 0;
};

/// What olsc reads of the local save before an upload (fsp 61/57/616).
struct LocalSave {
    std::uint64_t application_id = 0;
    std::int64_t data_size = 0;
    std::int64_t saved_at = 0; ///< extra data time stamp
    std::uint64_t commit_id = 0;
    std::uint8_t acd_index = 0;
    std::uint32_t launch_required_version = 0; ///< the app's launch version >> 16
    std::uint8_t datatype = 0; ///< 0 account save; 3 application-driven island backup
    std::string desc;          ///< device-backup custom data
};

/// fs's transfer manager for one task (fsp 82): one challenge per task.
class Fs {
public:
    virtual ~Fs() = default;
    virtual Key Challenge() = 0;
    virtual bool SetKeySeedPackage(std::span<const std::uint8_t> package) = 0;
    virtual std::unique_ptr<Exporter> OpenFullExporter() = 0;
    /// OpenSaveDataDiffExporter (33) against the previous archive's 8 KiB initial data; needs
    /// that archive's key seed package first.
    virtual std::unique_ptr<Exporter> OpenDiffExporter(std::span<const std::uint8_t> meta) = 0;
    virtual std::unique_ptr<Importer> OpenImporter(std::span<const std::uint8_t> meta) = 0;
};

/// A transfer's byte counters. The transfer thread writes them while olsc's IPC threads read them
/// (GetTransferTaskProgress), so they are atomic; a copy is a snapshot.
struct Progress {
    std::atomic<std::uint64_t> total{0};
    std::atomic<std::uint64_t> done{0};
    std::atomic<bool> cancelled{false};
    std::stop_token cancellation;

    Progress() = default;
    Progress(const Progress& other)
        : total{other.total.load()}, done{other.done.load()}, cancelled{other.cancelled.load()},
          cancellation{other.cancellation} {}
    Progress& operator=(const Progress& other) {
        total = other.total.load();
        done = other.done.load();
        cancelled = other.cancelled.load();
        cancellation = other.cancellation;
        return *this;
    }
    bool IsCancelled() const {
        return cancelled.load() || cancellation.stop_requested();
    }
};

struct Outcome {
    Error error = Error::None;
    std::uint64_t archive_id = 0;
    std::string detail;          ///< for the log (no ids of the user)
    std::uint64_t series_id = 0; ///< the archive's series, on success
    std::uint64_t commit_id = 0; ///< the archive's commit id (encoded_digest), on success
};

/// An upload (olsc-transfer.md §2.2): a delta against the title's fixed archive when the cloud
/// has one (§2.3), else a full upload. `series_id` 0 picks a random one for a full upload.
Outcome Upload(Rest& rest, Fs& fs, const LocalSave& save, std::uint64_t series_id,
               Progress* progress);

/// A download of `archive_id` (olsc-transfer.md §3.1). `launch_version` is the installed
/// application's launch version >> 16 (the policy request's version).
Outcome Download(Rest& rest, Fs& fs, std::uint64_t application_id, std::uint64_t archive_id,
                 std::uint32_t launch_version, Progress* progress, std::uint8_t datatype = 0);

struct BackupList {
    Error error = Error::None;
    std::vector<ArchiveInfo> archives;
};
BackupList ListDeviceBackups(Rest& rest, std::uint64_t application_id, std::size_t capacity);

// Encoders the console uses (base64url without padding; MD5 for component digests).
std::string Base64Url(std::span<const std::uint8_t> data);
std::optional<Bytes> FromBase64Url(std::string_view text);
std::array<std::uint8_t, 16> Md5(std::span<const std::uint8_t> data);

/// start_upload's body, keys in the console's order (0x1ec2f0).
std::string StartUploadBody(const LocalSave& save, std::uint64_t series_id);

inline constexpr const char* StorageHost = "storage.hac.lp1.scsi.srv.nintendo.net";
inline constexpr const char* PolicyHost = "policy.hac.lp1.scsi.srv.nintendo.net";

constexpr bool IsDeviceBackupTitle(std::uint64_t application_id) {
    return application_id == 0x01006f8002326000ULL || application_id == 0x0100e6c01163c000ULL;
}

} // namespace OpenPak::Olsc::Transfer

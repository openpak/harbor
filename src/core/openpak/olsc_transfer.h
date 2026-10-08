// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// The olsc sysmodule's upload and download of one save, as the 23.0.1 olsc does it
// (research/olsc-re/olsc-transfer.md §2-§3): the console protocol's REST calls in its order with
// its bodies, driving fs's transfer exporter/importer. Core-free: the REST transport and the fs
// side come in as interfaces, so the self-check can play the server.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace OpenPak::Olsc::Transfer {

using Bytes = std::vector<std::uint8_t>;
using Key = std::array<std::uint8_t, 0x10>;
using Aad = std::array<std::uint8_t, 0x20>;

/// olsc results, 2179-<description> (olsc-transfer.md §5).
enum class Error : std::uint32_t {
    None = 0,
    Cancelled = 0,
    PartitionCountDiffers = 301,
    LocalSaveNotFound = 2000,
    PolicyForbidden = 2001,
    LocalSaveChanged = 2021,
    ArchiveGone = 2100,
    ChunkWithoutComponent = 2101,
    AlreadyUploading = 2102,
    ComponentAlreadyFixed = 2312,
    ArchiveMissingAfterFinish = 2400,
    ArchiveIncomplete = 3201,
    AadMismatch = 3202,
    MetaWithoutUrl = 3221,
    MetaWrongSize = 3222,
    Server = 4000,  ///< "code" 0000 / 0019-1000 (other codes map 4001-4299)
    Http = 9000,    ///< no "code": 2155-<status>; reported here as Http
    Fs = 9100,      ///< the fs side refused (its own result is logged)
};

struct HttpResponse {
    int status = 0;
    std::string body;
};

/// The console protocol's REST transport, as one user (bearer and sign-in are its business).
class Rest {
public:
    virtual ~Rest() = default;
    /// A request to `host` (e.g. storage.hac.lp1.scsi.srv.nintendo.net); `body` empty and
    /// `json` false sends an empty POST body as the console does.
    virtual HttpResponse Request(const std::string& method, const std::string& host,
                                 const std::string& path, const std::string& body) = 0;
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
};

/// fs's transfer manager for one task (fsp 82): one challenge per task.
class Fs {
public:
    virtual ~Fs() = default;
    virtual Key Challenge() = 0;
    virtual bool SetKeySeedPackage(std::span<const std::uint8_t> package) = 0;
    virtual std::unique_ptr<Exporter> OpenFullExporter() = 0;
    virtual std::unique_ptr<Importer> OpenImporter(std::span<const std::uint8_t> meta) = 0;
};

struct Progress {
    std::uint64_t total = 0;
    std::uint64_t done = 0;
};

struct Outcome {
    Error error = Error::None;
    std::uint64_t archive_id = 0;
    std::string detail; ///< for the log (no ids of the user)
};

/// A full upload (olsc-transfer.md §2.2). `series_id` 0 picks a random one.
Outcome Upload(Rest& rest, Fs& fs, const LocalSave& save, std::uint64_t series_id,
               Progress* progress);

/// A download of `archive_id` (olsc-transfer.md §3.1).
Outcome Download(Rest& rest, Fs& fs, std::uint64_t application_id, std::uint64_t archive_id,
                 Progress* progress);

// Encoders the console uses (base64url without padding; MD5 for component digests).
std::string Base64Url(std::span<const std::uint8_t> data);
std::optional<Bytes> FromBase64Url(std::string_view text);
std::array<std::uint8_t, 16> Md5(std::span<const std::uint8_t> data);

/// start_upload's body, keys in the console's order (0x1ec2f0).
std::string StartUploadBody(const LocalSave& save, std::uint64_t series_id);

inline constexpr const char* StorageHost = "storage.hac.lp1.scsi.srv.nintendo.net";
inline constexpr const char* PolicyHost = "policy.hac.lp1.scsi.srv.nintendo.net";

} // namespace OpenPak::Olsc::Transfer

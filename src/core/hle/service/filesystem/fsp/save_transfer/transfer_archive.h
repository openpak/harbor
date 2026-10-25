// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// A Save Data Cloud archive as the 23.0.1 FS exporter writes it and its importer reads it
// (fs-transfer.md §9.4-§9.5): the "SID0" v2 initial data, the meta chunk (id 0) and the data
// chunks (ids 0x1000 + n) of the save image stream. Pure code over transfer_crypto: the save
// image itself comes from the codec, the keys from transfer_keys.

#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "core/hle/service/filesystem/fsp/save_transfer/transfer_crypto.h"

namespace Service::FileSystem::SaveTransfer {

inline constexpr int MaxDivisionCount = 64;
inline constexpr u16 MetaChunkId = 0;
inline constexpr u16 FirstDataChunkId = 0x1000;
inline constexpr std::size_t MetaChunkSize = 0x2000;
inline constexpr std::size_t InitialDataSize = 0x1fd0;
/// Data chunks split the stream on this block size.
inline constexpr s64 DivisionAlignment = 0x4000;

using Sha256Hash = std::array<u8, 0x20>;

/// InitialDataVersion2Detail.Content (LibHac names; offsets marked FACT in fs-transfer.md).
struct InitialDataContent {
    u32 magic{0x30444953}; ///< "SID0"
    u32 version{2};
    std::array<u8, 0x200> extra_data{};
    s32 division_count{};
    u32 reserved20c{};
    s64 division_alignment{};
    std::array<Sha256Hash, MaxDivisionCount> chunk_hashes{};
    std::array<s64, MaxDivisionCount> chunk_sizes{};
    std::array<Key128, MaxDivisionCount> chunk_macs{};
    s64 total_stream_size{}; ///< uncompressed concatenated save-image length
    std::array<u8, 0x20> aad{};
    std::array<u8, 0x20> hash_salt{};  ///< the save's integrity seed
    std::array<u8, 0x200> short_hashes{};
    u8 is_integrity_seed_enabled{};
    u8 hash_algorithm{};
    std::array<u8, InitialDataSize - 0x1262> reserved{};
};
static_assert(sizeof(InitialDataContent) == InitialDataSize);
static_assert(offsetof(InitialDataContent, extra_data) == 0x8);
static_assert(offsetof(InitialDataContent, chunk_sizes) == 0xa18);
static_assert(offsetof(InitialDataContent, chunk_macs) == 0xc18);
static_assert(offsetof(InitialDataContent, total_stream_size) == 0x1018);
static_assert(offsetof(InitialDataContent, aad) == 0x1020);
static_assert(offsetof(InitialDataContent, hash_salt) == 0x1040);
static_assert(offsetof(InitialDataContent, short_hashes) == 0x1060);
static_assert(offsetof(InitialDataContent, is_integrity_seed_enabled) == 0x1260);

/// The 32-byte AAD olsc sets and checks: {network service account id, archive id, 0, 0}.
std::array<u8, 0x20> MakeAad(u64 nsa_id, u64 archive_id);

/// Hashes native OLSC checks for one save-image slice. Prefix storages contribute their
/// bytes; raw data contributes salted IVFC leaf hashes, with zero hashes for free blocks.
struct ChunkVerificationHashes {
    Sha256Hash full{};
    std::array<u8, 8> halves{};
};
std::optional<ChunkVerificationHashes> NativeChunkHashes(std::span<const u8> stream,
                                                         const std::array<u8, 0x200>& extra_data,
                                                         const std::array<u8, 0x20>& integrity_seed,
                                                         std::size_t offset, std::size_t length);

/// What the exporter archives: the save image stream (the codec's storages concatenated) and
/// the save's metadata.
struct ArchiveSource {
    std::vector<u8> stream;
    std::array<u8, 0x200> extra_data{};
    std::array<u8, 0x20> integrity_seed{};
};

/// The exporter's archive (ISaveDataDivisionExporter's data side). Chunk IVs are drawn once,
/// so every chunk reads the same on every open.
class ArchiveWriter {
public:
    ArchiveWriter(ArchiveSource source, const Key128& stream_key, const Key128& mac_key);

    /// Splits the stream into `count` data chunks (1..64). False when out of range.
    bool SetDivisionCount(int count);
    int DivisionCount() const;

    /// A diff export against the previous archive's initial data (OpenSaveDataDiffExporter):
    /// its division count and AAD, and every chunk whose plaintext hashes as the base's keeps the
    /// base's hash, size and MAC and is not exported again (the server hands the base's bytes
    /// over). False when the base's division count is out of range.
    bool SetBase(const InitialDataContent& base);

    /// The chunks to send, data then meta, the order an iterator gives them (a diff export
    /// leaves out the unchanged ones).
    std::vector<u16> ChunkIds() const;

    /// A chunk's bytes: header ∥ AES-GCM(raw deflate(slice)) ∥ tag for data chunks; the
    /// 0x2000-byte initial-data stream for the meta chunk (every data chunk is made first).
    std::optional<std::vector<u8>> Chunk(u16 id);

    void SetAad(const std::array<u8, 0x20>& aad);
    const std::array<u8, 0x20>& Aad() const;

    /// CMAC of the initial data (after every chunk is made).
    Key128 InitialDataMac();
    const InitialDataContent& Content();

private:
    void MakeAll();

    ArchiveSource source;
    Key128 stream_key;
    Key128 mac_key;
    InitialDataContent content{};
    std::array<Key128, MaxDivisionCount> ivs{};
    Key128 meta_iv{};
    std::vector<std::optional<std::vector<u8>>> chunks;
    std::vector<std::pair<std::size_t, std::size_t>> slices;
    std::vector<bool> unchanged;
};

/// The importer's archive side: the initial data, then each data chunk checked and inflated.
class ArchiveReader {
public:
    enum class Error {
        None,
        Meta,      ///< the meta stream does not decrypt or is not SID0 v2
        Mac,       ///< the initial-data MAC differs from the package's
        UnknownId, ///< a chunk id the initial data does not have
        Chunk,     ///< a chunk does not decrypt, its tag or size differs, or it does not inflate
    };

    /// The meta chunk with the stream key; `expected_mac` (from the key seed package) is checked
    /// with `mac_key` when given.
    Error Open(std::span<const u8> meta, const Key128& stream_key, const Key128& mac_key,
               const std::optional<Key128>& expected_mac);

    const InitialDataContent& Content() const;
    /// The MAC computed by the last Open (for diagnostics when it differs).
    const Key128& LastMac() const {
        return last_mac;
    }
    std::vector<u16> ChunkIds() const;

    /// One data chunk's archive bytes. Chunks may come in any order, each once.
    Error Push(u16 id, std::span<const u8> bytes);

    bool Complete() const;
    /// The save image stream, once every chunk is in.
    std::vector<u8> Stream() const;

private:
    Key128 last_mac{};
    Key128 stream_key{};
    InitialDataContent content{};
    std::vector<std::optional<std::vector<u8>>> plain;
};

} // namespace Service::FileSystem::SaveTransfer

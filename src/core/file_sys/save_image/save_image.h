// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// The console save image that a Save Data Cloud archive carries, built from and written back to
// an Eden host-directory save. Interface between the fs save-transfer objects (stream O) and the
// save-image codec (stream savecodec). Layout facts: research/olsc-re/fs-transfer.md §12.
//
// A console exports only the "SAVE" core layer of a journal-integrity save (its control area,
// allocation table and block area as the core sees it), preceded by the save's thumbnail and
// followed by zero padding to a 0x4000 boundary before the block area (23.0.1 FS 0xc8700). It
// never exports the physical header, journal, duplex, remap or integrity layers; the importing
// console formats a fresh save from the extra data's DataSize/JournalSize and writes the stream
// into it. So the core's geometry must be what the console would format for those sizes.

#pragma once

#include <array>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "core/file_sys/vfs/vfs_types.h"

namespace FileSys::SaveImage {

/// The elements of the transfer stream, in stream order (23.0.1 FS 0xc8700 → concat 0x24ee60),
/// plus the two integrity storages that FS names but does not put in the stream.
enum class InternalStorage : u8 {
    Thumbnail,               ///< the save's thumbnail meta file (0x40060 bytes), see ThumbnailSize
    SaveDataControlArea,     ///< "SaveDataControlArea": the core's SAVE header (0x48 bytes)
    AllocationTableMeta,     ///< "AllocationTableMeta": the core's allocation table
    Padding,                 ///< zeros up to the next 0x4000 boundary of the stream
    RawSaveDataWithZeroFree, ///< "RawSaveDataWithZeroFree": the core's blocks, free ones zero
    Sha2Salt,                ///< not in the stream (written into the save by the exporter)
    IntegrityWithZeroFree,   ///< not in the stream (used for diff export only)
};

/// Thumbnail element size; present for Account (1) and Device (3) saves, and as zeros for System
/// (0) saves; absent for the other save types.
constexpr std::size_t ThumbnailSize = 0x40060;

/// The metadata "files" the exporter puts into the initial data (0xcaa50) and the importer
/// gives back.
struct ImageParameters {
    u8 hash_algorithm{};                   ///< "…HashAlgorithm" (1 byte)
    bool hash_salt_enabled{};              ///< "…HashSaltEnabled" (1 byte)
    std::array<u8, 0x20> integrity_seed{}; ///< "…IntegritySeed" (0x20 bytes)
};

/// The sizes the save was created with: the extra data's DataSize (+0x58) and JournalSize
/// (+0x60). The console formats the importing save from these.
struct SaveGeometry {
    s64 data_size{};
    s64 journal_size{};
    u8 save_data_type{1}; ///< the attribute's SaveDataType (extra data +0x20): 1 = Account
};

struct ExportedImage {
    ImageParameters parameters;
    /// The stream elements in stream order. The exporter concatenates their bytes as-is and
    /// splits the stream into chunks; it may read each file many times and gets identical bytes
    /// every time.
    std::vector<std::pair<InternalStorage, VirtualFile>> storages;
};

/// Build the console image of the save in `save_dir` (an Eden host-directory save: its files
/// and directories, nothing else). `thumbnail` is the save's thumbnail (zeros when empty).
/// nullopt when it cannot be built (too large for the geometry, unreadable files, invalid
/// names). Deterministic for the same directory contents.
std::optional<ExportedImage> ExportSaveImage(const VirtualDir& save_dir,
                                             const SaveGeometry& geometry,
                                             std::span<const u8> thumbnail = {});

/// Replace the contents of `save_dir` with the files and directories of an image. `stream` is
/// the whole transfer stream, exactly as a console's importer receives it: the codec splits it
/// by the element sizes its geometry for `geometry` gives (the console formats its own save from
/// the same sizes and writes the stream into it). The image comes from a console or from
/// ExportSaveImage. False (and `save_dir` untouched) when it does not parse. `parameters` are
/// not needed to read the core. The thumbnail goes to `out_thumbnail` when given.
bool ImportSaveImage(const VirtualFile& stream, const ImageParameters& parameters,
                     const SaveGeometry& geometry, const VirtualDir& save_dir,
                     std::vector<u8>* out_thumbnail = nullptr);

} // namespace FileSys::SaveImage

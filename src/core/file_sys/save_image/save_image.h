// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// The console save image that a Save Data Cloud archive carries, built from and written back to
// an Eden host-directory save. Interface between the fs save-transfer objects (stream O) and the
// save-image codec (stream savecodec). Layout facts: research/olsc-re/fs-transfer.md §9.5, §11.
//
// A console exports only the "SAVE" core layer of a journal-integrity save (its control area,
// allocation table and block area as the core sees it) plus the integrity hash data for it; it
// never exports the physical header, journal, duplex or remap layers, and the importing console
// formats a fresh save from the extra data's DataSize/JournalSize and writes these storages
// into it. So the core's geometry must be what the console would format for those sizes.

#pragma once

#include <array>
#include <optional>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "core/file_sys/vfs/vfs_types.h"

namespace FileSys::SaveImage {

/// The internal storages of a save, by the names the 23.0.1 FS gives them, in the order the
/// exporter concatenates them (0xc8700).
enum class InternalStorage : u8 {
    SaveDataControlArea,     ///< "SaveDataControlArea": the core's SAVE header area
    AllocationTableMeta,     ///< "AllocationTableMeta": the core's allocation table
    RawSaveDataWithZeroFree, ///< "RawSaveDataWithZeroFree": the core's blocks, free ones zero
    Sha2Salt,                ///< "InternalStorageFileNameSha2Salt": only when the hash is salted
    IntegrityWithZeroFree,   ///< "InternalStorageFileNameIntegrity[Sha2Salt]WithZeroFree"
};

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
};

struct ExportedImage {
    ImageParameters parameters;
    /// Present storages in export order (Sha2Salt only when hash_salt_enabled). The exporter
    /// concatenates their bytes and splits the stream into chunks; it may read each file many
    /// times and needs identical bytes every time.
    std::vector<std::pair<InternalStorage, VirtualFile>> storages;
};

/// Build the console image of the save in `save_dir` (an Eden host-directory save: its files
/// and directories, nothing else). nullopt when it cannot be built (too large for the geometry,
/// unreadable files). Must be deterministic for the same directory contents.
std::optional<ExportedImage> ExportSaveImage(const VirtualDir& save_dir,
                                             const SaveGeometry& geometry);

/// Replace the contents of `save_dir` with the files and directories of an image. `stream` is
/// the exported storages concatenated in InternalStorage order, exactly as a console's importer
/// receives it: the codec splits it by the storage sizes its geometry for `geometry` gives
/// (the console formats its own save from the same sizes and writes the stream into it). The
/// image comes from a console or from ExportSaveImage. False (and `save_dir` untouched) when it
/// does not parse. Integrity data may be checked or ignored.
bool ImportSaveImage(const VirtualFile& stream, const ImageParameters& parameters,
                     const SaveGeometry& geometry, const VirtualDir& save_dir);

} // namespace FileSys::SaveImage

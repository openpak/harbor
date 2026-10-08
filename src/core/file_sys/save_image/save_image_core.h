// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-FileCopyrightText: 2018 Alex Barney
// SPDX-License-Identifier: AGPL-3.0-only AND MIT

// The "SAVE" core layer of a console journal-integrity save (format version 0x50000, core
// version 6): its control area, allocation table and block area. This is the part of a save a
// console exports for Save Data Cloud; the importing console formats its own save from the
// DataSize/JournalSize of the extra data and writes these three storages into it.
//
// Layout and every write the builder makes follow the 23.0.1 FS sysmodule (format 0x5fed0,
// 0x60210, allocation table 0x67090/0x67520/0x677b0/0x67980/0x67cb0, key-value table
// 0x63b90/0x66430/0x66670). The on-disk structures were cross-checked against LibHac
// (Copyright (c) 2018 Alex Barney, MIT licence, LICENSES/MIT.txt): its SaveDataFileSystemCore,
// AllocationTable and HierarchicalSaveFileTable read the same bytes.

#pragma once

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "common/common_types.h"

namespace FileSys::SaveImage {

constexpr s64 BlockSize = 0x4000;
constexpr std::size_t ControlAreaSize = 0x48;
constexpr std::size_t MaxNameLength = 0x40;

/// A save's files as a tree: what an Eden host-directory save holds.
struct TreeDir {
    std::vector<std::pair<std::string, TreeDir>> dirs;
    std::vector<std::pair<std::string, std::vector<u8>>> files;

    bool operator==(const TreeDir&) const = default;
};

/// The core's geometry, fixed by the save's DataSize: what nnSdk formats for it.
struct CoreGeometry {
    u32 block_count{};  ///< data blocks of 0x4000 bytes: ceil(DataSize / 0x4000)
    s64 fat_size{};     ///< allocation table region: alignUp((block_count + 1) * 8, 0x4000)

    s64 RawSize() const {
        return static_cast<s64>(block_count) * BlockSize;
    }
};

/// nullopt when the size cannot hold a core (the console refuses fewer than 2 blocks).
std::optional<CoreGeometry> CoreGeometryFor(s64 data_size);

/// A built core. The block area is sparse: blocks not in `blocks` are free and read as zeros.
struct CoreImage {
    CoreGeometry geometry;
    std::array<u8, ControlAreaSize> control{};
    std::vector<u8> fat;
    std::map<u32, std::vector<u8>> blocks;

    /// Reads the block area ("RawSaveDataWithZeroFree"). Returns the bytes read.
    std::size_t ReadRaw(u8* out, std::size_t length, std::size_t offset) const;
};

/// Builds the core a console would hold after formatting a save of this geometry and creating
/// the tree's directories and files in order (names sorted, files before subdirectories).
/// Deterministic. nullopt with `error` set when it does not fit or a name is invalid.
std::optional<CoreImage> BuildCore(const TreeDir& root, const CoreGeometry& geometry,
                                   std::string* error);

/// Reads `length` bytes of the block area at `offset`. False on a short read.
using RawReader = std::function<bool(u8* out, std::size_t length, u64 offset)>;

/// Parses a core (from a console or from BuildCore) into a tree. Checks the structure the way
/// the FS core does on mount (magic, version, geometry, chains). nullopt with `error` set when
/// it does not parse.
std::optional<TreeDir> ParseCore(std::span<const u8> control, std::span<const u8> fat,
                                 const RawReader& raw, u64 raw_size, std::string* error);

/// Sorts a tree's entries by name, recursively (the builder's order; makes trees comparable).
void SortTree(TreeDir& dir);

} // namespace FileSys::SaveImage

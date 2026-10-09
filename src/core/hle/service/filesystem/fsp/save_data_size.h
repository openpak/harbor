// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// A save's size as a 23.0.1 console reports it: the journal-integrity save image's total size
// for the DataSize/JournalSize it was created with (research/olsc-re/fs-save-size.md). fs stores
// it in the save data indexer at creation (CreateSaveDataFileSystemCore 0xaa950 →
// QuerySaveDataTotalSize 0xab1e0 → QuerySize 0x131f00) and hands it out unchanged as
// SaveDataInfo.size (+0x30); fsp-srv 601 answers the same function. Thumbnails are not counted.

#pragma once

#include <optional>

#include "common/common_types.h"

namespace Service::FileSystem {

/// The total size of a version 0x50000 save image (block size 0x4000, one expansion), or nullopt
/// for sizes the console refuses (no data blocks, or a journal hash tree whose master hash would
/// not fit the header).
constexpr std::optional<s64> QuerySaveDataTotalSize(s64 data_size, s64 journal_size) {
    constexpr s64 BS = 0x4000;
    constexpr s64 ExpandMax = 1;
    const auto al = [](s64 x, s64 a) { return (x + a - 1) & -a; };
    const auto a512 = [&](s64 x) { return al(x, 0x200); };
    if (data_size <= 0 || journal_size < 0) {
        return std::nullopt;
    }
    // The hash levels of an IVFC tree over `data` (0x152e30): lv[levels] is the data, each level
    // up holds 0x20 bytes per block of the one below; levels 2..levels-1 are stored, the master
    // hash covers level 2.
    struct Levels {
        s64 master{};
        s64 level[4]{};
    };
    const auto ivfc = [&](s64 data, int levels) {
        s64 lv[6]{};
        lv[levels] = al(data, BS);
        for (int i = levels - 1; i > 1; --i) {
            lv[i] = al(lv[i + 1] / BS * 0x20, BS);
        }
        Levels out{lv[2] / BS * 0x20, {}};
        for (int i = 2; i < levels; ++i) {
            out.level[i - 2] = lv[i];
        }
        return out;
    };

    const s64 cd = (data_size + BS - 1) / BS;
    const s64 cj = (journal_size + BS - 1) / BS;
    // Journal mapping table and bitmaps (0x157300), allocation table (0x67080).
    const s64 table = cd * 8;
    const s64 physical = ((cd + cj + 0x1f) >> 3) & 0x1ffffffc;
    const s64 virtual_bitmap = ((cd + 0x1f) >> 3) & 0x1ffffffc;
    const s64 fat = al((cd + 1) * 8, BS);
    const Levels journal = ivfc(cd * BS, 5);
    if (journal.master != 0x20 || journal.level[0] != BS) {
        return std::nullopt;
    }
    const Levels fat_tree = ivfc(fat, 4); // version 5 and later
    const s64 meta = a512(table) + a512(physical) + a512(virtual_bitmap) + a512(physical) +
                     a512(fat) + a512(BS) + a512(journal.level[1]) + a512(journal.level[2]) +
                     a512(fat_tree.level[0]) + a512(fat_tree.level[1]);
    const s64 duplexed = al(meta, BS);
    const s64 bitmap = al(((duplexed / BS + 0x1ff) >> 3) & 0x3fffffc0, BS); // 0x14ae60
    const s64 main = al(cd * BS + cj * BS + 2 * (a512(bitmap) + a512(duplexed)), BS);
    const auto remap_table = [](s64 n) { return (n + (n - 1) / 2) * 0x20; }; // 0x16d170
    const s64 header = 0x8000 + al(remap_table(5 * ExpandMax + 5), BS) +
                       al(remap_table(10 + 10 * ExpandMax), BS);
    return header + main;
}

static_assert(QuerySaveDataTotalSize(0x4000, 0x4000) == 0x58000);
static_assert(QuerySaveDataTotalSize(0x80000, 0x80000) == 0x150000);
static_assert(QuerySaveDataTotalSize(1 << 20, 1 << 20) == 0x250000);
static_assert(QuerySaveDataTotalSize(32 << 20, 16 << 20) == 0x3078000);
static_assert(QuerySaveDataTotalSize(64 << 20, 64 << 20) == 0x80a8000);
static_assert(!QuerySaveDataTotalSize(0, 0));

} // namespace Service::FileSystem

// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-FileCopyrightText: 2018 Alex Barney
// SPDX-License-Identifier: AGPL-3.0-only AND MIT

// The "SAVE" core layer: builder and parser. Addresses below are 23.0.1 FS.bin (FS.elf) vaddrs;
// structure names follow LibHac (Copyright (c) 2018 Alex Barney, MIT, LICENSES/MIT.txt).

#include <algorithm>
#include <cstring>
#include <set>

#include "core/file_sys/save_image/save_image_core.h"

namespace FileSys::SaveImage {

namespace {

// "SAVE" header (control area +0x00, 0x18 bytes) then the allocation table header (+0x18,
// 0x30 bytes): written by 0x5fed0 / 0x60210, read by the core's mount 0x60d90.
constexpr u32 SaveMagic = 0x45564153;   // "SAVE"
constexpr u32 SaveVersion = 0x00060000; // the mount (0x60d90) wants the high half to be 6
constexpr std::size_t FatHeaderOffset = 0x18;

// Allocation table entries are {u32 prev, u32 next} pairs; entry 0 heads the free list and
// entry i (i >= 1) describes block i - 1.
constexpr u32 ListStart = 0x80000000; ///< prev of a list's first segment
constexpr u32 MultiBlock = 0x80000000; ///< next's high bit: the segment spans several blocks
constexpr u32 NoEnd = 0x80000000;      ///< "single block" end argument of 0x67980
constexpr u32 IndexMask = 0x7fffffff;

// Key-value tables (directory and file table): 0x60-byte entries in a block chain.
constexpr std::size_t TableEntrySize = 0x60;
constexpr std::size_t KeyParent = 0x00;
constexpr std::size_t KeyName = 0x04;
constexpr std::size_t KeySize = 0x44;
constexpr std::size_t ValueNextSibling = 0x44;
constexpr std::size_t ValueDirNextDir = 0x48;
constexpr std::size_t ValueDirNextFile = 0x4c;
constexpr std::size_t ValueFileStartBlock = 0x48;
constexpr std::size_t ValueFileLength = 0x4c;
constexpr std::size_t EntryNext = 0x5c;
constexpr u32 EmptyFileStartBlock = 0x80000000; ///< a zero-length file owns no blocks

u32 Get32(std::span<const u8> b, std::size_t off) {
    u32 v;
    std::memcpy(&v, b.data() + off, 4);
    return v;
}
u64 Get64(std::span<const u8> b, std::size_t off) {
    u64 v;
    std::memcpy(&v, b.data() + off, 8);
    return v;
}
void Put32(std::span<u8> b, std::size_t off, u32 v) {
    std::memcpy(b.data() + off, &v, 4);
}
void Put64(std::span<u8> b, std::size_t off, u64 v) {
    std::memcpy(b.data() + off, &v, 8);
}

/// The allocation table as nnSdk writes it (format 0x67090, allocate 0x67520/0x677b0,
/// segment write 0x67980, join 0x67cb0). Only the paths a freshly formatted table takes are
/// modelled: the free list is a single segment and allocations never span a hole.
class AllocationTable {
public:
    AllocationTable(std::vector<u8>& buffer, u32 block_count)
        : buf{buffer}, count{block_count} {}

    void Format() {
        Set(0, 0, 1);
        Set(1, ListStart, count < 2 ? 0 : MultiBlock);
        if (count >= 2) {
            Set(2, ListStart | 1, count);
            Set(count, ListStart | 1, count);
        }
    }

    /// Blocks still on the free list.
    u32 FreeBlocks() const {
        u32 total = 0;
        u32 idx = Next(0);
        while (idx != 0 && total <= count) {
            const u32 next = Next(idx);
            total += (next & MultiBlock) ? Next(idx + 1) - idx + 1 : 1;
            idx = next & IndexMask;
        }
        return total;
    }

    /// Takes `blocks` blocks from the head of the free list as one new list; returns its first
    /// block. nullopt when the free list's head segment is too short.
    std::optional<u32> Allocate(u32 blocks) {
        const u32 head = Next(0);
        if (blocks == 0 || head == 0) {
            return std::nullopt;
        }
        const u32 head_prev = Prev(head);
        const u32 head_next = Next(head);
        u32 taken;
        u32 new_free;
        bool split = false;
        if (head_next & MultiBlock) {
            const u32 end = Next(head + 1);
            const u32 length = end - head + 1;
            if (blocks < length) {
                // 0x677b0: the rest of the segment stays on the free list as a new list start.
                WriteSegment(head + blocks, head + blocks, end, head_prev, head_next & IndexMask);
                taken = blocks;
                new_free = head + blocks;
                split = true;
            } else {
                taken = length;
                new_free = head_next & IndexMask;
            }
        } else {
            taken = 1;
            new_free = head_next;
        }
        if (taken < blocks) {
            return std::nullopt;
        }
        if (!split && new_free != 0) {
            SetPrev(new_free, ListStart);
        }
        WriteSegment(head, head, taken == 1 ? NoEnd : head + taken - 1, ListStart, 0);
        Set(0, Prev(0), new_free);
        return head - 1;
    }

    /// Appends the list starting at `new_block` to the list starting at `head_block`.
    void Join(u32 head_block, u32 new_block) {
        const u32 head = head_block + 1;
        const u32 added = new_block + 1;
        const u32 added_prev = Prev(added);
        u32 tail = head;
        for (u32 guard = 0; (Next(tail) & IndexMask) != 0 && guard <= count; ++guard) {
            tail = Next(tail) & IndexMask;
        }
        const u32 tail_prev = Prev(tail);
        const u32 tail_next = Next(tail);
        SetPrev(head, added_prev);
        Set(tail, tail_prev, (tail_next & MultiBlock) | added);
        SetPrev(added, tail);
    }

    u32 Prev(u32 idx) const {
        return Get32(buf, idx * 8);
    }
    u32 Next(u32 idx) const {
        return Get32(buf, idx * 8 + 4);
    }

private:
    void Set(u32 idx, u32 prev, u32 next) {
        Put32(buf, idx * 8, prev);
        Put32(buf, idx * 8 + 4, next);
    }
    void SetPrev(u32 idx, u32 prev) {
        Put32(buf, idx * 8, prev);
    }

    /// 0x67980: writes a segment's head entry (and its range entries when it spans blocks).
    void WriteSegment(u32 head, u32 start, u32 end, u32 prev, u32 next) {
        const bool single = end == NoEnd || start == end;
        Set(start & IndexMask, start != head ? prev : ListStart, single ? next : next | MultiBlock);
        if ((next & IndexMask) != 0) {
            SetPrev(next & IndexMask, start);
        }
        if (!single) {
            Set(start + 1, start | ListStart, end);
            if (start + 1 != end) {
                Set(end, start | ListStart, end);
            }
        }
    }

    std::vector<u8>& buf;
    u32 count;
};

/// A key-value table (directory or file table) on a chain of blocks: 0x66430 add, 0x66670
/// entry allocation (grows the chain by one block when full), 0x63b90 initialisation.
class KeyValueTable {
public:
    KeyValueTable(AllocationTable& table, u32 first_block) : fat{table} {
        chain.push_back(first_block);
        bytes.resize(BlockSize);
    }

    void Initialize() {
        std::fill(bytes.begin(), bytes.begin() + 2 * TableEntrySize, u8{0});
        Put32(bytes, 0, 2);
        Put32(bytes, 4, Capacity());
    }

    /// Adds an entry with this key and value; returns its index. nullopt when the table
    /// cannot grow.
    std::optional<u32> Add(std::span<const u8, KeySize> key,
                           std::span<const u8, TableEntrySize - KeySize - 4> value) {
        const u32 free_head = Get32(bytes, EntryNext);
        u32 index;
        if (free_head != 0) {
            index = free_head;
            Put32(bytes, EntryNext, Get32(Entry(index), EntryNext));
        } else {
            const u32 length = Get32(bytes, 0);
            if (Get32(bytes, 4) <= length) {
                const auto block = fat.Allocate(1);
                if (!block) {
                    return std::nullopt;
                }
                fat.Join(chain.front(), *block);
                chain.push_back(*block);
                bytes.resize(bytes.size() + BlockSize);
                Put32(bytes, 4, Capacity());
            }
            Put32(bytes, 0, length + 1);
            index = length;
        }
        const u32 used_head = Get32(Entry(1), EntryNext);
        Put32(Entry(1), EntryNext, index);
        auto entry = Entry(index);
        std::copy(key.begin(), key.end(), entry.begin());
        std::copy(value.begin(), value.end(), entry.begin() + KeySize);
        Put32(entry, EntryNext, used_head);
        return index;
    }

    std::span<u8> Entry(u32 index) {
        return std::span<u8>(bytes).subspan(index * TableEntrySize, TableEntrySize);
    }

    /// Scatters the table into its blocks.
    void Store(std::map<u32, std::vector<u8>>& blocks) const {
        for (std::size_t i = 0; i < chain.size(); ++i) {
            blocks[chain[i]].assign(bytes.begin() + i * BlockSize,
                                    bytes.begin() + (i + 1) * BlockSize);
        }
    }

private:
    u32 Capacity() const {
        return static_cast<u32>(bytes.size() / TableEntrySize);
    }

    AllocationTable& fat;
    std::vector<u32> chain;
    std::vector<u8> bytes;
};

bool ValidName(const std::string& name) {
    return !name.empty() && name.size() <= MaxNameLength && name != "." && name != ".." &&
           name.find('/') == std::string::npos && name.find('\\') == std::string::npos &&
           name.find('\0') == std::string::npos;
}

std::array<u8, KeySize> MakeKey(u32 parent, const std::string& name) {
    std::array<u8, KeySize> key{};
    Put32(key, KeyParent, parent);
    std::memcpy(key.data() + KeyName, name.data(), name.size());
    return key;
}

class CoreBuilder {
public:
    CoreBuilder(CoreImage& image_, std::string* error_)
        : image{image_}, error{error_}, fat{image_.fat, image_.geometry.block_count} {}

    bool Build(const TreeDir& root) {
        const u32 n = image.geometry.block_count;
        auto header = std::span<u8>(image.control).subspan(FatHeaderOffset);
        Put64(header, 0x00, BlockSize);
        Put64(header, 0x08, 0);
        Put64(header, 0x10, n);
        Put64(header, 0x18, 0);
        Put64(header, 0x20, n);
        fat.Format();
        // 0x60210: directory table (3 entries) then file table (2 entries), one block each.
        const auto dir_block = fat.Allocate(1);
        const auto file_block = fat.Allocate(1);
        if (!dir_block || !file_block) {
            return Fail("the save is too small for its tables");
        }
        Put32(header, 0x28, *dir_block);
        Put32(header, 0x2c, *file_block);
        dirs.emplace(fat, *dir_block);
        files.emplace(fat, *file_block);
        dirs->Initialize();
        files->Initialize();
        const std::array<u8, 0x18> zero_value{};
        const auto root_index = dirs->Add(MakeKey(0, {}), zero_value);
        if (!root_index) {
            return Fail("the directory table is full");
        }
        if (!AddContents(root, *root_index)) {
            return false;
        }
        dirs->Store(image.blocks);
        files->Store(image.blocks);
        // 0x5fed0 writes the SAVE header last.
        Put32(image.control, 0x00, SaveMagic);
        Put32(image.control, 0x04, SaveVersion);
        Put64(image.control, 0x08, n);
        Put64(image.control, 0x10, BlockSize);
        return true;
    }

private:
    bool AddContents(const TreeDir& dir, u32 dir_index) {
        std::vector<const std::pair<std::string, std::vector<u8>>*> sorted_files;
        for (const auto& file : dir.files) {
            sorted_files.push_back(&file);
        }
        std::sort(sorted_files.begin(), sorted_files.end(),
                  [](const auto* a, const auto* b) { return a->first < b->first; });
        for (std::size_t i = 1; i < sorted_files.size(); ++i) {
            if (sorted_files[i - 1]->first == sorted_files[i]->first) {
                return Fail("duplicate file name");
            }
        }
        for (const auto* file : sorted_files) {
            if (!AddFile(file->first, file->second, dir_index)) {
                return false;
            }
        }
        std::vector<const std::pair<std::string, TreeDir>*> sorted_dirs;
        for (const auto& sub : dir.dirs) {
            sorted_dirs.push_back(&sub);
        }
        std::sort(sorted_dirs.begin(), sorted_dirs.end(),
                  [](const auto* a, const auto* b) { return a->first < b->first; });
        for (std::size_t i = 1; i < sorted_dirs.size(); ++i) {
            if (sorted_dirs[i - 1]->first == sorted_dirs[i]->first) {
                return Fail("duplicate directory name");
            }
        }
        for (const auto* sub : sorted_dirs) {
            const auto sub_index = AddDirectory(sub->first, dir_index);
            if (!sub_index || !AddContents(sub->second, *sub_index)) {
                return false;
            }
        }
        return true;
    }

    std::optional<u32> AddDirectory(const std::string& name, u32 parent) {
        if (!ValidName(name)) {
            Fail("invalid directory name");
            return std::nullopt;
        }
        // New children go to the head of the parent's list (LibHac LinkDirectoryToParent).
        std::array<u8, 0x18> value{};
        Put32(value, 0, Get32(dirs->Entry(parent), ValueDirNextDir));
        const auto index = dirs->Add(MakeKey(parent, name), value);
        if (!index) {
            Fail("the directory table is full");
            return std::nullopt;
        }
        Put32(dirs->Entry(parent), ValueDirNextDir, *index);
        return index;
    }

    bool AddFile(const std::string& name, const std::vector<u8>& data, u32 parent) {
        if (!ValidName(name)) {
            return Fail("invalid file name");
        }
        u32 start_block = EmptyFileStartBlock;
        if (!data.empty()) {
            const u64 blocks = (data.size() + BlockSize - 1) / BlockSize;
            if (blocks > fat.FreeBlocks()) {
                return Fail("the files do not fit the save's data size");
            }
            const auto first = fat.Allocate(static_cast<u32>(blocks));
            if (!first) {
                return Fail("allocation failed");
            }
            start_block = *first;
            for (u64 i = 0; i < blocks; ++i) {
                const std::size_t off = static_cast<std::size_t>(i * BlockSize);
                const std::size_t len = std::min<std::size_t>(BlockSize, data.size() - off);
                auto& block = image.blocks[static_cast<u32>(start_block + i)];
                block.assign(BlockSize, 0);
                std::memcpy(block.data(), data.data() + off, len);
            }
        }
        std::array<u8, 0x18> value{};
        Put32(value, 0, Get32(dirs->Entry(parent), ValueDirNextFile));
        Put32(value, ValueFileStartBlock - ValueNextSibling, start_block);
        Put64(value, ValueFileLength - ValueNextSibling, data.size());
        const auto index = files->Add(MakeKey(parent, name), value);
        if (!index) {
            return Fail("the file table is full");
        }
        Put32(dirs->Entry(parent), ValueDirNextFile, *index);
        return true;
    }

    bool Fail(const char* what) {
        if (error) {
            *error = what;
        }
        return false;
    }

    CoreImage& image;
    std::string* error;
    AllocationTable fat;
    std::optional<KeyValueTable> dirs;
    std::optional<KeyValueTable> files;
};

/// Reads the core back: chains, tables, files.
class CoreParser {
public:
    CoreParser(std::span<const u8> fat_, u32 count_, const RawReader& raw_, std::string* error_)
        : fat{fat_}, count{count_}, raw{raw_}, error{error_} {}

    /// The blocks of the list starting at `block`, in order.
    std::optional<std::vector<u32>> Chain(u32 block) {
        std::vector<u32> blocks;
        u32 idx = block + 1;
        bool first = true;
        while (idx != 0) {
            if (idx > count) {
                return Fail<std::vector<u32>>("allocation table index out of range");
            }
            const u32 prev = Get32(fat, idx * 8);
            const u32 next = Get32(fat, idx * 8 + 4);
            if (first && prev != ListStart) {
                return Fail<std::vector<u32>>("a list does not start at its first block");
            }
            first = false;
            u32 length = 1;
            if (next & MultiBlock) {
                if (idx + 1 > count) {
                    return Fail<std::vector<u32>>("allocation table range out of range");
                }
                const u32 range_start = Get32(fat, (idx + 1) * 8);
                const u32 end = Get32(fat, (idx + 1) * 8 + 4);
                if (range_start != (idx | ListStart) || end < idx + 1 || end > count) {
                    return Fail<std::vector<u32>>("invalid allocation table range entry");
                }
                length = end - idx + 1;
            }
            if (blocks.size() + length > count) {
                return Fail<std::vector<u32>>("allocation table cycle");
            }
            for (u32 i = 0; i < length; ++i) {
                blocks.push_back(idx - 1 + i);
            }
            idx = next & IndexMask;
        }
        return blocks;
    }

    /// Reads `length` bytes from the start of a chain.
    std::optional<std::vector<u8>> ReadChain(const std::vector<u32>& blocks, u64 length) {
        if (length > static_cast<u64>(blocks.size()) * BlockSize) {
            return Fail<std::vector<u8>>("a file is longer than its blocks");
        }
        std::vector<u8> out(static_cast<std::size_t>(length));
        for (std::size_t i = 0; i * BlockSize < length; ++i) {
            const std::size_t off = i * BlockSize;
            const std::size_t len = std::min<std::size_t>(BlockSize, out.size() - off);
            if (!raw(out.data() + off, len, static_cast<u64>(blocks[i]) * BlockSize)) {
                return Fail<std::vector<u8>>("short read of the block area");
            }
        }
        return out;
    }

    bool LoadTables(u32 dir_block, u32 file_block) {
        return LoadTable(dir_block, dir_table) && LoadTable(file_block, file_table);
    }

    std::optional<TreeDir> Tree() {
        // The root is the directory keyed (parent 0, empty name); nnSdk puts it at index 2.
        std::optional<u32> root;
        for (u32 idx : Used(dir_table)) {
            const auto entry = EntryOf(dir_table, idx);
            if (Get32(entry, KeyParent) == 0 && entry[KeyName] == 0) {
                root = idx;
                break;
            }
        }
        if (!root) {
            return Fail<TreeDir>("no root directory");
        }
        TreeDir tree;
        std::set<u32> seen{*root};
        if (!ReadDir(*root, tree, seen, 0)) {
            return std::nullopt;
        }
        return tree;
    }

private:
    bool LoadTable(u32 block, std::vector<u8>& out) {
        const auto chain = Chain(block);
        if (!chain) {
            return false;
        }
        auto bytes = ReadChain(*chain, static_cast<u64>(chain->size()) * BlockSize);
        if (!bytes) {
            return false;
        }
        const u32 length = Get32(*bytes, 0);
        const u32 capacity = Get32(*bytes, 4);
        if (length < 2 || length > capacity || capacity > bytes->size() / TableEntrySize) {
            return Fail<bool>("invalid key-value table header").has_value();
        }
        out = std::move(*bytes);
        return true;
    }

    std::span<const u8> EntryOf(const std::vector<u8>& table, u32 idx) const {
        return std::span<const u8>(table).subspan(idx * TableEntrySize, TableEntrySize);
    }

    bool InTable(const std::vector<u8>& table, u32 idx) const {
        return idx >= 2 && idx < Get32(table, 0);
    }

    std::vector<u32> Used(const std::vector<u8>& table) const {
        std::vector<u32> out;
        u32 idx = Get32(EntryOf(table, 1), EntryNext);
        while (idx != 0 && InTable(table, idx) && out.size() < Get32(table, 0)) {
            out.push_back(idx);
            idx = Get32(EntryOf(table, idx), EntryNext);
        }
        return out;
    }

    static std::string NameOf(std::span<const u8> entry) {
        const auto name = entry.subspan(KeyName, MaxNameLength);
        const auto end = std::find(name.begin(), name.end(), u8{0});
        return std::string(name.begin(), end);
    }

    bool ReadDir(u32 dir_idx, TreeDir& out, std::set<u32>& seen, int depth) {
        if (depth > 64) {
            return Fail<bool>("directory tree too deep").has_value();
        }
        const auto dir_entry = EntryOf(dir_table, dir_idx);
        std::set<u32> seen_files;
        for (u32 f = Get32(dir_entry, ValueDirNextFile); f != 0;) {
            if (!InTable(file_table, f) || !seen_files.insert(f).second) {
                return Fail<bool>("invalid file list").has_value();
            }
            const auto entry = EntryOf(file_table, f);
            if (Get32(entry, KeyParent) != dir_idx) {
                return Fail<bool>("a file's parent does not match its directory").has_value();
            }
            const u32 start = Get32(entry, ValueFileStartBlock);
            const u64 length = Get64(entry, ValueFileLength);
            std::vector<u8> data;
            if (length != 0) {
                const auto chain = Chain(start);
                if (!chain) {
                    return false;
                }
                auto bytes = ReadChain(*chain, length);
                if (!bytes) {
                    return false;
                }
                data = std::move(*bytes);
            }
            const auto name = NameOf(entry);
            if (!ValidName(name)) {
                return Fail<bool>("invalid file name in the image").has_value();
            }
            out.files.emplace_back(name, std::move(data));
            f = Get32(entry, ValueNextSibling);
        }
        for (u32 d = Get32(dir_entry, ValueDirNextDir); d != 0;) {
            if (!InTable(dir_table, d) || !seen.insert(d).second) {
                return Fail<bool>("invalid directory list").has_value();
            }
            const auto entry = EntryOf(dir_table, d);
            if (Get32(entry, KeyParent) != dir_idx) {
                return Fail<bool>("a directory's parent does not match").has_value();
            }
            const auto name = NameOf(entry);
            if (!ValidName(name)) {
                return Fail<bool>("invalid directory name in the image").has_value();
            }
            TreeDir sub;
            if (!ReadDir(d, sub, seen, depth + 1)) {
                return false;
            }
            out.dirs.emplace_back(name, std::move(sub));
            d = Get32(entry, ValueNextSibling);
        }
        return true;
    }

    template <typename T>
    std::optional<T> Fail(const char* what) {
        if (error) {
            *error = what;
        }
        return std::nullopt;
    }

    std::span<const u8> fat;
    u32 count;
    const RawReader& raw;
    std::string* error;
    std::vector<u8> dir_table;
    std::vector<u8> file_table;
};

} // namespace

std::optional<CoreGeometry> CoreGeometryFor(s64 data_size) {
    if (data_size <= 0) {
        return std::nullopt;
    }
    const s64 blocks = (data_size + BlockSize - 1) / BlockSize;
    if (blocks < 2 || blocks > 0x7ffffff) {
        return std::nullopt;
    }
    CoreGeometry g;
    g.block_count = static_cast<u32>(blocks);
    g.fat_size = ((blocks + 1) * 8 + BlockSize - 1) / BlockSize * BlockSize;
    return g;
}

std::size_t CoreImage::ReadRaw(u8* out, std::size_t length, std::size_t offset) const {
    const std::size_t size = static_cast<std::size_t>(geometry.RawSize());
    if (offset >= size) {
        return 0;
    }
    length = std::min(length, size - offset);
    std::size_t done = 0;
    while (done < length) {
        const std::size_t pos = offset + done;
        const u32 block = static_cast<u32>(pos / BlockSize);
        const std::size_t in_block = pos % BlockSize;
        const std::size_t len = std::min<std::size_t>(BlockSize - in_block, length - done);
        const auto it = blocks.find(block);
        if (it == blocks.end()) {
            std::memset(out + done, 0, len);
        } else {
            std::memcpy(out + done, it->second.data() + in_block, len);
        }
        done += len;
    }
    return done;
}

std::optional<CoreImage> BuildCore(const TreeDir& root, const CoreGeometry& geometry,
                                   std::string* error) {
    CoreImage image;
    image.geometry = geometry;
    image.fat.assign(static_cast<std::size_t>(geometry.fat_size), 0);
    CoreBuilder builder(image, error);
    if (!builder.Build(root)) {
        return std::nullopt;
    }
    return image;
}

std::optional<TreeDir> ParseCore(std::span<const u8> control, std::span<const u8> fat,
                                 const RawReader& raw, u64 raw_size, std::string* error) {
    const auto fail = [&](const char* what) -> std::optional<TreeDir> {
        if (error) {
            *error = what;
        }
        return std::nullopt;
    };
    if (control.size() < ControlAreaSize) {
        return fail("control area too small");
    }
    if (Get32(control, 0) != SaveMagic) {
        return fail("no SAVE header");
    }
    if ((Get32(control, 4) >> 16) != (SaveVersion >> 16)) {
        return fail("unsupported SAVE version");
    }
    const u64 block_count = Get64(control, 0x08);
    const u64 block_size = Get64(control, 0x10);
    const auto header = control.subspan(FatHeaderOffset);
    if (block_size != BlockSize || Get64(header, 0) != BlockSize || block_count < 2 ||
        block_count > 0x7ffffff || Get64(header, 0x10) != block_count ||
        Get64(header, 0x20) != block_count || Get64(header, 0x08) != 0 ||
        Get64(header, 0x18) != 0) {
        return fail("unexpected core geometry");
    }
    const u32 n = static_cast<u32>(block_count);
    if (fat.size() < (static_cast<std::size_t>(n) + 1) * 8) {
        return fail("allocation table too small");
    }
    if (raw_size < block_count * BlockSize) {
        return fail("block area too small");
    }
    CoreParser parser(fat, n, raw, error);
    if (!parser.LoadTables(Get32(header, 0x28), Get32(header, 0x2c))) {
        return std::nullopt;
    }
    return parser.Tree();
}

void SortTree(TreeDir& dir) {
    std::sort(dir.files.begin(), dir.files.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    std::sort(dir.dirs.begin(), dir.dirs.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& sub : dir.dirs) {
        SortTree(sub.second);
    }
}

} // namespace FileSys::SaveImage

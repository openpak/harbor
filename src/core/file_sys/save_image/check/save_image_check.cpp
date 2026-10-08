// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// harbor-save-image-check: the save-image codec's self-check, built with
// -DHARBOR_SAVE_IMAGE_CHECK=ON. Run it from a directory that has a user/ directory (the portable
// layout; logs go to ./user/log/eden_log.txt); it works under ./savecheck/.
//
//   harbor-save-image-check selftest
//       Host folder → stream → host folder round trips (small, large, nested, empty, many
//       entries, zero-length files, the .yuzu_save_size host file), determinism, the stream
//       layout (thumbnail, 0x48 control area, allocation table, padding to 0x4000, blocks),
//       the core's on-disk values against the 23.0.1 FS format, saves that do not fit, invalid
//       names, and a corrupt stream leaving the target untouched.
//       Prints "selftest: all checks passed" and exits 0, or names the failed check and exits 1.
//
//   harbor-save-image-check export <dir> <data_size> <stream> [save_data_type]
//   harbor-save-image-check import <stream> <data_size> <dir> [save_data_type]
//       Export a host directory to a stream file / import a stream into a host directory.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "common/logging.h"
#include "core/file_sys/save_image/save_image.h"
#include "core/file_sys/save_image/save_image_core.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/file_sys/vfs/vfs_vector.h"

namespace fs = std::filesystem;
using namespace FileSys;
using namespace FileSys::SaveImage;

namespace {

int g_failures = 0;

void Check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "  ok  " : "  FAIL", what.c_str());
    if (!ok) {
        ++g_failures;
    }
}

u32 Get32(const std::vector<u8>& b, std::size_t off) {
    u32 v;
    std::memcpy(&v, b.data() + off, 4);
    return v;
}
u64 Get64(const std::vector<u8>& b, std::size_t off) {
    u64 v;
    std::memcpy(&v, b.data() + off, 8);
    return v;
}

std::vector<u8> Pattern(std::size_t size, u32 seed) {
    std::vector<u8> out(size);
    std::mt19937 rng(seed);
    for (auto& b : out) {
        b = static_cast<u8>(rng());
    }
    return out;
}

std::vector<u8> Concatenate(const ExportedImage& image) {
    std::vector<u8> out;
    for (const auto& [kind, file] : image.storages) {
        const auto bytes = file->ReadAllBytes();
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
    return out;
}

bool WriteTreeTo(const VirtualDir& dir, const TreeDir& tree) {
    for (const auto& [name, data] : tree.files) {
        const auto file = dir->CreateFile(name);
        if (file == nullptr || !file->Resize(data.size()) ||
            (!data.empty() && file->Write(data.data(), data.size(), 0) != data.size())) {
            return false;
        }
    }
    for (const auto& [name, sub] : tree.dirs) {
        const auto child = dir->CreateSubdirectory(name);
        if (child == nullptr || !WriteTreeTo(child, sub)) {
            return false;
        }
    }
    return true;
}

TreeDir ReadTreeFrom(const VirtualDir& dir, bool skip_host_file) {
    TreeDir out;
    for (const auto& file : dir->GetFiles()) {
        if (skip_host_file && file->GetName() == ".yuzu_save_size") {
            continue;
        }
        out.files.emplace_back(file->GetName(), file->ReadAllBytes());
    }
    for (const auto& sub : dir->GetSubdirectories()) {
        out.dirs.emplace_back(sub->GetName(), ReadTreeFrom(sub, false));
    }
    SortTree(out);
    return out;
}

class Scratch {
public:
    Scratch() : root{fs::current_path() / "savecheck"} {
        fs::remove_all(root);
        fs::create_directories(root);
    }
    VirtualDir Dir(const std::string& name) {
        fs::create_directories(root / name);
        return vfs.OpenDirectory((root / name).string(), OpenMode::ReadWrite);
    }
    RealVfsFilesystem vfs;
    fs::path root;
};

/// Writes `tree` to a fresh directory, exports it, imports the stream into another fresh
/// directory and compares. Returns the stream.
std::vector<u8> RoundTrip(Scratch& scratch, const std::string& label, TreeDir tree,
                          const SaveGeometry& geometry) {
    SortTree(tree);
    const auto src = scratch.Dir(label + "-src");
    Check(WriteTreeTo(src, tree), label + ": source written");
    const auto exported = ExportSaveImage(src, geometry);
    Check(exported.has_value(), label + ": exported");
    if (!exported) {
        return {};
    }
    auto stream = Concatenate(*exported);
    const auto again = ExportSaveImage(src, geometry);
    Check(again && Concatenate(*again) == stream, label + ": export is deterministic");
    const auto dst = scratch.Dir(label + "-dst");
    const auto stream_file = std::make_shared<VectorVfsFile>(stream, "stream");
    Check(ImportSaveImage(stream_file, exported->parameters, geometry, dst), label + ": imported");
    Check(ReadTreeFrom(dst, false) == tree, label + ": identical tree and bytes");
    return stream;
}

TreeDir SmallTree() {
    TreeDir t;
    t.files.emplace_back("save.dat", Pattern(1234, 1));
    t.files.emplace_back("system.bin", Pattern(64, 2));
    return t;
}

TreeDir LargeTree() {
    TreeDir t;
    t.files.emplace_back("world.bin", Pattern(5 * 1024 * 1024 + 17, 3));
    t.files.emplace_back("exact.bin", Pattern(3 * 0x4000, 4));
    t.files.emplace_back("one.bin", Pattern(1, 5));
    return t;
}

TreeDir NestedTree() {
    TreeDir t;
    TreeDir* cur = &t;
    for (int depth = 0; depth < 12; ++depth) {
        cur->files.emplace_back("f" + std::to_string(depth), Pattern(100 + depth * 900, 10 + depth));
        cur->files.emplace_back("empty", std::vector<u8>{});
        TreeDir side;
        side.files.emplace_back("side.txt", Pattern(33, 99 + depth));
        cur->dirs.emplace_back("side" + std::to_string(depth), std::move(side));
        cur->dirs.emplace_back("d" + std::to_string(depth), TreeDir{});
        cur = &cur->dirs.back().second;
    }
    cur->dirs.emplace_back("empty_dir", TreeDir{});
    cur->files.emplace_back(std::string(64, 'n'), Pattern(77, 7)); // the longest name
    return t;
}

TreeDir ManyTree() {
    // More entries than one table block holds (0x4000 / 0x60 = 170): both tables grow.
    TreeDir t;
    for (int i = 0; i < 400; ++i) {
        t.files.emplace_back("file_" + std::to_string(i), Pattern(i % 7 == 0 ? 0 : 50 + i, i));
    }
    for (int i = 0; i < 200; ++i) {
        TreeDir d;
        d.files.emplace_back("x", Pattern(10, 1000 + i));
        t.dirs.emplace_back("dir_" + std::to_string(i), std::move(d));
    }
    return t;
}

int SelfTest() {
    Scratch scratch;
    const SaveGeometry account{0x400000, 0x100000, 1};

    // Geometry: what nnSdk formats for DataSize.
    const auto g = CoreGeometryFor(0x400000);
    Check(g && g->block_count == 0x100 && g->fat_size == 0x4000, "geometry for 4 MiB");
    const auto g2 = CoreGeometryFor(0x4001);
    Check(g2 && g2->block_count == 2, "DataSize rounds up to whole blocks");
    Check(!CoreGeometryFor(0x4000), "one block is refused (the core needs two)");
    const auto g3 = CoreGeometryFor(0x10000000);
    Check(g3 && g3->fat_size == 0x24000, "allocation table region for 256 MiB");

    // The core the 23.0.1 FS formats for an empty save.
    {
        std::string error;
        const auto image = BuildCore(TreeDir{}, *g, &error);
        Check(image.has_value(), "empty core builds");
        if (image) {
            const std::vector<u8> c(image->control.begin(), image->control.end());
            Check(Get32(c, 0) == 0x45564153 && Get32(c, 4) == 0x60000, "SAVE v6 header");
            Check(Get64(c, 8) == 0x100 && Get64(c, 0x10) == 0x4000, "SAVE block count/size");
            Check(Get64(c, 0x18) == 0x4000 && Get64(c, 0x28) == 0x100 && Get64(c, 0x38) == 0x100,
                  "allocation table header");
            Check(Get32(c, 0x40) == 0 && Get32(c, 0x44) == 1,
                  "directory table in block 0, file table in block 1");
            const auto& f = image->fat;
            Check(Get32(f, 0) == 0 && Get32(f, 4) == 3, "free list starts at block 2");
            Check(Get32(f, 8) == 0x80000000 && Get32(f, 0xc) == 0, "block 0: one-block list");
            Check(Get32(f, 0x10) == 0x80000000 && Get32(f, 0x14) == 0, "block 1: one-block list");
            Check(Get32(f, 0x18) == 0x80000000 && Get32(f, 0x1c) == 0x80000000 &&
                      Get32(f, 0x20) == 0x80000003 && Get32(f, 0x24) == 0x100 &&
                      Get32(f, 0x100 * 8) == 0x80000003 && Get32(f, 0x100 * 8 + 4) == 0x100,
                  "free segment 2..255 with its range entries");
            const auto& dirs = image->blocks.at(0);
            Check(Get32(dirs, 0) == 3 && Get32(dirs, 4) == 170 && Get32(dirs, 0x60 + 0x5c) == 2,
                  "directory table: 3 entries, capacity 170, root at index 2");
            const auto& files = image->blocks.at(1);
            Check(Get32(files, 0) == 2 && Get32(files, 4) == 170, "file table: empty");
            Check(image->blocks.size() == 2, "only the tables' blocks are used");
        }
    }

    // Round trips.
    {
        const auto stream = RoundTrip(scratch, "small", SmallTree(), account);
        const u64 fat = 0x4000;
        const u64 pre = ThumbnailSize + ControlAreaSize + fat;
        const u64 raw_offset = (pre + 0x3fff) & ~u64{0x3fff};
        Check(stream.size() == raw_offset + 0x400000, "stream size: thumbnail + 0x48 + FAT + "
                                                      "padding + blocks");
        Check(stream.size() > ThumbnailSize && Get32(stream, ThumbnailSize) == 0x45564153,
              "the control area follows the 0x40060-byte thumbnail");
        Check(raw_offset % 0x4000 == 0 && std::all_of(stream.begin() + pre,
                                                      stream.begin() + raw_offset,
                                                      [](u8 b) { return b == 0; }),
              "zero padding up to the 0x4000-aligned block area");
    }
    RoundTrip(scratch, "large", LargeTree(), SaveGeometry{0x800000, 0x100000, 1});
    RoundTrip(scratch, "nested", NestedTree(), account);
    RoundTrip(scratch, "empty", TreeDir{}, account);
    RoundTrip(scratch, "many", ManyTree(), SaveGeometry{0x1000000, 0x100000, 1});
    {
        const auto stream = RoundTrip(scratch, "bcat", SmallTree(), SaveGeometry{0x400000, 0, 2});
        Check(!stream.empty() && Get32(stream, 0) == 0x45564153,
              "a Bcat save's stream has no thumbnail");
    }
    {
        // Free blocks are zero ("WithZeroFree"): a full file removed leaves no trace.
        TreeDir t;
        t.files.emplace_back("a", Pattern(0x4000 * 3, 42));
        const auto src = scratch.Dir("zerofree");
        WriteTreeTo(src, t);
        const auto exported = ExportSaveImage(src, account);
        const auto stream = exported ? Concatenate(*exported) : std::vector<u8>{};
        const u64 raw = stream.size() - 0x400000;
        Check(!stream.empty() && std::all_of(stream.begin() + raw + 5 * 0x4000, stream.end(),
                                             [](u8 b) { return b == 0; }),
              "blocks past the used ones are zero");
        Check(exported && exported->parameters.hash_salt_enabled &&
                  exported->parameters.hash_algorithm == 0,
              "image parameters of a v5 save");
    }

    // The host's own .yuzu_save_size is not part of the save and survives an import.
    {
        const auto src = scratch.Dir("hostfile-src");
        WriteTreeTo(src, SmallTree());
        src->CreateFile(".yuzu_save_size")->WriteBytes(std::vector<u8>(16, 7));
        const auto exported = ExportSaveImage(src, account);
        const auto dst = scratch.Dir("hostfile-dst");
        dst->CreateFile(".yuzu_save_size")->WriteBytes(std::vector<u8>(16, 9));
        dst->CreateFile("stale.bin")->WriteBytes(std::vector<u8>(5, 1));
        const auto ok = exported && ImportSaveImage(std::make_shared<VectorVfsFile>(
                                                        Concatenate(*exported), "s"),
                                                    exported->parameters, account, dst);
        auto expected = SmallTree();
        SortTree(expected);
        Check(ok && ReadTreeFrom(dst, true) == expected, "host size file not exported");
        const auto kept = dst->GetFile(".yuzu_save_size");
        Check(kept && kept->ReadAllBytes() == std::vector<u8>(16, 9),
              "host size file kept, stale files replaced");
    }

    // Saves that cannot be built.
    {
        TreeDir big;
        big.files.emplace_back("big", Pattern(0x400000, 3));
        const auto src = scratch.Dir("toobig");
        WriteTreeTo(src, big);
        Check(!ExportSaveImage(src, account), "a save larger than DataSize is refused");
        TreeDir longname;
        longname.files.emplace_back(std::string(65, 'x'), Pattern(3, 3));
        const auto src2 = scratch.Dir("longname");
        WriteTreeTo(src2, longname);
        Check(!ExportSaveImage(src2, account), "a name over 64 bytes is refused");
    }

    // A corrupt or short stream leaves the target alone.
    {
        const auto src = scratch.Dir("corrupt-src");
        WriteTreeTo(src, SmallTree());
        const auto exported = ExportSaveImage(src, account);
        auto stream = Concatenate(*exported);
        const auto dst = scratch.Dir("corrupt-dst");
        dst->CreateFile("keep.bin")->WriteBytes(std::vector<u8>(8, 3));
        auto bad = stream;
        bad[ThumbnailSize] ^= 0xff;
        Check(!ImportSaveImage(std::make_shared<VectorVfsFile>(bad, "s"), {}, account, dst),
              "bad SAVE magic is refused");
        bad = stream;
        bad[ThumbnailSize + 0x48 + 0xf] = 0x7f; // the directory table's list leaves the table
        Check(!ImportSaveImage(std::make_shared<VectorVfsFile>(bad, "s"), {}, account, dst),
              "a broken allocation table is refused");
        stream.resize(stream.size() - 1);
        Check(!ImportSaveImage(std::make_shared<VectorVfsFile>(stream, "s"), {}, account, dst),
              "a short stream is refused");
        Check(dst->GetFile("keep.bin") != nullptr && dst->GetFiles().size() == 1,
              "the target directory is untouched");
    }

    fs::remove_all(scratch.root);
    if (g_failures == 0) {
        std::puts("selftest: all checks passed");
        return 0;
    }
    std::printf("selftest: %d check(s) failed\n", g_failures);
    return 1;
}

int ExportCmd(const std::string& dir, s64 size, const std::string& out, u8 type) {
    RealVfsFilesystem vfs;
    const auto src = vfs.OpenDirectory(dir, OpenMode::Read);
    const auto exported = ExportSaveImage(src, SaveGeometry{size, 0, type});
    if (!exported) {
        std::fputs("export failed (see the log)\n", stderr);
        return 1;
    }
    const auto bytes = Concatenate(*exported);
    std::FILE* f = std::fopen(out.c_str(), "wb");
    if (f == nullptr || std::fwrite(bytes.data(), 1, bytes.size(), f) != bytes.size()) {
        std::fputs("cannot write the stream\n", stderr);
        return 1;
    }
    std::fclose(f);
    std::printf("exported %zu bytes\n", bytes.size());
    return 0;
}

int ImportCmd(const std::string& stream_path, s64 size, const std::string& dir, u8 type) {
    RealVfsFilesystem vfs;
    const auto stream = vfs.OpenFile(stream_path, OpenMode::Read);
    fs::create_directories(dir);
    const auto dst = vfs.OpenDirectory(dir, OpenMode::ReadWrite);
    if (!ImportSaveImage(stream, {}, SaveGeometry{size, 0, type}, dst)) {
        std::fputs("import failed (see the log)\n", stderr);
        return 1;
    }
    std::puts("imported");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (!fs::is_directory(fs::current_path() / "user")) {
        std::fputs("run from a directory that has a user/ directory (the portable layout)\n",
                   stderr);
        return 64;
    }
    Common::Log::Initialize();
    Common::Log::SetColorConsoleBackendEnabled(true);
    Common::Log::Start();
    const std::string mode = argc > 1 ? argv[1] : "";
    const auto type = [&](int i) { return static_cast<u8>(argc > i ? std::stoi(argv[i]) : 1); };
    int status = 64;
    if (mode == "selftest") {
        status = SelfTest();
    } else if (mode == "export" && argc >= 5) {
        status = ExportCmd(argv[2], std::stoll(argv[3], nullptr, 0), argv[4], type(5));
    } else if (mode == "import" && argc >= 5) {
        status = ImportCmd(argv[2], std::stoll(argv[3], nullptr, 0), argv[4], type(5));
    } else {
        std::fputs("usage: harbor-save-image-check selftest\n"
                   "       harbor-save-image-check export <dir> <data_size> <stream> [type]\n"
                   "       harbor-save-image-check import <stream> <data_size> <dir> [type]\n",
                   stderr);
    }
    Common::Log::Stop();
    return status;
}

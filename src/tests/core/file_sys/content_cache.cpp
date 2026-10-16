// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include "common/fs/path_util.h"
#include "common/hex_util.h"
#include "common/logging.h"
#include "core/crypto/key_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_real.h"

namespace {
void Check(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
} // namespace

int main() {
    namespace FS = FileSys;
    using Json = nlohmann::json;
    try {
        Check(std::filesystem::is_directory(std::filesystem::current_path() / "user"),
              "run from an isolated portable directory containing user/");
        struct Logging {
            Logging() { Common::Log::Initialize(); Common::Log::Start(); }
            ~Logging() { Common::Log::Stop(); }
        } logging;
        const auto dir = std::filesystem::current_path() / "user" / "metadata-cache-test";
        std::filesystem::create_directories(dir);
        std::filesystem::create_directories(dir / "cache" / "container-metadata-v1");
        Common::FS::SetEdenPath(Common::FS::EdenPath::CacheDir, dir / "cache");
        const auto source = dir / "fixture.nsp";
        {
            std::ofstream output(source, std::ios::binary);
            for (unsigned int index = 0; index < 256; ++index) {
                output.put(static_cast<char>(index));
            }
        }
        const auto path = source.string();
        Core::Crypto::SHA256Hash hash{};
        u32 length{};
        EVP_Digest(path.data(), path.size(), hash.data(), &length, EVP_sha256(), nullptr);
        const auto cached = dir / "cache" / "container-metadata-v1" /
                            (Common::HexToString(hash) + "-all-0000000000000000.json");
        Json row = {{"offset", 32},          {"size", 16},
                    {"name", "fixture.nca"}, {"title_type", 0x81},
                    {"content_type", 1},     {"title_id", 0x0100000000000800ULL},
                    {"version", 42},         {"version_string", "1.2.3"}};
        Json cache = {
            {"schema", 1},
            {"path", path},
            {"size", 256},
            {"mtime", std::filesystem::last_write_time(source).time_since_epoch().count()},
            {"entries", Json::array({row})},
            {"tickets", Json::array()}};
        auto write_cache = [&](const Json& value) {
            std::ofstream output(cached);
            output << value;
        };
        FS::RealVfsFilesystem vfs;
        auto file = vfs.OpenFile(path, FS::OpenMode::Read);
        write_cache(cache);
        FS::ManualContentProvider provider;
        Check(provider.AddEntriesFromContainer(file), "valid cache was not used");
        const auto entry =
            provider.GetEntryRaw(0x0100000000000800ULL, FS::ContentRecordType::Program);
        Check(entry && entry->ReadBytes(16) == std::vector<u8>{32, 33, 34, 35, 36, 37, 38, 39,
                                                       40, 41, 42, 43, 44, 45, 46, 47},
              "cached offset/size incorrect");
        const auto updates = provider.ListUpdateVersions(0x0100000000000800ULL);
        Check(updates.size() == 1 && updates[0].version == 42 &&
                  updates[0].version_string == "1.2.3",
              "cached update version lost");
        Check(provider.GetSourceFiles().contains(path), "source tracking lost");
        auto reject = [&](Json value, const char* why) {
            provider.ClearAllEntries();
            write_cache(value);
            Check(!provider.AddEntriesFromContainer(file), why);
            Check(provider.ListEntries().empty(), "invalid cache left partial entries");
        };
        auto bad = cache;
        bad["size"] = 257;
        reject(bad, "changed size accepted");
        bad = cache;
        bad["mtime"] = 0;
        reject(bad, "changed timestamp accepted");
        bad = cache;
        bad["schema"] = 2;
        reject(bad, "unknown schema accepted");
        bad = cache;
        bad["entries"][0]["offset"] = 257;
        reject(bad, "out of bounds accepted");
        bad = cache;
        bad["entries"][0]["size"] = 300;
        reject(bad, "overflowing size accepted");
        bad = cache;
        bad["entries"][0]["content_type"] = 255;
        reject(bad, "invalid enum accepted");
        bad = cache;
        bad["entries"].push_back({{"offset", 0}});
        reject(bad, "partial corrupt row accepted");
        bad = cache;
        bad["tickets"].push_back({{"offset", 0}, {"size", 16}, {"name", "fixture.tik"}});
        reject(bad, "invalid source ticket accepted");
        provider.ClearAllEntries();
        {
            std::ofstream output(cached);
            output << "broken JSON";
        }
        Check(!provider.AddEntriesFromContainer(file), "malformed cache accepted");
        std::filesystem::remove_all(dir);
        std::cout << "container metadata cache: all checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#define VMA_IMPLEMENTATION 1
#include "video_core/vulkan_common/vma.h"
#pragma GCC diagnostic pop

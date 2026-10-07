// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"

namespace Service::BCAT {

// A title's BCAT data as OpenPak's news service publishes it for emulators
// (GET /api/emulator/v1/bcat/titles/<tid>: a manifest of paths, sizes, sha256 and file URLs), shaped
// into the delivery cache a game reads through bcat:u: directories of files, each name at most 31
// characters. Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).

inline constexpr size_t MaxBcatNameLength = 31;
inline constexpr size_t MaxBcatEntries = 100;

struct BcatFile {
    std::string directory;
    std::string name;
    std::vector<u8> data;
};

struct BcatManifestEntry {
    std::string path;
    std::string sha256;
    std::string url;
};

// A manifest path as the cache's directory and file name, when it is one.
bool SplitBcatPath(std::string_view path, std::string& directory, std::string& name);

// Whether a manifest's valid_from/valid_until window contains `now`. Throws on a manifest that is not
// JSON.
bool BcatInWindow(const std::string& manifest, std::chrono::system_clock::time_point now);

// The manifest's files: path, sha256 and URL. Throws on a manifest that is not JSON or has a file
// without them.
std::vector<BcatManifestEntry> BcatManifestEntries(const std::string& manifest);

// The directories the files fall into, in the order they first appear, at most 100 of each; names
// that differ only by case count as one, the first wins.
struct BcatGroup {
    std::string directory;
    std::vector<BcatFile> files;
};
std::vector<BcatGroup> GroupBcatFiles(const std::vector<BcatFile>& files);

std::string Sha256Hex(const std::vector<u8>& data);

} // namespace Service::BCAT

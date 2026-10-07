// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later
// Derived from openpak-client 3fd9ba7 (bcat.h).

// A title's BCAT data, as the news service publishes it for emulators
// (GET /api/emulator/v1/bcat/titles/<tid>: a manifest of paths, sizes, sha256 and file URLs),
// shaped for the delivery cache a game reads through bcat:u. Ported from Ryujinx's
// OpenPakBcat.cs.
//
// The delivery cache is two levels deep -- directories of files, each name at most 31
// characters -- so a manifest path is <directory>/<file> or it is not delivered. A dataset is
// taken whole or not at all (every file is checked against its sha256), and the last one fetched
// is kept on disk so a title started offline still gets it inside its validity window.

#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace OpenPak::Bcat {

inline constexpr std::size_t MaxNameLength = 31;

struct File {
    std::string directory;
    std::string name;
    std::vector<std::uint8_t> data;
};

enum class Outcome {
    Dataset,     ///< The service holds a dataset for this title and it is in its window.
    None,        ///< The service answered that there is nothing to deliver.
    Unreachable, ///< The service could not be asked, and nothing usable is cached.
};

struct Result {
    Outcome outcome = Outcome::Unreachable;
    std::vector<File> files;
};

/// What this title's delivery cache should hold: the service's dataset, or the cached one when
/// the service cannot be reached. Blocks for the downloads; keep it off a guest's IPC thread.
Result Fetch(std::uint64_t title_id);

/// A manifest path as the cache's directory and file name, when it is one.
bool Split(const std::string& path, std::string& directory, std::string& name);

/// Whether a manifest's valid_from / valid_until window contains `now`.
bool InWindow(const std::string& manifest, std::time_t now);

} // namespace OpenPak::Bcat

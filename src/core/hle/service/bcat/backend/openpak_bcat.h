// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <string>
#include <vector>

#include "common/common_types.h"
#include "core/openpak/bcat.h"

namespace Service::BCAT {

// The delivery cache is two levels deep: directories of files, at most 100 of each. A dataset
// (OpenPak::Bcat::Fetch) is shaped into it here. Behaviour follows the Ryujinx Harbor (MIT-licensed
// Ryujinx base).
inline constexpr size_t MaxBcatEntries = 100;

struct BcatGroup {
    std::string directory;
    std::vector<OpenPak::Bcat::File> files;
};

// The directories the files fall into, in the order they first appear, at most 100 of each; names
// that differ only by case count as one, the first wins.
std::vector<BcatGroup> GroupBcatFiles(const std::vector<OpenPak::Bcat::File>& files);

} // namespace Service::BCAT

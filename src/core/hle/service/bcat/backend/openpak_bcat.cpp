// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include "core/hle/service/bcat/backend/openpak_bcat.h"

#include <algorithm>
#include <cctype>

// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).

namespace Service::BCAT {
namespace {

bool IEquals(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

} // namespace

std::vector<BcatGroup> GroupBcatFiles(const std::vector<OpenPak::Bcat::File>& files) {
    std::vector<BcatGroup> groups;

    for (const auto& file : files) {
        auto group = std::find_if(groups.begin(), groups.end(), [&](const BcatGroup& other) {
            return IEquals(other.directory, file.directory);
        });
        if (group == groups.end()) {
            if (groups.size() == MaxBcatEntries) {
                continue;
            }
            groups.push_back({file.directory, {}});
            group = groups.end() - 1;
        }

        const bool known = std::any_of(group->files.begin(), group->files.end(),
                                       [&](const OpenPak::Bcat::File& other) {
                                           return IEquals(other.name, file.name);
                                       });
        if (group->files.size() < MaxBcatEntries && !known) {
            group->files.push_back(file);
        }
    }
    return groups;
}

} // namespace Service::BCAT

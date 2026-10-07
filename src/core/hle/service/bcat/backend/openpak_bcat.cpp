// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include "core/hle/service/bcat/backend/openpak_bcat.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <optional>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>

// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).

namespace Service::BCAT {
namespace {

bool ValidName(std::string_view name) {
    if (name.empty() || name.size() > MaxBcatNameLength || name == "." || name == "..") {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-' || c == '.';
    });
}

bool IEquals(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

// "2026-09-01T00:00:00Z" (a fraction and a +00:00 offset are tolerated), or nothing.
std::optional<std::chrono::system_clock::time_point> ParseTime(const nlohmann::json& value) {
    if (!value.is_string()) {
        return std::nullopt;
    }
    const std::string text = value.get<std::string>();
    int year, month, day, hour = 0, minute = 0, second = 0;
    if (std::sscanf(text.c_str(), "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute,
                    &second) < 3) {
        return std::nullopt;
    }

    using namespace std::chrono;
    const year_month_day date{std::chrono::year{year}, std::chrono::month{static_cast<unsigned>(month)},
                              std::chrono::day{static_cast<unsigned>(day)}};
    if (!date.ok()) {
        return std::nullopt;
    }
    return sys_days{date} + hours{hour} + minutes{minute} + seconds{second};
}

} // namespace

bool SplitBcatPath(std::string_view path, std::string& directory, std::string& name) {
    const size_t slash = path.find('/');
    if (slash == std::string_view::npos || path.find('/', slash + 1) != std::string_view::npos) {
        return false;
    }

    const auto dir = path.substr(0, slash);
    const auto file = path.substr(slash + 1);
    if (!ValidName(dir) || !ValidName(file)) {
        return false;
    }

    directory = std::string(dir);
    name = std::string(file);
    return true;
}

bool BcatInWindow(const std::string& manifest, std::chrono::system_clock::time_point now) {
    const auto root = nlohmann::json::parse(manifest);

    if (root.contains("valid_from")) {
        if (const auto start = ParseTime(root["valid_from"]); start && now < *start) {
            return false;
        }
    }
    if (root.contains("valid_until")) {
        if (const auto end = ParseTime(root["valid_until"]); end && now >= *end) {
            return false;
        }
    }
    return true;
}

std::vector<BcatManifestEntry> BcatManifestEntries(const std::string& manifest) {
    const auto root = nlohmann::json::parse(manifest);

    std::vector<BcatManifestEntry> entries;
    if (root.contains("files") && root["files"].is_array()) {
        for (const auto& file : root["files"]) {
            entries.push_back({file.at("path").get<std::string>(), file.at("sha256").get<std::string>(),
                               file.at("url").get<std::string>()});
        }
    }
    return entries;
}

std::vector<BcatGroup> GroupBcatFiles(const std::vector<BcatFile>& files) {
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
                                       [&](const BcatFile& other) { return IEquals(other.name, file.name); });
        if (group->files.size() < MaxBcatEntries && !known) {
            group->files.push_back(file);
        }
    }
    return groups;
}

std::string Sha256Hex(const std::vector<u8>& data) {
    u8 digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    EVP_Digest(data.data(), data.size(), digest, &length, EVP_sha256(), nullptr);

    std::string hex;
    for (unsigned int i = 0; i < length; ++i) {
        char byte[3];
        std::snprintf(byte, sizeof(byte), "%02x", digest[i]);
        hex += byte;
    }
    return hex;
}

} // namespace Service::BCAT

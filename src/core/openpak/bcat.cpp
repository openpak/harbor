// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later
// Derived from openpak-client 3fd9ba7 (bcat.cpp).

// Ported from Ryujinx's OpenPakBcat.cs.

#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>

#include <fmt/format.h>
#include "common/httplib.h"
#include <nlohmann/json.hpp>
#include <openssl/sha.h>

#include "core/openpak/bcat.h"
#include "core/openpak/network_profile.h"
#include "common/logging.h"
#include "core/openpak/platform.h"

namespace OpenPak::Bcat {

namespace {

using Json = nlohmann::json;

std::filesystem::path CacheDirectory(const std::string& title) {
    return Platform::CacheDir() / "bcat" / title;
}

bool ValidName(const std::string& name) {
    if (name.empty() || name.size() > MaxNameLength || name == "." || name == "..") {
        return false;
    }
    for (const unsigned char c : name) {
        if (!std::isalnum(c) && c != '_' && c != '-' && c != '.') {
            return false;
        }
    }
    return true;
}

/// RFC 3339 UTC ("2026-09-23T00:00:00Z") as seconds, or nothing.
std::optional<std::time_t> ParseTime(const Json& value) {
    if (!value.is_string()) {
        return std::nullopt;
    }
    std::tm tm{};
    const std::string text = value.get<std::string>();
    if (std::sscanf(text.c_str(), "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                    &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6) {
        return std::nullopt;
    }
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
#ifdef _WIN32
    return _mkgmtime(&tm);
#else
    return timegm(&tm);
#endif
}

std::string Sha256Hex(const std::string& data) {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest.data());
    std::string out;
    for (const unsigned char byte : digest) {
        out += fmt::format("{:02x}", byte);
    }
    return out;
}

std::unique_ptr<httplib::Client> ClientFor(const std::string& base) {
    auto client = std::make_unique<httplib::Client>(base);
    client->set_connection_timeout(10);
    client->set_read_timeout(10);
    client->set_follow_location(true);
    Platform::ApplySystemCa(*client);
    if (std::string header = Platform::ClientHeader(); !header.empty()) {
        client->set_default_headers({{"X-OpenPak-Client", std::move(header)}});
    }
    return client;
}

/// GET a URL that is either a path on the website or absolute. Nothing on any failure.
std::optional<std::string> Download(const std::string& url) {
    std::string base = NetworkProfile::Site();
    std::string path = url;
    if (url.starts_with("http://") || url.starts_with("https://")) {
        const std::size_t path_at = url.find('/', url.find("://") + 3);
        base = url.substr(0, path_at);
        path = path_at == std::string::npos ? "/" : url.substr(path_at);
    }
    const auto result = ClientFor(base)->Get(path);
    if (!result || result->status != 200) {
        return std::nullopt;
    }
    return result->body;
}

void Forget(const std::string& title) {
    std::error_code ec;
    std::filesystem::remove_all(CacheDirectory(title), ec);
}

void Store(const std::string& title, const std::string& manifest, const std::vector<File>& files) {
    Forget(title);
    const auto root = CacheDirectory(title);
    for (const File& file : files) {
        Platform::WriteFile(root / "files" / file.directory / file.name,
                            std::string(file.data.begin(), file.data.end()));
    }
    Platform::WriteFile(root / "manifest.json", manifest);
}

std::optional<std::vector<File>> Cached(const std::string& title) {
    const auto root = CacheDirectory(title);
    const std::string manifest = Platform::ReadFile(root / "manifest.json");
    if (manifest.empty() || !InWindow(manifest, std::time(nullptr))) {
        return std::nullopt;
    }
    const Json json = Json::parse(manifest, nullptr, false);
    std::vector<File> files;
    for (const auto& entry : json.value("files", Json::array())) {
        File file;
        if (!Split(entry.value("path", ""), file.directory, file.name)) {
            continue;
        }
        std::ifstream in(root / "files" / file.directory / file.name, std::ios::binary);
        if (!in) {
            return std::nullopt;
        }
        file.data.assign(std::istreambuf_iterator<char>(in), {});
        files.push_back(std::move(file));
    }
    return files;
}

Result FromCache(const std::string& title) {
    if (auto files = Cached(title)) {
        return {Outcome::Dataset, std::move(*files)};
    }
    return {Outcome::Unreachable, {}};
}

} // namespace

bool Split(const std::string& path, std::string& directory, std::string& name) {
    const std::size_t slash = path.find('/');
    if (slash == std::string::npos || path.find('/', slash + 1) != std::string::npos) {
        return false;
    }
    std::string dir = path.substr(0, slash);
    std::string file = path.substr(slash + 1);
    if (!ValidName(dir) || !ValidName(file)) {
        return false;
    }
    directory = std::move(dir);
    name = std::move(file);
    return true;
}

bool InWindow(const std::string& manifest, std::time_t now) {
    const Json json = Json::parse(manifest, nullptr, false);
    if (!json.is_object()) {
        return false;
    }
    if (const auto from = ParseTime(json.value("valid_from", Json{})); from && now < *from) {
        return false;
    }
    if (const auto until = ParseTime(json.value("valid_until", Json{})); until && now >= *until) {
        return false;
    }
    return true;
}

Result Fetch(std::uint64_t title_id) {
    const std::string title = fmt::format("{:016x}", title_id);

    try {
        const auto reply = ClientFor(NetworkProfile::Site())
                               ->Get(fmt::format("/api/emulator/v1/bcat/titles/{}", title));
        if (!reply) {
            return FromCache(title);
        }
        if (reply->status == 404) {
            Forget(title);
            return {Outcome::None, {}};
        }
        if (reply->status != 200 || !InWindow(reply->body, std::time(nullptr))) {
            if (reply->status == 200) {
                Forget(title);
                return {Outcome::None, {}};
            }
            return FromCache(title);
        }

        const Json manifest = Json::parse(reply->body);
        std::vector<File> files;

        for (const auto& entry : manifest.value("files", Json::array())) {
            const std::string path = entry.value("path", "");
            File file;
            if (!Split(path, file.directory, file.name)) {
                LOG_WARNING(WebService, "[OpenPak] {}: {} is not <directory>/<file> within 31 "
                                    "characters each; not delivered", title, path);
                continue;
            }
            const auto data = Download(entry.value("url", ""));
            if (!data || Sha256Hex(*data) != Platform::ToLower(entry.value("sha256", ""))) {
                LOG_INFO(WebService, "[OpenPak] BCAT data for {} not fetched: {} failed", title, path);
                return FromCache(title);
            }
            file.data.assign(data->begin(), data->end());
            files.push_back(std::move(file));
        }

        Store(title, reply->body, files);
        return {Outcome::Dataset, std::move(files)};
    } catch (const std::exception& e) {
        LOG_INFO(WebService, "[OpenPak] BCAT data for {} not fetched: {}", title, e.what());
        return FromCache(title);
    }
}

} // namespace OpenPak::Bcat

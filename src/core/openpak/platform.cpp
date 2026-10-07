// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Derived from openpak-client 3fd9ba7 (platform.cpp, and the CA handling of api.cpp).

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <mutex>
#include <sstream>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include "common/httplib.h"
#include "common/logging.h"
#include "core/openpak/platform.h"

#ifdef YUZU_BUNDLED_OPENSSL
#include <openssl/cert.h>
#endif

namespace OpenPak::Platform {

namespace {

std::mutex g_dirs_mutex;
std::filesystem::path g_data;
std::filesystem::path g_cache;

// Made on first use, not at load: Android's emulator session is a static object that may set the
// source while this file's statics are still being initialised.
struct ProfileSource {
    std::mutex mutex;
    std::function<std::string()> source;
};

ProfileSource& Profile() {
    static ProfileSource profile;
    return profile;
}

std::mutex g_client_mutex;
std::string g_client;

} // namespace

void SetDirectories(std::filesystem::path data_dir, std::filesystem::path cache_dir) {
    std::lock_guard lock{g_dirs_mutex};
    g_data = std::move(data_dir);
    g_cache = std::move(cache_dir);
}

std::filesystem::path DataDir() {
    std::lock_guard lock{g_dirs_mutex};
    return g_data;
}

std::filesystem::path CacheDir() {
    std::lock_guard lock{g_dirs_mutex};
    return g_cache.empty() ? g_data / "cache" : g_cache;
}

std::filesystem::path ServerJsonPath() {
    return DataDir() / "server.json";
}

std::filesystem::path CaPath() {
    return DataDir() / "ca.pem";
}

std::filesystem::path NetworkProfilePath() {
    return DataDir() / "network-profile.json";
}

std::filesystem::path CeilingPath() {
    return DataDir() / "network-ceiling.json";
}

std::filesystem::path CeilingVersionPath() {
    return DataDir() / "network-ceiling-version";
}

void SetProfileSource(std::function<std::string()> source) {
    std::lock_guard lock{Profile().mutex};
    Profile().source = std::move(source);
}

std::string ProfileId() {
    std::lock_guard lock{Profile().mutex};
    return Profile().source ? Profile().source() : std::string{};
}

std::string_view OsName() {
#if defined(__ANDROID__)
    return "android";
#elif defined(_WIN32)
    return "windows";
#elif defined(__APPLE__) && TARGET_OS_IPHONE
    return "ios";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return {};
#endif
}

void SetClient(std::string name, std::string version) {
    std::lock_guard lock{g_client_mutex};
    if (name.empty()) {
        g_client.clear();
        return;
    }
    g_client = name + "/" + (version.empty() ? "unknown" : version);
    if (const std::string_view os = OsName(); !os.empty()) {
        g_client += " (";
        g_client += os;
        g_client += ")";
    }
}

std::string ClientHeader() {
    std::lock_guard lock{g_client_mutex};
    return g_client;
}

bool CreateParentDirs(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    return !ec;
}

bool WriteFile(const std::filesystem::path& path, const std::string& contents) {
    CreateParentDirs(path);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        return false;
    }
    f.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    return static_cast<bool>(f);
}

bool WriteAtomically(const std::filesystem::path& path, const std::string& contents) {
    CreateParentDirs(path);
    const auto temp = std::filesystem::path(path.string() + ".tmp");
    {
        std::ofstream f(temp, std::ios::binary | std::ios::trunc);
        if (!f) {
            return false;
        }
        f.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!f) {
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::permissions(temp,
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, ec);
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return false;
    }
    return true;
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string StripSpaces(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) {
        return {};
    }
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

namespace {

template <typename C>
void ApplyRoots(C& client) {
#ifdef YUZU_BUNDLED_OPENSSL
    client.load_ca_cert_store(kCert, sizeof(kCert));
#elif defined(__linux__) && !defined(__ANDROID__)
    static constexpr std::array<const char*, 4> candidates{
        "/etc/ssl/certs/ca-certificates.crt", // Debian/Ubuntu/Arch
        "/etc/pki/tls/certs/ca-bundle.crt",   // Fedora/RHEL/CentOS
        "/etc/ssl/cert.pem",                  // Alpine
        "/etc/ssl/ca-bundle.pem",             // openSUSE
    };
    for (const char* path : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) {
            client.set_ca_cert_path(path);
            return;
        }
    }
    LOG_ERROR(WebService, "[OpenPak] No known system CA bundle found");
#else
    (void)client;
#endif
}

} // namespace

void ApplySystemCa(httplib::Client& client) {
    ApplyRoots(client);
}

void ApplySystemCa(httplib::SSLClient& client) {
    ApplyRoots(client);
}

} // namespace OpenPak::Platform

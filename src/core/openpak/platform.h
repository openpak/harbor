// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Derived from openpak-client 3fd9ba7 (platform.h, and the CA handling of api.cpp).

// What the OpenPak layer needs from the host that is not the network: where its files live, who
// the active firmware user is, which build is asking, and the public roots for openpak.org.

#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace httplib {
class Client;
class SSLClient;
} // namespace httplib

namespace OpenPak::Platform {

// Set once by BootConnection::Run before any other call. `data_dir` is <user>/openpak and holds
// Harbor's files (below); `cache_dir` holds regenerable data (BCAT datasets).
void SetDirectories(std::filesystem::path data_dir, std::filesystem::path cache_dir);
std::filesystem::path DataDir();
std::filesystem::path CacheDir();

// Harbor's data-file contract, all under DataDir().
std::filesystem::path ServerJsonPath();          // server.json: website + console server
std::filesystem::path CaPath();                  // ca.pem: the console-facing CA, SHA-256 pinned
std::filesystem::path NetworkProfilePath();      // network-profile.json: {"etag", "profile"}
std::filesystem::path CeilingPath();             // network-ceiling.json: the signed envelope
std::filesystem::path CeilingVersionPath();      // network-ceiling-version: highest accepted

// Which firmware user the OpenPak identity speaks for when a caller names none: the uid acc has
// open, as the 32 hex digits acc prints a UserId as. Each user is its own BaaS device account.
// The acc service installs the source; no source, or an empty answer, means nobody is open.
// Asked each time it matters, so a switch is noticed wherever it happens.
void SetProfileSource(std::function<std::string()> source);
std::string ProfileId();

// "harbor", "<version>": every request to OpenPak says X-OpenPak-Client: harbor/<version> (<os>).
void SetClient(std::string name, std::string version);
std::string ClientHeader();
std::string_view OsName();

bool CreateParentDirs(const std::filesystem::path& path);
bool WriteFile(const std::filesystem::path& path, const std::string& contents);
// Written beside itself and renamed over the old copy, owner-only (0600) where the platform has
// permissions, so a crash leaves one whole copy or the other.
bool WriteAtomically(const std::filesystem::path& path, const std::string& contents);
std::string ReadFile(const std::filesystem::path& path);

std::string ToLower(std::string s);
std::string StripSpaces(const std::string& s);

// Public roots only, for openpak.org and the website: the bundled OpenSSL's blob, or the distro
// bundle on desktop Linux. Never the OpenPak CA.
void ApplySystemCa(httplib::Client& client);
void ApplySystemCa(httplib::SSLClient& client);

} // namespace OpenPak::Platform

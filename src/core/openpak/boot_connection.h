// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Harbor's connection to OpenPak, made by the host before the console boots.
//
// The data-file contract, under <user>/openpak/:
//   server.json              {"website": "https://openpak.org", "console_server": "<address>"}
//   ca.pem                   the console-facing CA, kept only when its SHA-256 is the profile's
//   network-profile.json     {"etag": ..., "profile": <the profile as the site sent it>}
//   network-ceiling.json     the signed ceiling envelope, re-verified on every load
//   network-ceiling-version  the highest ceiling version accepted (rollback guard)
//
// Run() is the one call a frontend makes, before Core::System::Load. It does the site check
// (server.json's website, https only; the default site on an install that never connected), the
// signed ceiling (pinned openpak.org origin) and the network profile fetched side by side, the CA
// pin, and server.json written on the first connect. Offline, everything comes from the stored
// files; an install that never connected stays offline (and every ceiling family fails closed)
// until a boot that can reach the site. A background re-check runs every 6 h (+-10 %).

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/openpak/network_profile.h"

namespace OpenPak {

struct BootResult {
    /// OpenPak is on for this process: a site is saved, the console server is known and ca.pem
    /// matches its pin. Off, the guest's OpenPak names still fail closed.
    bool enabled = false;
    /// The profile was fetched now (rather than read from network-profile.json or built in).
    bool online = false;
    std::string website;
    std::string console_server;        ///< The address the console-facing names go to.
    std::string profile_source;        ///< "fetched", "cached" or "built-in"
    long long profile_version = 0;
    std::string ceiling_source;        ///< "fetched", "cached" or "built-in"
    long long ceiling_version = 0;
    std::string error;                 ///< Why it is off or offline, for the log.
};

namespace BootConnection {

/// Connect for this process. `user_dir` is Eden's user directory (Common::FS EdenDir).
/// Blocking: about 2 s when the site does not answer, up to ~10 s on a first connect (the CA
/// download). Idempotent: later calls return the first result. Never throws.
BootResult Run(const std::filesystem::path& user_dir);

/// The result of the Run() that connected this process (default-constructed before it).
BootResult Last();

/// On the way out of Harbor: stop the heartbeat and the push, saying the active user is going.
void Stop();

} // namespace BootConnection

/// What the guest-facing services (sfdnsres, nsd, ssl, nifm) read. Cheap, thread-safe, never
/// touches the network.
namespace Network {

/// The platform the console redirects for.
inline constexpr const char* PlatformName = "switch";

/// Whether OpenPak is on (BootResult::enabled).
bool Enabled();

/// The address the console-facing names go to: the applied profile's server.address, else
/// server.json's console_server, else the website's host. Empty when there is none.
std::string ConsoleServer();

/// How the guest's resolver answers `host`: Redirect to an address, Blocked (fail closed: inside
/// a ceiling family with nowhere to go, whether or not OpenPak is on), or Direct (host DNS).
NetworkProfile::GuestName ResolveGuestName(std::string_view host);

/// The redirect alone (NetworkProfile::RedirectFor with the console server), or nothing.
std::optional<std::string> RedirectFor(std::string_view host);

/// The pinned CA: its path, its PEM as loaded at boot (empty when OpenPak is off), and the first
/// certificate in DER form for the guest's certificate store (cert 1033).
std::filesystem::path CaPath();
const std::string& CaPem();
std::vector<std::uint8_t> CaDer();

} // namespace Network

} // namespace OpenPak

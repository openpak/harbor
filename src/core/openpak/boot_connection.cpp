// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base): OpenPakHostSetup,
// OpenPakConfig, OpenPakBoot and the boot half of OpenPakNetworkProfile.

#include <cstdlib>
#include <mutex>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "common/logging.h"
#include "common/scm_rev.h"
#include "core/openpak/boot_connection.h"
#include "core/openpak/network_profile.h"
#include "core/openpak/platform.h"
#include "core/openpak/service_hooks.h"
#include "core/openpak/session.h"

namespace OpenPak {

namespace {

struct Boot {
    std::mutex mutex;
    bool ran = false;
    BootResult result;
    std::string website;
    std::string saved_console_server;
    std::string ca_pem;
};

Boot& Get() {
    static Boot boot;
    return boot;
}

std::string Env(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? Platform::StripSpaces(value) : std::string{};
}

/// An https:// address without a trailing slash; a bare host gets https:// in front. The site
/// check: anything else is not a site the console may be pointed at. Empty otherwise.
std::string Normalise(std::string website) {
    std::string text = Platform::StripSpaces(website);
    while (!text.empty() && text.back() == '/') {
        text.pop_back();
    }
    if (text.empty()) {
        return {};
    }
    if (text.find("://") == std::string::npos) {
        text = "https://" + text;
    }
    constexpr std::string_view scheme = "https://";
    if (Platform::ToLower(text.substr(0, scheme.size())) != scheme) {
        return {};
    }
    const std::string authority = text.substr(scheme.size());
    if (authority.empty() || authority.find_first_of("/?#@ ") != std::string::npos) {
        return {};
    }
    return text;
}

std::string HostOf(const std::string& website) {
    std::string host = website;
    if (const auto scheme = host.find("://"); scheme != std::string::npos) {
        host = host.substr(scheme + 3);
    }
    if (const auto slash = host.find('/'); slash != std::string::npos) {
        host = host.substr(0, slash);
    }
    if (const auto colon = host.rfind(':'); colon != std::string::npos && host.back() != ']') {
        host = host.substr(0, colon);
    }
    return host;
}

struct Saved {
    std::string website;
    std::string console_server;
};

Saved ReadServerJson() {
    Saved saved;
    const std::string text = Platform::ReadFile(Platform::ServerJsonPath());
    if (text.empty()) {
        return saved;
    }
    const nlohmann::json doc = nlohmann::json::parse(text, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        LOG_WARNING(WebService, "[OpenPak] Ignoring an unreadable server.json");
        return saved;
    }
    if (doc.contains("website") && doc.at("website").is_string()) {
        saved.website = Normalise(doc.at("website").get<std::string>());
    }
    if (doc.contains("console_server") && doc.at("console_server").is_string()) {
        saved.console_server = Platform::StripSpaces(doc.at("console_server").get<std::string>());
    }
    return saved;
}

bool WriteServerJson(const std::string& website, const std::string& console_server) {
    nlohmann::json doc = nlohmann::json::object();
    doc["website"] = website;
    if (!console_server.empty()) {
        doc["console_server"] = console_server;
    }
    return Platform::WriteAtomically(Platform::ServerJsonPath(), doc.dump(2));
}

/// The console server, as OpenPakConfig.ResolvedConsoleServer resolves it with the profile's
/// address winning. Called with the boot mutex held.
std::string ConsoleServerLocked(const Boot& boot, const NetworkProfile::Profile& profile) {
    if (std::string env = Env("OPENPAK_SERVER"); !env.empty()) {
        return env;
    }
    if (!profile.server_address.empty()) {
        return profile.server_address;
    }
    if (!boot.saved_console_server.empty()) {
        return boot.saved_console_server;
    }
    return boot.website.empty() ? std::string{} : HostOf(boot.website);
}

} // namespace

namespace BootConnection {

BootResult Run(const std::filesystem::path& user_dir) {
    Boot& boot = Get();
    std::lock_guard lock{boot.mutex};
    if (boot.ran) {
        return boot.result;
    }
    boot.ran = true;

    BootResult& out = boot.result;
    try {
        Platform::SetDirectories(user_dir / "openpak", user_dir / "cache" / "openpak");
        Platform::SetClient("harbor", Common::g_build_version);

        // The site check: the saved site, else (an install that never connected) the default.
        const Saved saved = ReadServerJson();
        std::string website = Normalise(Env("OPENPAK_WEBSITE"));
        if (website.empty()) {
            website = saved.website;
        }
        const bool first_connect = website.empty();
        if (first_connect) {
            website = NetworkProfile::DefaultSite;
        }
        boot.website = website;
        boot.saved_console_server = saved.console_server;
        out.website = website;
        NetworkProfile::SetSite(website);

        // The ceiling and the profile side by side, two seconds; the CA pinned on a fresh profile.
        const NetworkProfile::Result fetched = NetworkProfile::Fetch(Network::PlatformName);
        const NetworkProfile::Ceiling ceiling = NetworkProfile::CurrentCeiling();

        out.online = fetched.source == NetworkProfile::Source::Fetched;
        out.profile_source = NetworkProfile::SourceName(fetched.source);
        out.profile_version = fetched.profile.version;
        out.ceiling_version = ceiling.version;
        out.ceiling_source = ceiling.version == 0 ? "built-in" : "verified";
        out.error = fetched.error;
        out.console_server = ConsoleServerLocked(boot, fetched.profile);

        // Trusted only as the bytes the profile pins: a CA on disk that does not match the
        // applied profile is not the console server's.
        const std::string pem = Platform::ReadFile(Platform::CaPath());
        const bool pinned =
            fetched.source != NetworkProfile::Source::BuiltIn && !fetched.profile.ca_sha256.empty()
                ? NetworkProfile::CaMatchesPin(pem, fetched.profile.ca_sha256)
                : false;

        if (fetched.source == NetworkProfile::Source::BuiltIn || !pinned) {
            out.enabled = false;
            if (out.error.empty()) {
                out.error = fmt::format("{} handed out no certificate authority Harbor could check",
                                        website);
            }
            LOG_WARNING(WebService, "[OpenPak] Not connected to {}: {}; the console stays offline",
                        website, out.error);
        } else if (out.console_server.empty()) {
            out.enabled = false;
            out.error = "no console server address";
            LOG_WARNING(WebService, "[OpenPak] Not connected: no console server address");
        } else {
            out.enabled = true;
            boot.ca_pem = pem;
            // The console-facing edge is the profile's address, not the site's host (the site may
            // sit behind a CDN). Saved so an offline boot goes to the same place.
            if (first_connect || saved.console_server != out.console_server ||
                saved.website != website) {
                if (!WriteServerJson(website, out.console_server)) {
                    LOG_WARNING(WebService, "[OpenPak] Could not write server.json");
                }
            }
            Session::Configure(out.console_server, 443, Platform::CaPath().string());
            Session::StartHeartbeat({});
        }

        NetworkProfile::StartWatching(Network::PlatformName);
        InstallServiceHooks();

        LOG_INFO(WebService,
                 "[OpenPak] Boot connection: {} ({}), profile {} v{}, ceiling {} v{}, console "
                 "server {}",
                 out.enabled ? "on" : "off", out.online ? "online" : "offline", out.profile_source,
                 out.profile_version, out.ceiling_source, out.ceiling_version,
                 out.console_server.empty() ? std::string{"none"} : out.console_server);
    } catch (const std::exception& e) {
        out.enabled = false;
        out.error = e.what();
        LOG_ERROR(WebService, "[OpenPak] Boot connection failed: {}", e.what());
    }
    return out;
}

BootResult Last() {
    Boot& boot = Get();
    std::lock_guard lock{boot.mutex};
    return boot.result;
}

void Stop() {
    if (Last().enabled) {
        Session::GoOffline();
    }
}

} // namespace BootConnection

namespace Network {

bool Enabled() {
    return BootConnection::Last().enabled;
}

std::string ConsoleServer() {
    return BootConnection::Last().console_server;
}

NetworkProfile::GuestName ResolveGuestName(std::string_view host) {
    return NetworkProfile::ClassifyGuestName(std::string{host}, PlatformName, ConsoleServer());
}

std::optional<std::string> RedirectFor(std::string_view host) {
    return NetworkProfile::RedirectFor(std::string{host}, ConsoleServer());
}

std::filesystem::path CaPath() {
    return OpenPak::Platform::CaPath();
}

const std::string& CaPem() {
    // Written once, under the boot mutex, before any guest service can ask.
    return Get().ca_pem;
}

std::vector<std::uint8_t> CaDer() {
    return Enabled() ? Session::CaCertificateDer() : std::vector<std::uint8_t>{};
}

} // namespace Network

} // namespace OpenPak

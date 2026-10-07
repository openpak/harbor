// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later
// Derived from openpak-client 3fd9ba7 (network_profile_fetch.cpp).
//
// The network profile, implemented against the contract in network_profile.h
// (emulators/prds/emulator-network-profile-prd.md §2 and §3, docs/signed-ceiling.md). One
// conditional GET, two seconds, no retries; the signed ceiling (network_ceiling.cpp) bounds what
// is applied, name by name; last-known-good beats a fetch and the built-in default beats nothing.
// The stored layout and the CA pin follow the Ryujinx Harbor (MIT-licensed Ryujinx base).

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <future>
#include <mutex>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <openssl/sha.h>

#include "common/httplib.h" // brings the platform's socket headers (inet_pton) on Windows too
#include "common/logging.h"
#include "core/openpak/network_profile.h"
#include "core/openpak/platform.h"

namespace OpenPak::NetworkProfile {

namespace {

bool IsLiteralAddress(std::string const& address) {
    in_addr v4{};
    in6_addr v6{};
    return inet_pton(AF_INET, address.c_str(), &v4) == 1 ||
           inet_pton(AF_INET6, address.c_str(), &v6) == 1;
}

bool IsLoopbackOrLinkLocal(std::string const& address) {
    unsigned char v4[4]{};
    unsigned char v6[16]{};
    if (inet_pton(AF_INET, address.c_str(), v4) == 1) {
        return v4[0] == 127 ||                            // 127.0.0.0/8
               (v4[0] == 169 && v4[1] == 254) ||          // 169.254.0.0/16
               v4[0] == 0 ||                              // this-network
               (v4[0] == 255 && v4[1] == 255 && v4[2] == 255 && v4[3] == 255);  // broadcast
    }
    if (inet_pton(AF_INET6, address.c_str(), v6) == 1) {
        bool zero_prefix = true;  // :: and ::1 share fifteen zero bytes
        for (int i = 0; i < 15; ++i) {
            zero_prefix = zero_prefix && v6[i] == 0;
        }
        if (zero_prefix && (v6[15] == 0 || v6[15] == 1)) {
            return true;
        }
        return v6[0] == 0xFE && (v6[1] & 0xC0) == 0x80;  // fe80::/10
    }
    return true;  // not an address at all: treat as unusable
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

// A pattern starting with a dot matches every name under it; anything else matches one name.
// Case-insensitive, as DNS is.
bool Matches(std::string const& host, std::string const& pattern) {
    std::string const h = Lower(host);
    std::string const p = Lower(pattern);
    if (p.empty()) {
        return false;
    }
    if (p.front() == '.') {
        return h.size() > p.size() && h.compare(h.size() - p.size(), p.size(), p) == 0;
    }
    return h == p;
}

std::string Sha256Hex(std::string const& data) {
    std::uint8_t digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<unsigned char const*>(data.data()), data.size(), digest);
    std::string hex;
    hex.reserve(SHA256_DIGEST_LENGTH * 2);
    for (std::uint8_t byte : digest) {
        hex += fmt::format("{:02x}", byte);
    }
    return hex;
}

// "https://host[:port]/path" split into the origin httplib connects to and the path it asks for.
// Only https: the CA is a trust anchor, and a URL that is not https never names one.
std::optional<std::pair<std::string, std::string>> SplitHttpsUrl(std::string const& url) {
    constexpr std::string_view scheme = "https://";
    if (url.size() <= scheme.size() || Lower(url.substr(0, scheme.size())) != scheme) {
        return std::nullopt;
    }
    std::size_t const slash = url.find('/', scheme.size());
    std::string origin = url.substr(0, slash);
    std::string path = slash == std::string::npos ? "/" : url.substr(slash);
    if (origin.size() == scheme.size()) {
        return std::nullopt;
    }
    return std::pair{std::move(origin), std::move(path)};
}

httplib::Headers ClientHeaders() {
    httplib::Headers headers;
    if (std::string header = Platform::ClientHeader(); !header.empty()) {
        headers.emplace("X-OpenPak-Client", std::move(header));
    }
    return headers;
}

std::mutex g_applied_mutex;
std::optional<Result> g_applied;

std::mutex g_site_mutex;
std::string g_site = DefaultSite;

struct Stored {
    std::string body;  // the profile exactly as the server sent it
    std::string etag;
};

std::optional<Stored> ReadStored() {
    std::string const text = Platform::ReadFile(Platform::NetworkProfilePath());
    if (text.empty()) {
        return std::nullopt;
    }
    nlohmann::json const doc = nlohmann::json::parse(text, nullptr, false);
    if (doc.is_discarded() || !doc.is_object() || !doc.contains("profile") ||
        !doc.at("profile").is_object()) {
        LOG_WARNING(WebService, "[OpenPak] network profile: the stored copy is unreadable");
        return std::nullopt;
    }
    Stored stored;
    stored.body = doc.at("profile").dump();
    if (doc.contains("etag") && doc.at("etag").is_string()) {
        stored.etag = doc.at("etag").get<std::string>();
    }
    return stored;
}

}  // namespace

void SetSite(std::string website) {
    std::lock_guard lock{g_site_mutex};
    g_site = website.empty() ? std::string{DefaultSite} : std::move(website);
}

std::string Site() {
    std::lock_guard lock{g_site_mutex};
    return g_site;
}

std::optional<Profile> Parse(std::string const& json, std::string& error) {
    try {
        auto const doc = nlohmann::json::parse(json);
        Profile p;
        p.version = doc.value("version", 0LL);
        p.generated_at = doc.value("generated_at", 0LL);
        p.platform = doc.value("platform", std::string{});
        if (doc.contains("server") && doc.at("server").is_object()) {
            auto const& server = doc.at("server");
            p.server_address = server.value("address", std::string{});
            p.https_port = server.value("https_port", 443);
            p.http_port = server.value("http_port", 80);
        }
        if (doc.contains("ca") && doc.at("ca").is_object()) {
            auto const& ca = doc.at("ca");
            p.ca_url = ca.value("url", std::string{});
            p.ca_sha256 = ca.value("sha256", std::string{});
            std::transform(p.ca_sha256.begin(), p.ca_sha256.end(), p.ca_sha256.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            p.ca_not_after = ca.value("not_after", 0LL);
        }
        if (doc.contains("redirect") && doc.at("redirect").is_object()) {
            auto const& redirect = doc.at("redirect");
            for (auto const& s : redirect.value("suffixes", std::vector<std::string>{})) {
                p.suffixes.push_back(s);
            }
            for (auto const& s : redirect.value("exact", std::vector<std::string>{})) {
                p.exact.push_back(s);
            }
            for (auto const& s : redirect.value("never", std::vector<std::string>{})) {
                p.never.push_back(s);
            }
            if (redirect.contains("overrides") && redirect.at("overrides").is_object()) {
                for (auto const& [name, address] : redirect.at("overrides").items()) {
                    if (address.is_string()) {
                        p.overrides.emplace_back(name, address.get<std::string>());
                    }
                }
            }
        }
        if (doc.contains("services") && doc.at("services").is_array()) {
            for (auto const& s : doc.at("services")) {
                p.services.push_back(
                    Service{s.value("id", std::string{}), s.value("url", std::string{})});
            }
        }
        p.recheck_after = doc.value("recheck_after", 21600LL);
        return p;
    } catch (std::exception const& e) {
        error = fmt::format("unparseable profile body: {}", e.what());
        return std::nullopt;
    }
}

std::optional<std::string> Validate(Profile const& profile, std::string const& expected_platform) {
    if (!expected_platform.empty() && !profile.platform.empty() &&
        profile.platform != expected_platform) {
        return fmt::format("profile is for platform {}, not {}", profile.platform,
                           expected_platform);
    }
    if (!IsLiteralAddress(profile.server_address)) {
        return "server.address is not a literal IP address";
    }
    if (IsLoopbackOrLinkLocal(profile.server_address)) {
        return "server.address is a loopback or link-local address";
    }
    // Which names may be redirected is the ceiling's call, name by name (FilterToCeiling); only
    // the addresses they would go to are judged here.
    for (auto const& [name, address] : profile.overrides) {
        if (!IsLiteralAddress(address) || IsLoopbackOrLinkLocal(address)) {
            return fmt::format("override {} does not name a usable literal address", name);
        }
    }
    // A CA arrives over HTTPS and carries its digest, or it is not a CA this profile can name.
    if (!profile.ca_url.empty()) {
        if (!SplitHttpsUrl(profile.ca_url)) {
            return "ca.url is not an https:// address";
        }
        if (profile.ca_sha256.size() != 64 ||
            profile.ca_sha256.find_first_not_of("0123456789abcdef") != std::string::npos) {
            return "ca.sha256 is not a SHA-256 digest";
        }
    }
    return std::nullopt;  // valid
}

char const* SourceName(Source s) {
    switch (s) {
    case Source::Fetched: return "fetched";
    case Source::Cached: return "cached";
    case Source::BuiltIn: return "built-in";
    }
    return "built-in";
}

Profile BuiltIn(std::string const& platform) {
    Profile p;
    p.platform = platform;
    p.server_address.clear();  // the console server from server.json stands in
    if (platform == "switch") {
        p.suffixes = {".nintendo.net", ".nintendo.com", ".nintendo.co.jp", ".nintendowifi.net",
                      ".nintendo-europe.com"};
        // The NAT check compares what two addresses observe of one console, so its second probe
        // must not collapse onto the first the way the wildcard would collapse it.
        p.overrides = {{"nncs2-lp1.n.n.srv.nintendo.net", "145.241.228.207"}};
    } else {
        p.suffixes = {".nintendowifi.net", ".gamespy.com", ".nintendo.net", ".openpak.org"};
    }
    // Inside the ceiling like anything else, and quietly.
    FilterToCeiling(p, CurrentCeiling().FamiliesFor(platform), false);
    return p;
}

std::optional<Profile> LoadStored(std::string const& platform) {
    auto const stored = ReadStored();
    if (!stored) {
        return std::nullopt;
    }
    std::string error;
    auto parsed = Parse(stored->body, error);
    if (!parsed) {
        LOG_WARNING(WebService, "[OpenPak] network profile: stored copy unreadable: {}", error);
        return std::nullopt;
    }
    auto invalid = Validate(*parsed, platform);
    if (invalid) {
        LOG_WARNING(WebService, "[OpenPak] network profile: stored copy rejected: {}", *invalid);
        return std::nullopt;
    }
    FilterToCeiling(*parsed, CurrentCeiling().FamiliesFor(platform));
    return parsed;
}

bool StoreRaw(std::string const& body, std::string const& etag) {
    nlohmann::json const profile = nlohmann::json::parse(body, nullptr, false);
    if (profile.is_discarded() || !profile.is_object()) {
        return false;
    }
    nlohmann::json doc = nlohmann::json::object();
    doc["etag"] = etag;
    doc["profile"] = profile;
    if (!Platform::WriteAtomically(Platform::NetworkProfilePath(), doc.dump(2))) {
        LOG_WARNING(WebService, "[OpenPak] network profile: could not store the profile");
        return false;
    }
    return true;
}

bool CaMatchesPin(std::string const& pem, std::string const& sha256_hex) {
    return !pem.empty() && Sha256Hex(pem) == Lower(sha256_hex);
}

bool EnsureCA(Profile const& profile) {
    if (profile.ca_url.empty()) {
        return true;  // no CA section: nothing to pin
    }
    if (CaMatchesPin(Platform::ReadFile(Platform::CaPath()), profile.ca_sha256)) {
        return true;  // the pinned bytes are already on disk
    }
    auto const url = SplitHttpsUrl(profile.ca_url);
    if (!url) {
        LOG_WARNING(WebService, "[OpenPak] network profile: the CA address is not https");
        return false;
    }
    httplib::Client client(url->first);
    client.set_connection_timeout(5, 0);
    client.set_read_timeout(5, 0);
    client.set_follow_location(false);
    Platform::ApplySystemCa(client);
    auto const result = client.Get(url->second, ClientHeaders());
    if (!result || result->status != 200) {
        LOG_WARNING(WebService, "[OpenPak] network profile: could not download the CA from {}",
                    profile.ca_url);
        return false;
    }
    std::string const digest = Sha256Hex(result->body);
    if (digest != Lower(profile.ca_sha256)) {
        LOG_WARNING(WebService,
                    "[OpenPak] network profile {}: CA digest mismatch ({} != {}); not trusted",
                    profile.version, digest, profile.ca_sha256);
        return false;
    }
    if (!Platform::WriteAtomically(Platform::CaPath(), result->body)) {
        LOG_WARNING(WebService, "[OpenPak] network profile: could not write ca.pem");
        return false;
    }
    LOG_INFO(WebService, "[OpenPak] Pinned the console-facing CA from network profile {}",
             profile.version);
    return true;
}

namespace {

struct Fetched {
    int status = 0;  // 0: no answer
    std::string body;
    std::string etag;
};

// One GET of the profile, two seconds, no retries.
Fetched GetProfile(std::string const& platform, std::string const& etag) {
    httplib::Client client(Site());
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(2, 0);
    client.set_follow_location(false);
    Platform::ApplySystemCa(client);
    httplib::Headers headers = ClientHeaders();
    if (!etag.empty()) {
        headers.emplace("If-None-Match", etag);
    }
    auto const result =
        client.Get(fmt::format("/api/v1/network/profile?platform={}", platform), headers);
    if (!result) {
        return {};
    }
    return {result->status, result->body, result->get_header_value("ETag")};
}

// A fetched body that validates and whose CA pins, stored as sent. The profile, unfiltered.
std::optional<Profile> Accept(std::string const& platform, Fetched const& fetch,
                              std::string const& etag_sent, std::string& error) {
    auto fetched = Parse(fetch.body, error);
    if (!fetched) {
        return std::nullopt;
    }
    if (auto const invalid = Validate(*fetched, platform)) {
        error = *invalid;
        return std::nullopt;
    }
    // A trust anchor is the one thing never updated on a maybe: a CA that does not match the
    // digest rejects the whole profile, and last-known-good stays.
    if (!EnsureCA(*fetched)) {
        error = "the CA did not match the digest the profile published";
        return std::nullopt;
    }
    // Stored as the server sent it: a later ceiling may admit what this one drops.
    StoreRaw(fetch.body, fetch.etag.empty() ? etag_sent : fetch.etag);
    return fetched;
}

// The shared shape of Fetch and Refresh: everything in §2 of the PRD after the request line.
Result Finish(std::string const& platform, Fetched const& fetch, std::string const& etag_sent) {
    Result out;
    out.ok = true;
    out.profile = BuiltIn(platform);

    auto stored = LoadStored(platform);
    if (stored) {
        out.profile = *stored;
        out.source = Source::Cached;
    }

    if (fetch.status == 0) {
        out.error = fmt::format("no answer from {}", Site());
        return out;
    }
    if (fetch.status == 304) {
        if (!stored) {
            out.error = "server says the profile is unchanged, but nothing is stored";
        }
        // The stored copy is the current one. Nothing reads the files' age (the re-check is a
        // timer from boot), so the stored file is left as it is.
        out.confirmed = stored.has_value();
        return out;  // stored, or built-in if nothing was stored
    }
    if (fetch.status != 200) {
        out.error = fmt::format("HTTP {}", fetch.status);
        return out;
    }

    std::string error;
    auto fetched = Accept(platform, fetch, etag_sent, error);
    if (!fetched) {
        out.error = error;
        return out;
    }
    FilterToCeiling(*fetched, CurrentCeiling().FamiliesFor(platform));
    out.profile = *fetched;
    out.source = Source::Fetched;
    out.confirmed = true;
    return out;
}

// The ceiling first (contract rule 1), fetched beside the profile so the two share one two-second
// wait; the stored profile is only read once the ceiling that filters it is settled.
Result FetchWithCeiling(std::string const& platform, bool conditional, char const* verb) {
    auto ceiling = std::async(std::launch::async, [] { return FetchCeilingEnvelope(); });

    std::string etag;
    if (conditional) {
        // A 304 can only stand on a stored copy that parses and validates.
        if (auto const stored = ReadStored()) {
            std::string error;
            auto const parsed = Parse(stored->body, error);
            if (parsed && !Validate(*parsed, platform)) {
                etag = stored->etag;
            }
        }
    }
    Fetched const fetch = GetProfile(platform, etag);

    LoadCeiling(ceiling.get());
    Result out = Finish(platform, fetch, etag);
    Apply(out);
    LOG_INFO(WebService, "[OpenPak] network profile: {} platform {} -> {} source, version {}{}{}",
             verb, platform, SourceName(out.source), out.profile.version,
             fetch.status == 304 && out.confirmed ? " (unchanged, 304)" : "",
             out.error.empty() ? std::string{} : fmt::format(" ({})", out.error));
    return out;
}

}  // namespace

Result Fetch(std::string const& platform) {
    return FetchWithCeiling(platform, true, "fetched");
}

Result Refresh(std::string const& platform) {
    return FetchWithCeiling(platform, false, "refreshed");
}

Result DecideFetched(std::string const& platform, int status, std::string const& body,
                     std::string const& etag, std::string const& etag_sent) {
    return Finish(platform, Fetched{status, body, etag}, etag_sent);
}

std::optional<std::string> FetchFreshDigest(std::string const& platform) {
    RefreshCeiling();
    Fetched const fetch = GetProfile(platform, {});
    if (fetch.status != 200) {
        return std::nullopt;
    }
    std::string error;
    auto profile = Accept(platform, fetch, {}, error);
    if (!profile) {
        LOG_WARNING(WebService, "[OpenPak] network re-check: profile not taken: {}", error);
        return std::nullopt;
    }
    FilterToCeiling(*profile, CurrentCeiling().FamiliesFor(platform));
    return EffectiveDigest(*profile);
}

void Apply(Result const& applied) {
    {
        std::lock_guard lock{g_applied_mutex};
        g_applied = applied;
    }
    SetDigestInUse(EffectiveDigest(applied.profile));
}

bool Loaded() {
    std::lock_guard lock{g_applied_mutex};
    return g_applied && g_applied->source != Source::BuiltIn;
}

std::optional<std::string> RedirectFor(std::string const& host,
                                       std::string const& server_address) {
    std::lock_guard lock{g_applied_mutex};
    if (!g_applied) {
        return std::nullopt;
    }
    Profile const& profile = g_applied->profile;

    // Names that must reach the real internet win over every wildcard below them: the
    // console's own connection test is the example, and pointing it at OpenPak measures OpenPak.
    for (auto const& never : profile.never) {
        if (Matches(host, never)) {
            return std::nullopt;
        }
    }
    // An address of its own, before the wildcards collapse it onto the server.
    for (auto const& [name, address] : profile.overrides) {
        if (Matches(host, name)) {
            return address;
        }
    }
    std::string const& server =
        profile.server_address.empty() ? server_address : profile.server_address;
    for (auto const& exact : profile.exact) {
        if (Matches(host, exact)) {
            return server;
        }
    }
    for (auto const& suffix : profile.suffixes) {
        if (Matches(host, suffix)) {
            return server;
        }
    }
    return std::nullopt;
}

bool IsNever(std::string const& host) {
    std::lock_guard lock{g_applied_mutex};
    if (!g_applied) {
        return false;
    }
    return std::any_of(g_applied->profile.never.begin(), g_applied->profile.never.end(),
                       [&](std::string const& never) { return Matches(host, never); });
}

GuestName ClassifyGuestName(std::string const& host, std::string const& platform,
                            std::string const& server_address) {
    if (IsNever(host)) {
        return {GuestNameKind::Direct, {}};
    }
    if (auto const address = RedirectFor(host, server_address)) {
        if (address->empty()) {
            return {GuestNameKind::Blocked, {}};
        }
        return {GuestNameKind::Redirect, *address};
    }
    if (NameAllowed(host, platform)) {
        return {GuestNameKind::Blocked, {}};
    }
    return {GuestNameKind::Direct, {}};
}

}  // namespace OpenPak::NetworkProfile

// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later
// Derived from openpak-client 3fd9ba7 (network_ceiling.cpp).
//
// The signed redirect ceiling and the change notice, implemented against docs/signed-ceiling.md
// (the umbrella's contract; every client follows it). Ed25519 verification is OpenSSL's EVP, which
// every host's TLS library provides: OpenSSL 1.1.1 and later, and LibreSSL 3.7 and later
// (Azahar's bundled 3.8).

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <random>
#include <set>
#include <thread>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/sha.h>

#include "common/httplib.h"
#include "common/logging.h"
#include "core/openpak/network_profile.h"
#include "core/openpak/platform.h"

namespace OpenPak::NetworkProfile {

namespace {

// The pinned origin. Never OPENPAK_API: a local stack may serve profiles, but only openpak.org
// serves the ceiling that bounds them.
constexpr char const* kCeilingOrigin = "https://openpak.org";
constexpr char const* kCeilingPath = "/api/v1/network/ceiling";

std::filesystem::path CeilingPath() {
    return Platform::CeilingPath();
}

std::filesystem::path HighestPath() {
    return Platform::CeilingVersionPath();
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string Hex(unsigned char const* data, std::size_t size) {
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out += fmt::format("{:02x}", data[i]);
    }
    return out;
}

std::string Sha256Hex(std::string const& data) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<unsigned char const*>(data.data()), data.size(), digest);
    return Hex(digest, sizeof(digest));
}

// Standard base64 with padding, strictly: anything else is not what the signer wrote.
std::optional<std::string> DecodeBase64(std::string const& in) {
    if (in.empty() || in.size() % 4 != 0) {
        return std::nullopt;
    }
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string out;
    out.reserve(in.size() / 4 * 3);
    for (std::size_t i = 0; i < in.size(); i += 4) {
        bool const last = i + 4 == in.size();
        int v[4];
        int pad = 0;
        for (int j = 0; j < 4; ++j) {
            char const c = in[i + j];
            if (c == '=') {
                // Padding only at the very end, and only in the last two places.
                if (!last || j < 2) {
                    return std::nullopt;
                }
                ++pad;
                v[j] = 0;
                continue;
            }
            if (pad != 0) {
                return std::nullopt;
            }
            v[j] = value(c);
            if (v[j] < 0) {
                return std::nullopt;
            }
        }
        unsigned const triple = (unsigned(v[0]) << 18) | (unsigned(v[1]) << 12) |
                                (unsigned(v[2]) << 6) | unsigned(v[3]);
        out += static_cast<char>((triple >> 16) & 0xFF);
        if (pad < 2) out += static_cast<char>((triple >> 8) & 0xFF);
        if (pad < 1) out += static_cast<char>(triple & 0xFF);
    }
    return out;
}

bool VerifyEd25519(PinnedKey const& key, std::string const& message, std::string const& sig) {
    if (sig.size() != 64) {
        return false;
    }
    EVP_PKEY* pkey =
        EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, key.key.data(), key.key.size());
    if (pkey == nullptr) {
        return false;
    }
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    bool ok = false;
    if (ctx != nullptr && EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, pkey) == 1) {
        ok = EVP_DigestVerify(ctx, reinterpret_cast<unsigned char const*>(sig.data()), sig.size(),
                              reinterpret_cast<unsigned char const*>(message.data()),
                              message.size()) == 1;
    }
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return ok;
}

// One line per distinct reason: a failure that repeats every six hours says so once.
void LogOnce(std::string const& message) {
    static std::mutex mutex;
    static std::set<std::string> said;
    std::lock_guard lock{mutex};
    if (said.insert(message).second) {
        LOG_WARNING(WebService, "network ceiling: {}", message);
    }
}

bool WriteAtomically(std::filesystem::path const& path, std::string const& contents) {
    return Platform::WriteAtomically(path, contents);
}

std::optional<std::string> ReadWhole(std::filesystem::path const& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return std::nullopt;
    }
    std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (body.empty()) {
        return std::nullopt;
    }
    return body;
}

long long ReadHighest() {
    auto const text = ReadWhole(HighestPath());
    if (!text) {
        return 0;
    }
    try {
        return (std::max)(0LL, std::stoll(*text));
    } catch (std::exception const&) {
        return 0;
    }
}

std::mutex g_ceiling_mutex;
std::optional<Ceiling> g_ceiling;

}  // namespace

PinnedKey MakePinnedKey(std::array<unsigned char, 32> const& raw) {
    PinnedKey key;
    key.key = raw;
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(raw.data(), raw.size(), digest);
    key.keyid = Hex(digest, sizeof(digest));
    return key;
}

std::vector<PinnedKey> const& PinnedKeys() {
    // keyid 36e8bcdd93c2c1a1d7e5d87bbba05a7a4f97882131cf6878f1c053a25a377fe4
    static const std::vector<PinnedKey> keys = {
        MakePinnedKey({0x0f, 0xd2, 0xa2, 0x66, 0x08, 0x68, 0xe5, 0x3d, 0x20, 0xfc, 0x81,
                       0x1e, 0x3f, 0x15, 0x71, 0x0c, 0xb1, 0xad, 0x15, 0x7b, 0xa7, 0xda,
                       0x80, 0x19, 0xf7, 0xa5, 0x9e, 0x8a, 0xd6, 0x04, 0xd8, 0x7c}),
    };
    return keys;
}

std::vector<std::string> const& Ceiling::FamiliesFor(std::string const& platform) const {
    static const std::vector<std::string> none;
    auto const it = platforms.find(platform);
    return it == platforms.end() ? none : it->second;
}

std::vector<IpRedirect> const& Ceiling::IpRedirectsFor(std::string const& platform) const {
    static const std::vector<IpRedirect> none;
    auto const it = ip_redirects.find(platform);
    return it == ip_redirects.end() ? none : it->second;
}

bool ParseIpv4Endpoint(std::string const& text, std::array<unsigned char, 4>& ip,
                       unsigned short& port) {
    std::size_t const colon = text.find(':');
    if (colon == std::string::npos || text.find(':', colon + 1) != std::string::npos) {
        return false;
    }
    auto number = [](std::string const& part, unsigned long max, unsigned long& out) {
        if (part.empty() || part.size() > 5 || (part.size() > 1 && part.front() == '0')) {
            return false;
        }
        out = 0;
        for (char c : part) {
            if (c < '0' || c > '9') {
                return false;
            }
            out = out * 10 + static_cast<unsigned long>(c - '0');
        }
        return out <= max;
    };
    std::string const address = text.substr(0, colon);
    std::size_t start = 0;
    for (int i = 0; i < 4; ++i) {
        std::size_t const dot = address.find('.', start);
        if ((i < 3) != (dot != std::string::npos)) {
            return false;
        }
        unsigned long octet = 0;
        if (!number(address.substr(start, i < 3 ? dot - start : std::string::npos), 255, octet)) {
            return false;
        }
        ip[static_cast<std::size_t>(i)] = static_cast<unsigned char>(octet);
        start = dot + 1;
    }
    unsigned long p = 0;
    if (!number(text.substr(colon + 1), 65535, p) || p == 0) {
        return false;
    }
    port = static_cast<unsigned short>(p);
    return true;
}

char const* CeilingSourceName(CeilingSource s) {
    switch (s) {
    case CeilingSource::Fetched: return "fetched";
    case CeilingSource::Cached: return "cached";
    case CeilingSource::BuiltIn: return "built-in";
    }
    return "built-in";
}

Ceiling CompiledCeiling() {
    // Frozen: what every build carried before the ceiling was signed. A new family is a signed
    // ceiling on the server, never an edit here.
    static const std::vector<std::string> families = {
        ".nintendo.net",        ".nintendo.com", ".nintendo.co.jp", ".nintendowifi.net",
        ".nintendo-europe.com", ".gamespy.com",  ".openpak.org",    ".among.us",
        ".photonengine.io",
    };
    Ceiling c;
    for (char const* platform : {"switch", "wiiu", "3ds", "wii", "ds"}) {
        c.platforms[platform] = families;
    }
    return c;
}

bool FamilyWellFormed(std::string const& family) {
    if (family.size() < 2 || family.front() != '.') {
        return false;
    }
    std::size_t labels = 0;
    std::size_t start = 1;
    while (true) {
        std::size_t const dot = family.find('.', start);
        std::string const label =
            family.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-') {
            return false;
        }
        for (char c : label) {
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
                return false;
            }
        }
        ++labels;
        if (dot == std::string::npos) {
            break;
        }
        start = dot + 1;
    }
    return labels >= 2;
}

std::optional<Ceiling> VerifyCeiling(std::string const& envelope,
                                     std::vector<PinnedKey> const& keys, std::string& error) {
    std::string payload;
    try {
        auto const env = nlohmann::json::parse(envelope);
        if (!env.is_object() || !env.contains("payload") || !env.at("payload").is_string()) {
            error = "envelope has no payload";
            return std::nullopt;
        }
        auto decoded = DecodeBase64(env.at("payload").get<std::string>());
        if (!decoded) {
            error = "payload is not base64";
            return std::nullopt;
        }
        payload = std::move(*decoded);
        if (!env.contains("signatures") || !env.at("signatures").is_array() ||
            env.at("signatures").empty()) {
            error = "envelope has no signatures";
            return std::nullopt;
        }
        bool verified = false;
        bool known_key = false;
        for (auto const& s : env.at("signatures")) {
            if (!s.is_object() || !s.contains("keyid") || !s.at("keyid").is_string() ||
                !s.contains("sig") || !s.at("sig").is_string()) {
                continue;
            }
            std::string const keyid = Lower(s.at("keyid").get<std::string>());
            auto const sig = DecodeBase64(s.at("sig").get<std::string>());
            for (auto const& key : keys) {
                if (key.keyid != keyid) {
                    continue;
                }
                known_key = true;
                if (sig && VerifyEd25519(key, payload, *sig)) {
                    verified = true;
                }
            }
        }
        if (!verified) {
            error = known_key ? "bad signature" : "no signature under a pinned keyid";
            return std::nullopt;
        }
    } catch (std::exception const& e) {
        error = fmt::format("envelope does not parse: {}", e.what());
        return std::nullopt;
    }

    // Only now, and only these bytes, are parsed.
    try {
        auto const doc = nlohmann::json::parse(payload);
        if (!doc.is_object() || !doc.contains("type") || doc.at("type") != "openpak-ceiling") {
            error = "payload type is not openpak-ceiling";
            return std::nullopt;
        }
        if (!doc.contains("version") || !doc.at("version").is_number_integer() ||
            doc.at("version").get<long long>() <= 0) {
            error = "payload version is not a positive integer";
            return std::nullopt;
        }
        if (!doc.contains("platforms") || !doc.at("platforms").is_object()) {
            error = "payload has no platforms";
            return std::nullopt;
        }
        Ceiling c;
        c.version = doc.at("version").get<long long>();
        if (doc.contains("issued") && doc.at("issued").is_string()) {
            c.issued = doc.at("issued").get<std::string>();
        }
        for (auto const& [platform, list] : doc.at("platforms").items()) {
            if (!list.is_array()) {
                error = fmt::format("platform {} is not a list", platform);
                return std::nullopt;
            }
            std::vector<std::string> families;
            for (auto const& entry : list) {
                if (!entry.is_string() || !FamilyWellFormed(entry.get<std::string>())) {
                    error = fmt::format("platform {} has a malformed family {}", platform,
                                        entry.dump());
                    return std::nullopt;
                }
                families.push_back(entry.get<std::string>());
            }
            c.platforms[platform] = std::move(families);
        }
        // Optional: titles that dial a hard-coded address (bsd_ip_redirect.h). Strict like the
        // families: one malformed or duplicate entry rejects the file.
        if (doc.contains("ip_redirects")) {
            if (!doc.at("ip_redirects").is_object()) {
                error = "ip_redirects is not an object";
                return std::nullopt;
            }
            for (auto const& [platform, list] : doc.at("ip_redirects").items()) {
                if (!list.is_array()) {
                    error = fmt::format("ip_redirects {} is not a list", platform);
                    return std::nullopt;
                }
                std::vector<IpRedirect> rules;
                for (auto const& entry : list) {
                    IpRedirect r;
                    bool ok = entry.is_object() && entry.contains("from") &&
                              entry.at("from").is_string() && entry.contains("to") &&
                              entry.at("to").is_string() && entry.contains("proto") &&
                              entry.at("proto").is_string() &&
                              (entry.at("proto") == "tcp" || entry.at("proto") == "udp") &&
                              ParseIpv4Endpoint(entry.at("from").get<std::string>(), r.from_ip,
                                                r.from_port) &&
                              ParseIpv4Endpoint(entry.at("to").get<std::string>(), r.to_ip,
                                                r.to_port);
                    // "title" (16 hex digits) scopes the rule to one program on a console, whose
                    // bsd mitm only takes over that program's sessions. Harbor's bsd sees every
                    // process anyway, so it checks the field's shape and applies the rule to all.
                    if (ok && entry.contains("title")) {
                        auto const& node = entry.at("title");
                        std::string const title = node.is_string() ? node.get<std::string>() : "";
                        ok = title.size() == 16 &&
                             std::all_of(title.begin(), title.end(), [](char ch) {
                                 return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
                                        (ch >= 'A' && ch <= 'F');
                             });
                    }
                    if (ok) {
                        r.tcp = entry.at("proto") == "tcp";
                        ok = std::none_of(rules.begin(), rules.end(), [&](IpRedirect const& o) {
                            return o.tcp == r.tcp && o.from_ip == r.from_ip &&
                                   o.from_port == r.from_port;
                        });
                    }
                    if (!ok) {
                        error = fmt::format("ip_redirects {} has a malformed or duplicate entry {}",
                                            platform, entry.dump());
                        return std::nullopt;
                    }
                    rules.push_back(r);
                }
                c.ip_redirects[platform] = std::move(rules);
            }
        }
        return c;
    } catch (std::exception const& e) {
        error = fmt::format("payload does not parse: {}", e.what());
        return std::nullopt;
    }
}

CeilingDecision DecideCeiling(std::optional<std::string> const& cached,
                              std::optional<std::string> const& fetched,
                              long long highest_accepted, std::vector<PinnedKey> const& keys) {
    CeilingDecision out;
    out.highest_accepted = highest_accepted;
    out.result.ceiling = CompiledCeiling();
    out.result.source = CeilingSource::BuiltIn;

    std::string cache_error;
    if (cached) {
        std::string error;
        auto const verified = VerifyCeiling(*cached, keys, error);
        if (!verified) {
            cache_error = fmt::format("cached ceiling rejected: {}", error);
        } else if (verified->version < highest_accepted) {
            cache_error = fmt::format("cached ceiling v{} is older than v{} accepted before",
                                      verified->version, highest_accepted);
        } else {
            out.result.ceiling = *verified;
            out.result.source = CeilingSource::Cached;
            out.highest_accepted = (std::max)(highest_accepted, verified->version);
        }
    }

    if (!fetched) {
        out.result.error = cache_error.empty() ? "no answer from openpak.org" : cache_error;
        return out;
    }
    std::string error;
    auto const verified = VerifyCeiling(*fetched, keys, error);
    if (!verified) {
        out.result.error = fmt::format("fetched ceiling rejected: {}", error);
        return out;
    }
    if (verified->version < out.highest_accepted) {
        out.result.error = fmt::format("fetched ceiling v{} is older than v{} accepted before",
                                       verified->version, out.highest_accepted);
        return out;
    }
    out.result.ceiling = *verified;
    out.result.source = CeilingSource::Fetched;
    out.result.error = cache_error;
    out.highest_accepted = verified->version;
    out.store = true;
    return out;
}

std::optional<std::string> FetchCeilingEnvelope() {
    httplib::Client client(kCeilingOrigin);
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(2, 0);
    client.set_follow_location(false);
    // Public roots only: ApplySystemCa is the system store, never the OpenPak CA.
    Platform::ApplySystemCa(client);
    httplib::Headers headers;
    if (std::string header = Platform::ClientHeader(); !header.empty()) {
        headers.emplace("X-OpenPak-Client", std::move(header));
    }
    auto const result = client.Get(kCeilingPath, headers);
    if (!result) {
        LogOnce(fmt::format("could not reach {}{}", kCeilingOrigin, kCeilingPath));
        return std::nullopt;
    }
    if (result->status != 200) {
        LogOnce(fmt::format("{}{} answered HTTP {}", kCeilingOrigin, kCeilingPath,
                            result->status));
        return std::nullopt;
    }
    return result->body;
}

CeilingResult LoadCeiling(std::optional<std::string> const& fetched,
                          std::vector<PinnedKey> const& keys) {
    auto const decision = DecideCeiling(ReadWhole(CeilingPath()), fetched, ReadHighest(), keys);
    if (decision.store) {
        if (!WriteAtomically(CeilingPath(), *fetched)) {
            LogOnce(fmt::format("could not cache the ceiling at {}", CeilingPath().string()));
        }
    }
    if (decision.highest_accepted > ReadHighest()) {
        WriteAtomically(HighestPath(), std::to_string(decision.highest_accepted));
    }
    // The fetch reports its own failure; everything else is said here, once.
    if (!decision.result.error.empty() && (fetched || decision.result.error.rfind("cached", 0) == 0)) {
        LogOnce(decision.result.error);
    }
    {
        std::lock_guard lock{g_ceiling_mutex};
        g_ceiling = decision.result.ceiling;
    }
    LOG_INFO(WebService, "network ceiling: {} v{}", CeilingSourceName(decision.result.source),
                     decision.result.ceiling.version);
    return decision.result;
}

CeilingResult RefreshCeiling() {
    return LoadCeiling(FetchCeilingEnvelope());
}

Ceiling CurrentCeiling() {
    {
        std::lock_guard lock{g_ceiling_mutex};
        if (g_ceiling) {
            return *g_ceiling;
        }
    }
    auto const decision =
        DecideCeiling(ReadWhole(CeilingPath()), std::nullopt, ReadHighest(), PinnedKeys());
    std::lock_guard lock{g_ceiling_mutex};
    if (!g_ceiling) {
        g_ceiling = decision.result.ceiling;
    }
    return *g_ceiling;
}

std::optional<std::pair<std::array<unsigned char, 4>, unsigned short>> IpRedirectFor(
    std::array<unsigned char, 4> const& ip, unsigned short port, bool tcp,
    std::string const& platform) {
    // Called for every datagram a guest sends to an address: look in place, no copy.
    auto find = [&](Ceiling const& ceiling)
        -> std::optional<std::pair<std::array<unsigned char, 4>, unsigned short>> {
        for (auto const& r : ceiling.IpRedirectsFor(platform)) {
            if (r.tcp == tcp && r.from_ip == ip && r.from_port == port) {
                return std::make_pair(r.to_ip, r.to_port);
            }
        }
        return std::nullopt;
    };
    {
        std::lock_guard lock{g_ceiling_mutex};
        if (g_ceiling) {
            return find(*g_ceiling);
        }
    }
    return find(CurrentCeiling());
}

bool NameInFamilies(std::string const& host, std::vector<std::string> const& families) {
    std::string const name = Lower(host);
    for (auto const& family : families) {
        if (family.size() < 2) {
            continue;
        }
        if (name == family || name == family.substr(1) ||
            (name.size() > family.size() &&
             name.compare(name.size() - family.size(), family.size(), family) == 0)) {
            return true;
        }
    }
    return false;
}

bool NameAllowed(std::string const& host, std::string const& platform) {
    return NameInFamilies(host, CurrentCeiling().FamiliesFor(platform));
}

std::vector<std::string> FilterToCeiling(Profile& profile,
                                         std::vector<std::string> const& families, bool log) {
    std::vector<std::string> dropped;
    auto keep = [&](std::string const& name, char const* what) {
        if (NameInFamilies(name, families)) {
            return true;
        }
        if (log) {
            LOG_WARNING(WebService, "network profile: dropped {} {}: outside this platform's ceiling",
                                what, name);
        }
        dropped.push_back(name);
        return false;
    };
    std::erase_if(profile.suffixes, [&](auto const& n) { return !keep(n, "suffix"); });
    std::erase_if(profile.exact, [&](auto const& n) { return !keep(n, "exact name"); });
    std::erase_if(profile.overrides, [&](auto const& o) { return !keep(o.first, "override"); });
    return dropped;
}

std::string EffectiveDigest(Profile const& profile) {
    std::vector<std::string> entries;
    for (auto const& s : profile.suffixes) {
        entries.push_back("family:" + Lower(s));
    }
    for (auto const& e : profile.exact) {
        entries.push_back("exact:" + Lower(e));
    }
    for (auto const& [name, address] : profile.overrides) {
        entries.push_back("override:" + Lower(name) + "=" + address);
    }
    if (!profile.server_address.empty()) {
        entries.push_back("address:" + profile.server_address);
    }
    // The signed ip redirects are part of what this platform redirects (none: no line, so a
    // platform without any keeps its digest).
    auto endpoint = [](std::array<unsigned char, 4> const& ip, unsigned short port) {
        return fmt::format("{}.{}.{}.{}:{}", ip[0], ip[1], ip[2], ip[3], port);
    };
    Ceiling const ceiling = CurrentCeiling();
    for (auto const& r : ceiling.IpRedirectsFor(profile.platform)) {
        entries.push_back(fmt::format("ip:{}:{}={}", r.tcp ? "tcp" : "udp",
                                      endpoint(r.from_ip, r.from_port),
                                      endpoint(r.to_ip, r.to_port)));
    }
    std::sort(entries.begin(), entries.end());
    entries.erase(std::unique(entries.begin(), entries.end()), entries.end());
    std::string joined;
    for (auto const& entry : entries) {
        joined += entry;
        joined += '\n';
    }
    return Sha256Hex(joined);
}

// ---------------------------------------------------------------------------------------------
// The change notice.

namespace {

// Leaked on purpose: the re-check thread is detached and may still be waiting at exit.
struct Watch {
    std::mutex mutex;
    std::condition_variable wake;
    bool recheck = false;
    bool started = false;
    std::string platform;
    std::function<void()> notifier;
    std::string in_use;
    std::string announced;
};

Watch& W() {
    static Watch* watch = new Watch;
    return *watch;
}

void WatchLoop() {
    std::mt19937_64 rng{std::random_device{}()};
    // Six hours, +-10 %.
    std::uniform_int_distribution<long long> period(19440, 23760);
    while (true) {
        std::string platform;
        {
            std::unique_lock lock{W().mutex};
            W().wake.wait_for(lock, std::chrono::seconds(period(rng)),
                              [] { return W().recheck; });
            W().recheck = false;
            platform = W().platform;
        }
        auto const digest = FetchFreshDigest(platform);
        if (!digest || !ShouldAnnounce(*digest)) {
            continue;
        }
        LOG_INFO(WebService, "network profile: the redirects for {} changed; a restart applies them",
                         platform);
        std::function<void()> notifier;
        {
            std::lock_guard lock{W().mutex};
            notifier = W().notifier;
        }
        if (notifier) {
            notifier();
        }
    }
}

}  // namespace

void SetChangeNotifier(std::function<void()> notifier) {
    std::lock_guard lock{W().mutex};
    W().notifier = std::move(notifier);
}

void StartWatching(std::string const& platform) {
    std::lock_guard lock{W().mutex};
    W().platform = platform;
    if (W().started) {
        return;
    }
    W().started = true;
    std::thread(WatchLoop).detach();
}

void RecheckSoon() {
    std::lock_guard lock{W().mutex};
    if (!W().started) {
        return;
    }
    W().recheck = true;
    W().wake.notify_all();
}

std::string DigestInUse() {
    std::lock_guard lock{W().mutex};
    return W().in_use;
}

bool ShouldAnnounce(std::string const& digest) {
    std::lock_guard lock{W().mutex};
    if (W().in_use.empty() || digest == W().in_use || digest == W().announced) {
        return false;
    }
    W().announced = digest;
    return true;
}

// Apply() calls this too: what the process now redirects with.
void SetDigestInUse(std::string const& digest) {
    std::lock_guard lock{W().mutex};
    W().in_use = digest;
}

}  // namespace OpenPak::NetworkProfile

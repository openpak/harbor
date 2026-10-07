// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Where a guest name lookup goes: the decision behind sfdnsres 2/6/10/12 and the firmware 23
// resolver (100), and the name rewriting nsd 20/21 does first.
//
//   1. nsd: '%' becomes the environment ("lp1") and the account names map to their BaaS hosts
//      (only when the guest asked for nsd resolution, or through nsd itself).
//   2. The network profile: a redirected name answers with OpenPak's address; a `never` name goes
//      to the host resolver as it is.
//   3. Fail closed: a name in a ceiling family that the profile did not redirect, and any
//      Nintendo name at all, never reaches the host resolver. An unredirected Nintendo name
//      resolving for real is a console talking to Nintendo (2181-4008).
//   4. Everything else: the host resolver.
//
// Header-only and free of Eden types, so sfdnsres_redirect_selfcheck.cpp can build it on its own.
// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base), with the fail-closed rule
// widened to every ceiling family.

#pragma once

#include <algorithm>
#include <array>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace Service::Sockets::Dns {

// The network profile's answer for one name.
enum class Route {
    NoMatch,     ///< Not the profile's to answer.
    Redirect,    ///< Resolve to `address`, a literal IPv4 address.
    PassThrough, ///< A `never` name: it must reach the real internet.
};

struct RouteAnswer {
    Route route = Route::NoMatch;
    std::string address;
};

// What the host boot (src/core/openpak) installs once the profile and the ceiling are loaded.
// Called on IPC threads; each must answer from memory, never from the network.
struct Hooks {
    // True while OpenPak is on for this session.
    std::function<bool()> enabled;
    // The profile's answer for a (normalised) name: RedirectFor plus the `never` list.
    std::function<RouteAnswer(const std::string& host)> route;
    // True when the (normalised) name is inside one of this platform's ceiling families.
    std::function<bool(const std::string& host)> in_ceiling;
};

inline std::mutex& HookMutex() {
    static std::mutex mutex;
    return mutex;
}

inline Hooks& HookStorage() {
    static Hooks hooks;
    return hooks;
}

inline void SetHooks(Hooks hooks) {
    std::scoped_lock lock{HookMutex()};
    HookStorage() = std::move(hooks);
}

inline Hooks GetHooks() {
    std::scoped_lock lock{HookMutex()};
    return HookStorage();
}

// Lower case, without the trailing dot a fully qualified name carries (the firmware 23 clients
// send "api.hac.lp1.ctest.srv.nintendo.net.").
inline std::string Normalise(std::string_view host) {
    std::string out{host};
    while (!out.empty() && out.back() == '.') {
        out.pop_back();
    }
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    });
    return out;
}

// `host` is `family` (".nintendo.net") or below it, on a label boundary.
inline bool InFamily(std::string_view host, std::string_view family) {
    if (family.empty() || family.front() != '.') {
        return false;
    }
    const std::string_view apex = family.substr(1);
    if (host == apex) {
        return true;
    }
    return host.size() > family.size() && host.substr(host.size() - family.size()) == family;
}

// Nintendo's own families: never resolved for real, with OpenPak on or off.
inline constexpr std::array NintendoFamilies = {
    std::string_view{".nintendo.net"},       std::string_view{".nintendo.com"},
    std::string_view{".nintendo.co.jp"},     std::string_view{".nintendo-europe.com"},
    std::string_view{".nintendowifi.net"},   std::string_view{".nintendoswitch.com"},
    std::string_view{".nintendoswitch.cn"},  std::string_view{".nintendo.jp"},
};

// Upstream Eden's list, matched as it matches it (anywhere in the name): third-party services
// that misbehave when reached for real, and Nintendo's regional sites.
inline constexpr std::array UpstreamBlockedSubstrings = {
    std::string_view{"srv.nintendo.net"},
    std::string_view{"nintendo.es"},
    std::string_view{"nintendowifi.net"},
    std::string_view{"nintendo-europe.com"},
    std::string_view{"nintendo.com.hk"},
    std::string_view{"nintendo.com.au"},
    std::string_view{"nintendo.co.kr"},
    std::string_view{"nintendo.co.uk"},
    std::string_view{"nintendo.co.jp"},
    std::string_view{"nintendo.co.nz"},
    std::string_view{"nintendo.co.za"},
    std::string_view{"nintendo.com"},
    std::string_view{"nintendo.jp"},
    std::string_view{"nintendo.tw"},
    std::string_view{"nintendo.at"},
    std::string_view{"nintendo.be"},
    std::string_view{"nintendo.dk"},
    std::string_view{"nintendo.de"},
    std::string_view{"nintendo.fi"},
    std::string_view{"nintendo.fr"},
    std::string_view{"nintendo.gr"},
    std::string_view{"nintendo.hu"},
    std::string_view{"nintendo.it"},
    std::string_view{"nintendo.nl"},
    std::string_view{"nintendo.no"},
    std::string_view{"nintendo.pt"},
    std::string_view{"nintendo.ru"},
    std::string_view{"nintendo.ch"},
    std::string_view{"nintendo.se"},
    std::string_view{"nintendoswitch.com.cn"},
    std::string_view{"nintendoswitch.com"},
    std::string_view{"sun.hac.lp1.d4c.nintendo.net"},
    std::string_view{"phoenix-api.wbagora.com"}, // Hogwarts Legacy
    std::string_view{"battle.net"},
    std::string_view{"microsoft.com"}, // Minecraft Dungeons and other games
    std::string_view{"mojang.com"},
    std::string_view{"xboxlive.com"},
    std::string_view{"api.epicgames.dev"}, // Marvel Cosmic Invasion and others
    std::string_view{"minecraftservices.com"},
    std::string_view{"508223012e5a5ff19f30a391b2bdadc0.my.2k.com"}, // Civilization 5
};

inline bool IsBuiltInBlocked(const std::string& host) {
    for (const auto family : NintendoFamilies) {
        if (InFamily(host, family)) {
            return true;
        }
    }
    for (const auto part : UpstreamBlockedSubstrings) {
        if (host.find(part) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// nsd (FqdnResolver): the substitutions a console makes before anything resolves a name.

inline constexpr std::string_view Environment = "lp1";

inline std::string NsdResolve(std::string_view fqdn) {
    // Names nsd hands back untouched (connection tests and its own dummy).
    if (fqdn == "api.sect.srv.nintendo.net" || fqdn == "ctest.cdn.nintendo.net" ||
        fqdn == "ctest.cdn.n.nintendoswitch.cn" || fqdn == "unknown.dummy.nintendo.net") {
        return std::string{fqdn};
    }
    std::string name{fqdn};
    for (std::size_t at = name.find('%'); at != std::string::npos; at = name.find('%', at)) {
        name.replace(at, 1, Environment);
        at += Environment.size();
    }
    // The account names nsd maps to their BaaS hosts. accounts.nintendo.com is not one of them:
    // it is the Nintendo Account site itself, the Login applet's sign-in page, and mapping it to
    // BaaS sent that page to a host that does not serve it.
    if (name == "e97b8a9d672e4ce4845ec6947cd66ef6-sb-api.accounts.nintendo.com" ||
        name == "e97b8a9d672e4ce4845ec6947cd66ef6-sb.accounts.nintendo.com") {
        return "e97b8a9d672e4ce4845ec6947cd66ef6-sb.baas.nintendo.com";
    }
    if (name == "api.accounts.nintendo.com") {
        return "e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com";
    }
    return name;
}

// ---------------------------------------------------------------------------------------------

enum class Verdict {
    Redirect,   ///< Answer with `address`.
    FailClosed, ///< Answer "host not found"; the host resolver is never asked.
    HostDns,    ///< Ask the host resolver.
};

struct Decision {
    Verdict verdict = Verdict::FailClosed;
    std::string host;    ///< The normalised name the decision was made for.
    std::string address; ///< Set for Redirect.
    const char* reason = "";
};

inline bool IsIpv4Literal(std::string_view host) {
    int dots = 0;
    int digits = 0;
    for (const char c : host) {
        if (c == '.') {
            if (digits == 0) {
                return false;
            }
            ++dots;
            digits = 0;
        } else if (c >= '0' && c <= '9') {
            if (++digits > 3) {
                return false;
            }
        } else {
            return false;
        }
    }
    return dots == 3 && digits > 0;
}

// `host` after nsd resolution (when the guest asked for it).
inline Decision Decide(std::string_view host_in, const Hooks& hooks) {
    Decision decision;
    decision.host = Normalise(host_in);
    const std::string& host = decision.host;

    if (host.empty()) {
        decision.reason = "empty name";
        return decision;
    }
    if (host == "localhost" || IsIpv4Literal(host) || host.find(':') != std::string::npos) {
        decision.verdict = Verdict::HostDns;
        decision.reason = "literal or local";
        return decision;
    }

    const bool enabled = hooks.enabled && hooks.enabled();
    if (enabled && hooks.route) {
        RouteAnswer answer = hooks.route(host);
        if (answer.route == Route::Redirect && IsIpv4Literal(answer.address)) {
            decision.verdict = Verdict::Redirect;
            decision.address = std::move(answer.address);
            decision.reason = "network profile";
            return decision;
        }
        if (answer.route == Route::PassThrough) {
            decision.verdict = Verdict::HostDns;
            decision.reason = "network profile: never redirected";
            return decision;
        }
    }
    if (enabled && hooks.in_ceiling && hooks.in_ceiling(host)) {
        decision.reason = "ceiling family, not redirected";
        return decision;
    }
    if (IsBuiltInBlocked(host)) {
        decision.reason = "Nintendo or blocked name, not redirected";
        return decision;
    }
    decision.verdict = Verdict::HostDns;
    decision.reason = "not OpenPak's";
    return decision;
}

inline Decision Decide(std::string_view host) {
    return Decide(host, GetHooks());
}

} // namespace Service::Sockets::Dns

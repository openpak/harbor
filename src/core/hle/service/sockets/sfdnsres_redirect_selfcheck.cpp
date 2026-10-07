// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Self-check for the resolver decision table (sfdnsres_redirect.h). Eden's unit tests are off in
// the Harbor build, so this is a standalone program:
//
//   g++ -std=c++20 -I <src> sfdnsres_redirect_selfcheck.cpp -o dns_selfcheck && ./dns_selfcheck
//
// It prints "resolver decision self-check: OK" and exits 0, or names the failed check and exits 1.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/hle/service/sockets/sfdnsres_redirect.h"

using namespace Service::Sockets::Dns;

namespace {

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAILED line %d: %s\n", __LINE__, #cond);                         \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)

// The switch ceiling the compiled fallback carries, plus the third-party families OpenPak serves
// on the live box (dns_mitm startup log, 2026-10-05).
const std::vector<std::string> Families = {
    ".nintendo.net",   ".nintendo.com",          ".nintendo.co.jp",   ".nintendo-europe.com",
    ".nintendowifi.net", ".photonengine.io",     ".among.us",         ".gamespy.com",
    ".klei.com",       ".kleientertainment.com", ".t17service.com",   ".exitgames.com",
    ".epicgames.dev",  ".ea.com",                ".demonware.net",    ".battle.net",
    ".xboxlive.com",   ".microsoft.com",         ".live.com",         ".mojang.com",
};

bool InCeiling(const std::string& host) {
    for (const auto& family : Families) {
        if (InFamily(host, family)) {
            return true;
        }
    }
    return false;
}

// A profile that redirects two names, overrides one and keeps one away from OpenPak.
Hooks ProfileHooks(bool enabled) {
    Hooks hooks;
    hooks.enabled = [enabled] { return enabled; };
    hooks.route = [](const std::string& host) {
        if (host == "dauth-lp1.ndas.srv.nintendo.net" || host == "lobby-v2.klei.com") {
            return RouteAnswer{Route::Redirect, "145.241.199.19"};
        }
        if (host == "nncs2-lp1.n.n.srv.nintendo.net") {
            return RouteAnswer{Route::Redirect, "145.241.199.20"};
        }
        if (host == "ctest.cdn.nintendo.net") {
            return RouteAnswer{Route::PassThrough, {}};
        }
        if (host == "bogus.nintendo.net") {
            return RouteAnswer{Route::Redirect, "not-an-address"};
        }
        return RouteAnswer{};
    };
    hooks.in_ceiling = InCeiling;
    return hooks;
}

void EveryFamilyFailsClosedWhenUnredirected() {
    const Hooks hooks = ProfileHooks(true);
    for (const auto& family : Families) {
        const std::string apex = family.substr(1);
        for (const std::string& name : {apex, "unredirected." + apex, "a.b.c." + apex + "."}) {
            const Decision d = Decide(name, hooks);
            if (d.verdict != Verdict::FailClosed) {
                std::fprintf(stderr, "%s did not fail closed\n", name.c_str());
            }
            CHECK(d.verdict == Verdict::FailClosed);
        }
    }
}

void ProfileAnswers() {
    const Hooks hooks = ProfileHooks(true);
    Decision d = Decide("dauth-lp1.ndas.srv.nintendo.net.", hooks);
    CHECK(d.verdict == Verdict::Redirect && d.address == "145.241.199.19");
    d = Decide("DAUTH-LP1.NDAS.SRV.NINTENDO.NET", hooks);
    CHECK(d.verdict == Verdict::Redirect);
    d = Decide("nncs2-lp1.n.n.srv.nintendo.net", hooks);
    CHECK(d.verdict == Verdict::Redirect && d.address == "145.241.199.20");
    d = Decide("lobby-v2.klei.com", hooks);
    CHECK(d.verdict == Verdict::Redirect);
    // A `never` name reaches the real resolver, Nintendo's or not.
    CHECK(Decide("ctest.cdn.nintendo.net", hooks).verdict == Verdict::HostDns);
    // A malformed redirect is no redirect: the name still fails closed.
    CHECK(Decide("bogus.nintendo.net", hooks).verdict == Verdict::FailClosed);
}

void OutsideTheCeiling() {
    const Hooks hooks = ProfileHooks(true);
    CHECK(Decide("example.org", hooks).verdict == Verdict::HostDns);
    // Label boundaries: these are not the families they resemble.
    CHECK(Decide("evilnintendo.net", hooks).verdict == Verdict::HostDns);
    CHECK(Decide("a.nintendo.net.evil.example", hooks).verdict == Verdict::HostDns);
    CHECK(Decide("notphotonengine.io", hooks).verdict == Verdict::HostDns);
    // Literals and localhost never touch a resolver for a name.
    CHECK(Decide("145.241.199.19", hooks).verdict == Verdict::HostDns);
    CHECK(Decide("localhost", hooks).verdict == Verdict::HostDns);
    CHECK(Decide("", hooks).verdict == Verdict::FailClosed);
}

void NintendoFailsClosedWithoutOpenPak() {
    // No hooks installed, and OpenPak installed but off: Nintendo names still never resolve.
    for (const Hooks& hooks : {Hooks{}, ProfileHooks(false)}) {
        for (const char* name :
             {"api.hac.lp1.ctest.srv.nintendo.net.", "dauth-lp1.ndas.srv.nintendo.net",
              "atum.hac.lp1.d4c.nintendo.net", "accounts.nintendo.com", "nintendo.com",
              "e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com", "conntest.nintendowifi.net",
              "www.nintendo.co.jp", "ec.nintendoswitch.cn", "store.nintendo.co.uk"}) {
            CHECK(Decide(name, hooks).verdict == Verdict::FailClosed);
        }
        // A third-party family is the profile's only while OpenPak is on.
        CHECK(Decide("ns.exitgames.com", hooks).verdict == Verdict::HostDns);
        // Upstream's own blocks stay.
        CHECK(Decide("login.live.battle.net", hooks).verdict == Verdict::FailClosed);
    }
}

void Nsd() {
    CHECK(NsdResolve("dauth-%.ndas.srv.nintendo.net") == "dauth-lp1.ndas.srv.nintendo.net");
    CHECK(NsdResolve("nncs1-%.n.n.srv.nintendo.net") == "nncs1-lp1.n.n.srv.nintendo.net");
    CHECK(NsdResolve("a%b%c") == "alp1blp1c");
    CHECK(NsdResolve("api.accounts.nintendo.com") ==
          "e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com");
    CHECK(NsdResolve("e97b8a9d672e4ce4845ec6947cd66ef6-sb-api.accounts.nintendo.com") ==
          "e97b8a9d672e4ce4845ec6947cd66ef6-sb.baas.nintendo.com");
    CHECK(NsdResolve("e97b8a9d672e4ce4845ec6947cd66ef6-sb.accounts.nintendo.com") ==
          "e97b8a9d672e4ce4845ec6947cd66ef6-sb.baas.nintendo.com");
    // The Login applet's page stays on its own name.
    CHECK(NsdResolve("accounts.nintendo.com") == "accounts.nintendo.com");
    CHECK(NsdResolve("ctest.cdn.nintendo.net") == "ctest.cdn.nintendo.net");
    CHECK(NsdResolve("example.org") == "example.org");
}

} // namespace

int main() {
    EveryFamilyFailsClosedWhenUnredirected();
    ProfileAnswers();
    OutsideTheCeiling();
    NintendoFailsClosedWithoutOpenPak();
    Nsd();
    std::puts("resolver decision self-check: OK");
    return 0;
}

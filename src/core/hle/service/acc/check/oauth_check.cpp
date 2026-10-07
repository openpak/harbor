// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Self-check of the Nintendo Account OAuth helpers (nintendo_account_oauth.cpp): PKCE, the
// authorize URL, the callback parse, the token request body and the two JSON readers; the
// procedures' state machine and GetRequest's output layout (oauth_procedure_rules.h); and the
// IManagerForSystemService 136 / IAdministrator 290 buffers. No emulator, no network. Build and run from the source root:
//
//   c++ -std=c++20 -Isrc -I<nlohmann_json>/include src/core/hle/service/acc/check/oauth_check.cpp
//       src/core/hle/service/acc/nintendo_account_oauth.cpp -lfmt -lcrypto -o oauth_check
//   ./oauth_check
//
// Prints "oauth_check: all checks passed" and exits 0, or names the failed check and exits 1.

#include <array>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>

#include "core/hle/service/acc/nintendo_account_oauth.h"
#include "core/hle/service/acc/oauth_procedure_rules.h"

namespace oauth = Service::Account::NintendoAccountOAuth;
namespace rules = Service::Account::ProcedureRules;

namespace {

int failures = 0;

void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "  ok  " : "  FAIL", what);
    if (!ok) {
        ++failures;
    }
}

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

std::string At(const std::array<std::uint8_t, oauth::NasUserBaseSize>& base, std::size_t offset) {
    return std::string(reinterpret_cast<const char*>(base.data() + offset));
}

void CheckStateMachine() {
    using rules::Kind;
    using rules::Stage;

    // acc:su 211: GetRequest, Suspend, the Login applet, 212 Resume, ApplyResponseAsync, 110.
    rules::Machine reg{.kind = Kind::Registration};
    Check(rules::CanGetRequest(reg) && rules::PrepareSucceeds(reg) && rules::CanSuspend(reg) &&
              rules::CanApply(reg),
          "registration: a fresh procedure takes a request, a prepare, a suspend, a response");
    Check(!rules::CanReadApplied(reg) && !rules::CanRegister(reg),
          "registration: nothing to read or register before a response");
    Check(rules::AfterSuspend(reg) == Stage::Fresh,
          "registration: Suspend keeps stage 0 on the object");
    reg.stage = rules::AfterResume(Kind::Registration);
    Check(reg.stage == Stage::Fresh && rules::CanApply(reg),
          "registration: a resumed procedure (212/214) takes the response");
    reg.has_code = true;
    Check(!rules::CanApply(reg) && !rules::CanGetRequest(reg),
          "registration: once a callback is taken, no second response or request");
    Check(rules::AfterApply(reg, false) == Stage::Fresh && !rules::CanApply(reg),
          "registration: a failed response stays failed");
    reg.stage = rules::AfterApply(reg, true);
    Check(reg.stage == Stage::Applied && rules::CanReadApplied(reg) && rules::CanRegister(reg),
          "registration: an applied response reads back and registers");
    Check(!rules::CanSuspend(reg) && !rules::PrepareSucceeds(reg),
          "registration: no suspend or prepare after the response");
    reg.registered = true;
    Check(!rules::CanRegister(reg), "registration: registers once");

    // IAdministrator 251: GetRequest, Suspend, the Login applet, 252 Resume, ApplyResponseAsync.
    rules::Machine link{.kind = Kind::Linkage};
    Check(rules::CanGetRequest(link) && rules::CanSuspend(link) && rules::CanApply(link),
          "linkage: a fresh procedure takes a request, a suspend, a response");
    link.stage = rules::AfterSuspend(link);
    Check(link.stage == Stage::Suspended && !rules::CanApply(link) && !rules::CanSuspend(link) &&
              !rules::CanGetRequest(link),
          "linkage: the suspended object takes nothing more");
    link = {.kind = Kind::Linkage, .stage = rules::AfterResume(Kind::Linkage)};
    Check(link.stage == Stage::Resumed && rules::CanApply(link) && rules::CanSuspend(link) &&
              !rules::CanGetRequest(link),
          "linkage: a resumed procedure takes a response or another suspend, not a request");
    link.has_code = true;
    Check(!rules::CanApply(link) && !rules::CanSuspend(link),
          "linkage: no response or suspend once a callback is taken");
    link.stage = rules::AfterApply(link, true);
    Check(link.stage == Stage::Applied && !rules::CanRegister(link),
          "linkage: applied, and a linkage never registers a user");
}

void CheckLayouts(const std::string& url) {
    using rules::RequestLayout;
    Check(rules::PlanRequest({0x1000, 0x100}) == RequestLayout::TwoEntries,
          "GetRequest: two receive-list entries, one per output");
    Check(rules::PlanRequest({0x1100}) == RequestLayout::OneEntry,
          "GetRequest: one entry of 0x1100 takes both back to back");
    Check(rules::PlanRequest({0x1000}) == RequestLayout::Refused &&
              rules::PlanRequest({}) == RequestLayout::Refused &&
              rules::PlanRequest({0x1000, 0}) == RequestLayout::Refused,
          "GetRequest: too few or empty entries are refused");

    const auto url_out = rules::FixedString(url, rules::RequestUrlSize, 0x1000);
    Check(url_out.size() == 0x1000 &&
              std::string(reinterpret_cast<const char*>(url_out.data())) == url,
          "GetRequest: the URL is written whole and NUL terminated into 0x1000 bytes");
    const auto callback = rules::FixedString(oauth::RedirectUri, rules::CallbackUriSize, 0x200);
    Check(callback.size() == 0x100 &&
              std::string(reinterpret_cast<const char*>(callback.data())) == oauth::RedirectUri,
          "GetRequest: the callback is 0x100 bytes even in a bigger entry");
    const auto cut = rules::FixedString(std::string(0x300, 'x'), rules::CallbackUriSize, 0x100);
    Check(cut.size() == 0x100 && cut[0xFF] == 0 && cut[0xFE] == 'x',
          "GetRequest: a long text is cut to leave its NUL");

    const auto base = oauth::NasUserBase(std::string(0x50, 'n'), "1990-01-02", "GB");
    Check(base.size() == 0x270, "136: NasUserBase is 0x270 bytes");
    Check(At(base, 0x1a1) == std::string(0x3f, 'n') && base[0x1a1 + 0x3f] == 0,
          "136: the nickname at 0x1a1, cut to 0x3f and NUL terminated");
    Check(At(base, 0x1e1) == "1990-01-02" && At(base, 0x204) == "GB",
          "136: the birthday at 0x1e1 and the country at 0x204");
    Check(base[0x251] == 2 && base[0x252] == 2 && base[0x253] == 2 && base[0] == 0,
          "136: the three consents read userConfirmed, the rest is zero");

    const auto view = oauth::UserResourceViewUrl("hdr.payload.sig", 0x1000);
    Check(view && *view == "https://accounts.nintendo.com/app/profile?id_token_hint=hdr.payload.sig",
          "290: the 23.0.1 profile page with the token as the hint");
    const std::string prefix = "https://accounts.nintendo.com/app/profile?id_token_hint=";
    Check(oauth::UserResourceViewUrl(std::string(0x1000 - prefix.size() - 1, 't'), 0x1000) &&
              !oauth::UserResourceViewUrl(std::string(0x1000 - prefix.size(), 't'), 0x1000),
          "290: a URL fits only with room for its NUL");
    Check(!oauth::UserResourceViewUrl("t", 0x20), "290: a small buffer is refused");
}

} // namespace

int main() {
    // RFC 7636 appendix B: the S256 challenge of its example verifier.
    Check(oauth::Challenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk") ==
              "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM",
          "S256 challenge matches RFC 7636 appendix B");

    const std::string verifier = oauth::NewVerifier();
    const std::string alphabet =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYXabcdefghijklmnopqrstuvwxyz-._~";
    Check(verifier.size() == 128, "verifier is 128 characters");
    Check(verifier.find_first_not_of(alphabet) == std::string::npos,
          "verifier only uses the module's alphabet");
    Check(oauth::NewVerifier() != verifier, "two verifiers differ");

    const std::string state = oauth::NewState();
    Check(state.size() == 43, "state is 43 characters of base64url");
    Check(state.find_first_of("+/=") == std::string::npos, "state is url-safe without padding");

    const std::string url = oauth::RequestUrl(verifier, state, oauth::RegistrationTheme);
    Check(url.rfind("https://accounts.nintendo.com/connect/1.0.0/authorize?response_type=code"
                    "&code_challenge_method=S256&scope=nx&client_id=6ffd70c434d303c8&state=",
                    0) == 0,
          "authorize URL prefix as the module's format string");
    Check(Contains(url, "&state=" + state + "&code_challenge=" + oauth::Challenge(verifier) +
                            "&theme=simple_authenticate&redirect_uri=nintendo%3A%2F%2Faccount.nx.sys"),
          "authorize URL carries state, challenge, theme 3 and the encoded redirect");
    Check(url.size() < 0x1000, "authorize URL fits the 0x1000 RequestUrl buffer");
    Check(Contains(oauth::RequestUrl(verifier, state, oauth::LinkageTheme), "&theme=intro&"),
          "linkage asks for theme 1 (intro)");

    std::string code;
    Check(oauth::ParseResponse("nintendo://account.nx.sys?state=" + state + "&code=abc%2Bd",
                               state, code) == oauth::ParseError::None &&
              code == "abc+d",
          "query callback with this state gives the unescaped code");
    Check(oauth::ParseResponse("nintendo://account.nx.sys#code=xyz&state=" + state +
                                   "&session_state=1",
                               state, code) == oauth::ParseError::None &&
              code == "xyz",
          "fragment callback, any order, extra keys ignored");
    Check(oauth::ParseResponse("nintendo://account.nx.sys?state=other&code=abc", state, code) ==
                  oauth::ParseError::Mismatch &&
              code.empty(),
          "another state is refused");
    Check(oauth::ParseResponse("nintendo://account.nx.sys?state=" + state, state, code) ==
              oauth::ParseError::Mismatch,
          "no code is refused");
    Check(oauth::ParseResponse("nintendo://account.nx.sys?state=" + state +
                                   "&code=a&error=access_denied",
                               state, code) == oauth::ParseError::Mismatch,
          "an error field is refused");
    Check(oauth::ParseResponse("nintendo://profile.account.nx.sys?state=" + state + "&code=a",
                               state, code) == oauth::ParseError::Mismatch,
          "another redirect is refused");
    Check(oauth::ParseResponse("nintendo://account.nx.sys?state=" + state + "&&code=a", state,
                               code) == oauth::ParseError::Mismatch,
          "an empty pair is refused");
    Check(oauth::ParseResponse("nintendo://account.nx.sys", state, code) ==
              oauth::ParseError::Mismatch,
          "the bare redirect is refused");

    const std::string body = oauth::TokenRequestBody("c0de", verifier, "dtoken");
    Check(body == "grant_type=authorization_code&client_id=6ffd70c434d303c8&scope=openid+offline"
                  "+napps+urn%3Aoauth%3Ainit-sso+user+user.birthday+user.email+user.links"
                  "+user.links%5B%5D.id+user.loginId+user.screenName+user.terms&code_verifier=" +
                      verifier +
                      "&code=c0de&device_authentication_token=dtoken"
                      "&redirect_uri=nintendo%3A%2F%2Faccount.nx.sys",
          "token request body as the module's format string");

    const auto tokens = oauth::ParseTokens(
        R"({"access_token":"AT","expires_in":900,"id_token":"IT","token_type":"Bearer"})");
    Check(tokens && tokens->id_token == "IT" && tokens->access_token == "AT",
          "token answer gives id_token and access_token");
    Check(!oauth::ParseTokens(R"({"error":"invalid_grant"})"), "token error answer is nothing");
    Check(!oauth::ParseTokens("not json"), "non-JSON token answer is nothing");

    const auto me = oauth::ParseUsersMe(
        R"({"id":"0123456789abcdef","nickname":"Tester","birthday":"1990-01-02","country":"GB",
            "iconUri":"https://cdn.example/1/x"})");
    Check(me && me->id == 0x0123456789abcdefULL && me->nickname == "Tester" &&
              me->birthday == "1990-01-02" && me->country == "GB",
          "users/me gives id, nickname, birthday, country");
    Check(!oauth::ParseUsersMe(R"({"id":"not-hex"})"), "users/me with a bad id is nothing");

    Check(oauth::IsUsableBirthday("1990-01-02"), "YYYY-MM-DD is a usable birthday");
    Check(oauth::IsUsableBirthday("0000-00-00"), "0000-00-00 has the usable shape");
    Check(!oauth::IsUsableBirthday("1990-1-2") && !oauth::IsUsableBirthday(""),
          "short birthdays are not usable");

    CheckStateMachine();
    CheckLayouts(url);

    if (failures != 0) {
        std::printf("oauth_check: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("oauth_check: all checks passed\n");
    return 0;
}

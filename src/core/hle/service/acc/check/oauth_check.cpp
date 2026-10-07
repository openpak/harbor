// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Self-check of the Nintendo Account OAuth helpers (nintendo_account_oauth.cpp): PKCE, the
// authorize URL, the callback parse, the token request body and the two JSON readers. No
// emulator, no network. Build and run from the source root:
//
//   c++ -std=c++20 -Isrc -I<nlohmann_json>/include src/core/hle/service/acc/check/oauth_check.cpp
//       src/core/hle/service/acc/nintendo_account_oauth.cpp -lfmt -lcrypto -o oauth_check
//   ./oauth_check
//
// Prints "oauth_check: all checks passed" and exits 0, or names the failed check and exits 1.

#include <cstdio>
#include <set>
#include <string>

#include "core/hle/service/acc/nintendo_account_oauth.h"

namespace oauth = Service::Account::NintendoAccountOAuth;

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

    if (failures != 0) {
        std::printf("oauth_check: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("oauth_check: all checks passed\n");
    return 0;
}

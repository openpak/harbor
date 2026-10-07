// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// The Nintendo Account sign-in the account sysmodule runs for its OAuth procedures (acc:su
// 211-214 user registration, IAdministrator 251/252 linkage), as read from the 22.5.0 module
// (scratch/ryujinx-android/account-oauth/notes.md). The page itself is shown by the firmware's
// Login applet, which stops at RedirectUri and hands the caller the URL it stopped at.
//
// Pure functions only: no network, no kernel, no service code, so the self-check builds it alone.
// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Service::Account::NintendoAccountOAuth {

// nsd's lp1 NasServiceSetting "NxAccount", which the module looks up by name.
constexpr std::uint64_t ClientId = 0x6ffd70c434d303c8;
constexpr std::string_view RedirectUri = "nintendo://account.nx.sys";

// The dauth client whose device token goes with the code exchange (module 0x085f81f0).
constexpr std::string_view DeviceTokenClientId = "81333c548b2e876d";

constexpr std::string_view AccountsHost = "accounts.nintendo.com";
constexpr std::string_view ApiAccountsHost = "api.accounts.nintendo.com";
constexpr std::string_view AuthorizePath = "/connect/1.0.0/authorize";
constexpr std::string_view TokenPath = "/connect/1.0.0/api/token";
constexpr std::string_view UsersMePath = "/2.0.0/users/me";

// Theme table at 0x0868edd0. Registration asks for 3, linkage's GetRequest for 1,
// GetRequestWithTheme for 0..3.
constexpr int RegistrationTheme = 3;
constexpr int LinkageTheme = 1;
constexpr int MaxRequestTheme = 3;
std::string_view ThemeName(int theme);

/// 128 draws from the module's verifier alphabet (its "YX" included, no 'Z').
std::string NewVerifier();
/// base64url of a SHA-256 over a fresh random value: 43 characters.
std::string NewState();
/// base64url(SHA-256(verifier)), no padding.
std::string Challenge(std::string_view verifier);

/// GetRequest's URL (builder 0x086026f0): the format string at 0x0868e598 plus the redirect.
std::string RequestUrl(std::string_view verifier, std::string_view state, int theme);

enum class ParseError {
    None,
    /// Not this procedure's callback: another prefix, a malformed query, another state, an
    /// error field or no code (2124-3122).
    Mismatch,
};

/// ApplyResponse's check of the URL the applet stopped at (0x085f9a70): the redirect, then '?'
/// or '#', then non-empty key=value pairs; the state must be this procedure's and a code must be
/// there. On success `code` holds the unescaped code.
ParseError ParseResponse(std::string_view response, std::string_view state, std::string& code);

/// The token request body (0x08604c00): grant, client, scope, verifier, code, device token,
/// redirect.
std::string TokenRequestBody(std::string_view code, std::string_view verifier,
                             std::string_view device_token);

struct Tokens {
    std::string id_token;
    std::string access_token;
};
/// $.id_token and $.access_token of the token endpoint's answer, or nothing.
std::optional<Tokens> ParseTokens(std::string_view body);

/// The account itself, from users/me: what the module caches as the NA user resource.
struct User {
    std::uint64_t id = 0;
    std::string nickname;
    std::string birthday;
    std::string country;
    std::string icon_uri;
};
/// users/me's answer, or nothing when it has no usable id.
std::optional<User> ParseUsersMe(std::string_view body);

/// Whether a birthday is YYYY-MM-DD with digits only; systemWeb splits it at fixed offsets and
/// asserts on anything shorter.
bool IsUsableBirthday(std::string_view birthday);

// nn::account::nas::NasUserBase, as IManagerForSystemService 136 hands it out: 0x270 bytes. Offsets
// from the account module's cache reader (0x085c44a0): 0x1a1 nickname[0x40], 0x1e1 birthday[0x10],
// 0x204 country[4], 0x251/0x252/0x253 targetMarketing/internalAnalysis/dataCollection
// (0b010 = userConfirmed).
inline constexpr std::size_t NasUserBaseSize = 0x270;
std::array<std::uint8_t, NasUserBaseSize> NasUserBase(std::string_view nickname,
                                                      std::string_view birthday,
                                                      std::string_view country);

/// IAdministrator 290's buffer on 23.0.1 (impl 0x085A6C00, builder 0x08605C80):
/// "https://accounts.nintendo.com/app/profile?id_token_hint=" and the token, NUL terminated, in a
/// 0x1000 buffer. Nothing when it does not fit `capacity` with its NUL (2124-0031).
inline constexpr std::size_t UserResourceViewUrlSize = 0x1000;
std::optional<std::string> UserResourceViewUrl(std::string_view id_token, std::size_t capacity);

std::string PercentEncode(std::string_view text);
std::string PercentDecode(std::string_view text);

} // namespace Service::Account::NintendoAccountOAuth

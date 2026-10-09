// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).

#include <algorithm>
#include <array>
#include <map>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include "core/hle/service/acc/nintendo_account_oauth.h"

namespace Service::Account::NintendoAccountOAuth {

namespace {

// Sent pre-encoded, as the module's string is.
constexpr std::string_view Scope =
    "openid+offline+napps+urn%3Aoauth%3Ainit-sso+user+user.birthday+user.email+user.links"
    "+user.links%5B%5D.id+user.loginId+user.screenName+user.terms";

constexpr std::array<std::string_view, 5> Themes{
    "register", "intro", "email_authentication", "simple_authenticate",
    "simple_authenticate_transfer"};

// The module's verifier alphabet (0x0867d533), with its "YX" and without 'Z'.
constexpr std::string_view VerifierAlphabet =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYXabcdefghijklmnopqrstuvwxyz-._~";

std::string Base64Url(const unsigned char* data, std::size_t size) {
    static constexpr char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    for (std::size_t i = 0; i < size; i += 3) {
        const std::uint32_t n = (std::uint32_t{data[i]} << 16) |
                                (i + 1 < size ? std::uint32_t{data[i + 1]} << 8 : 0) |
                                (i + 2 < size ? std::uint32_t{data[i + 2]} : 0);
        out += table[(n >> 18) & 63];
        out += table[(n >> 12) & 63];
        if (i + 1 < size) {
            out += table[(n >> 6) & 63];
        }
        if (i + 2 < size) {
            out += table[n & 63];
        }
    }
    return out;
}

std::string Sha256Url(const unsigned char* data, std::size_t size) {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256(data, size, digest.data());
    return Base64Url(digest.data(), digest.size());
}

std::string Text(const nlohmann::json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

} // namespace

std::string_view ThemeName(int theme) {
    return theme >= 0 && theme < static_cast<int>(Themes.size()) ? Themes[theme] : Themes[0];
}

std::string NewVerifier() {
    std::array<unsigned char, 128> random{};
    RAND_bytes(random.data(), static_cast<int>(random.size()));

    // 256 is not a multiple of the alphabet's 66 letters: redraw the values past the last whole
    // multiple so every letter is equally likely.
    constexpr unsigned limit = 256 - 256 % VerifierAlphabet.size();

    std::string verifier(random.size(), '0');
    for (std::size_t i = 0; i < random.size(); ++i) {
        unsigned char value = random[i];
        while (value >= limit) {
            RAND_bytes(&value, 1);
        }
        verifier[i] = VerifierAlphabet[value % VerifierAlphabet.size()];
    }
    return verifier;
}

std::string NewState() {
    std::array<unsigned char, 16> random{};
    RAND_bytes(random.data(), static_cast<int>(random.size()));
    return Sha256Url(random.data(), random.size());
}

std::string Challenge(std::string_view verifier) {
    return Sha256Url(reinterpret_cast<const unsigned char*>(verifier.data()), verifier.size());
}

std::string PercentEncode(std::string_view text) {
    std::string out;
    for (const char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if ((u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') ||
            u == '-' || u == '.' || u == '_' || u == '~') {
            out += c;
        } else {
            out += fmt::format("%{:02X}", u);
        }
    }
    return out;
}

std::string PercentDecode(std::string_view text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') {
                return c - '0';
            }
            if (c >= 'a' && c <= 'f') {
                return c - 'a' + 10;
            }
            if (c >= 'A' && c <= 'F') {
                return c - 'A' + 10;
            }
            return -1;
        };
        if (text[i] == '%' && i + 2 < text.size() && hex(text[i + 1]) >= 0 &&
            hex(text[i + 2]) >= 0) {
            out += static_cast<char>(hex(text[i + 1]) * 16 + hex(text[i + 2]));
            i += 2;
        } else {
            out += text[i];
        }
    }
    return out;
}

std::string RequestUrl(std::string_view verifier, std::string_view state, int theme) {
    return fmt::format("https://{}{}?response_type=code&code_challenge_method=S256&scope=nx"
                       "&client_id={:016x}&state={}&code_challenge={}&theme={}&redirect_uri={}",
                       AccountsHost, AuthorizePath, ClientId, state, Challenge(verifier),
                       ThemeName(theme), PercentEncode(RedirectUri));
}

ParseError ParseResponse(std::string_view response, std::string_view state, std::string& code) {
    code.clear();

    if (response.size() <= RedirectUri.size() ||
        response.substr(0, RedirectUri.size()) != RedirectUri ||
        (response[RedirectUri.size()] != '?' && response[RedirectUri.size()] != '#')) {
        return ParseError::Mismatch;
    }

    std::map<std::string, std::string, std::less<>> fields;
    std::string_view rest = response.substr(RedirectUri.size() + 1);
    while (true) {
        const std::size_t amp = rest.find('&');
        const std::string_view pair = rest.substr(0, amp);
        const std::size_t equals = pair.find('=');
        if (equals == std::string_view::npos || equals == 0) {
            return ParseError::Mismatch;
        }
        fields[std::string{pair.substr(0, equals)}] = PercentDecode(pair.substr(equals + 1));
        if (amp == std::string_view::npos) {
            break;
        }
        rest = rest.substr(amp + 1);
    }

    // The module maps some error names to results of their own (0x08601f90, table not read);
    // every error here is the mismatch it falls back to.
    const auto state_it = fields.find("state");
    const auto code_it = fields.find("code");
    if (fields.contains("error") || state_it == fields.end() || state_it->second != state ||
        code_it == fields.end() || code_it->second.empty()) {
        return ParseError::Mismatch;
    }

    code = code_it->second;
    return ParseError::None;
}

std::string TokenRequestBody(std::string_view code, std::string_view verifier,
                             std::string_view device_token) {
    return fmt::format("grant_type=authorization_code&client_id={:016x}&scope={}"
                       "&code_verifier={}&code={}&device_authentication_token={}&redirect_uri={}",
                       ClientId, Scope, verifier, PercentEncode(code), device_token,
                       PercentEncode(RedirectUri));
}

std::optional<Tokens> ParseTokens(std::string_view body) {
    const nlohmann::json parsed = nlohmann::json::parse(body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return std::nullopt;
    }
    Tokens tokens{Text(parsed, "id_token"), Text(parsed, "access_token")};
    if (tokens.id_token.empty() || tokens.access_token.empty()) {
        return std::nullopt;
    }
    return tokens;
}

std::optional<User> ParseUsersMe(std::string_view body) {
    const nlohmann::json parsed = nlohmann::json::parse(body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return std::nullopt;
    }
    const std::string id = Text(parsed, "id");
    if (id.empty() || id.size() > 16 ||
        id.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
        return std::nullopt;
    }
    User user;
    user.id = std::stoull(id, nullptr, 16);
    user.nickname = Text(parsed, "nickname");
    user.birthday = Text(parsed, "birthday");
    user.country = Text(parsed, "country");
    user.icon_uri = Text(parsed, "iconUri");
    user.screen_name = Text(parsed, "screenName");
    user.email = Text(parsed, "email");
    user.login_id = Text(parsed, "loginId");
    user.gender = Text(parsed, "gender");
    user.language = Text(parsed, "language");
    user.region = Text(parsed, "region");
    if (const auto zone = parsed.find("timezone"); zone != parsed.end() && zone->is_object()) {
        user.timezone = Text(*zone, "id");
    }
    return user;
}

std::array<std::uint8_t, NasUserBaseSize> NasUserBase(std::string_view nickname,
                                                      std::string_view birthday,
                                                      std::string_view country,
                                                      const NasUserNames& names) {
    std::array<std::uint8_t, NasUserBaseSize> out{};
    const auto field = [&out](std::size_t offset, std::size_t size, std::string_view value) {
        std::copy_n(value.begin(), std::min(value.size(), size - 1), out.begin() + offset);
    };
    field(0x1, 0x80, names.screen_name);
    field(0x81, 0x100, names.email);
    field(0x181, 0x20, names.login_id);
    field(0x1a1, 0x40, nickname);
    field(0x1f1, 8, names.gender);
    field(0x1fc, 8, names.language);
    field(0x208, 4, names.region);
    field(0x20c, 0x40, names.timezone);
    field(0x1e1, 0x10, birthday);
    field(0x204, 4, country);
    out[0x251] = out[0x252] = out[0x253] = 0b010;
    return out;
}

std::array<std::uint8_t, NasUserBaseForApplicationSize> NasUserBaseForApplication(
    std::string_view birthday, std::string_view country, const NasUserNames& names) {
    std::array<std::uint8_t, NasUserBaseForApplicationSize> out{};
    // Fixed-width fields as the module copies them: no room kept for a terminator.
    const auto field = [&out](std::size_t offset, std::size_t size, std::string_view value) {
        std::copy_n(value.begin(), std::min(value.size(), size), out.begin() + offset);
    };
    field(0x0, 0x10, birthday);
    field(0x10, 8, names.gender);
    field(0x18, 8, names.language);
    field(0x20, 4, country);
    field(0x24, 4, names.region);
    field(0x28, 0x40, names.timezone);
    return out;
}

std::optional<std::string> UserResourceViewUrl(std::string_view id_token, std::size_t capacity) {
    std::string url = fmt::format("https://{}/app/profile?id_token_hint={}", AccountsHost, id_token);
    if (url.size() + 1 > std::min(capacity, UserResourceViewUrlSize)) {
        return std::nullopt;
    }
    return url;
}

bool IsUsableBirthday(std::string_view birthday) {
    if (birthday.size() != 10 || birthday[4] != '-' || birthday[7] != '-') {
        return false;
    }
    for (std::size_t i = 0; i < birthday.size(); ++i) {
        if (i != 4 && i != 7 && (birthday[i] < '0' || birthday[i] > '9')) {
            return false;
        }
    }
    return true;
}

} // namespace Service::Account::NintendoAccountOAuth

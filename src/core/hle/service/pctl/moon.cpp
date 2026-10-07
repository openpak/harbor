// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).

#include <array>
#include <limits>
#include <map>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "core/hle/service/pctl/moon.h"

namespace Service::PCTL::Moon {

namespace {

using json = nlohmann::json;

constexpr std::string_view ErrorBase = "https://moon.nintendo.com/errors/v1/";

// The sixteen identities pctl classifies (.text 0x5ab80), by the 2142-2xxx description they
// become. 2005 has no identity.
const std::map<std::string, Description, std::less<>>& Identities() {
    static const std::map<std::string, Description, std::less<>> identities{
        {"503/under_maintenance", 2001},      {"410/service_terminated", 2002},
        {"500/internal_server_error", 2003},  {"400/invalid_params", 2004},
        {"401/invalid_token", 2006},          {"403/insufficient_scope", 2007},
        {"403/forbidden", 2008},              {"404/resource_is_not_found", 2009},
        {"405/method_not_allowed", 2010},     {"406/not_acceptable_language", 2011},
        {"409/resource_already_exists", 2012}, {"424/failed_dependency", 2013},
        {"400/firmware_update_required", 2014}, {"404/resource_not_available", 2015},
        {"409/over_the_time_limit", 2016},    {"400/devices_maxed", 2017},
    };
    return identities;
}

bool TryParse(std::string_view body, json& out) {
    out = json::parse(body, nullptr, false);
    return !out.is_discarded();
}

/// The key, or nullptr when absent or null: every Optional* below is one typed key of a parse
/// table, where absent and null are fine and the wrong type is a refusal.
const json* Present(const json& parent, const char* name) {
    const auto it = parent.find(name);
    if (it == parent.end() || it->is_null()) {
        return nullptr;
    }
    return &*it;
}

bool OptionalInteger(const json& parent, const char* name, std::int64_t& value) {
    const json* element = Present(parent, name);
    if (element == nullptr) {
        return true;
    }
    if (element->is_number_unsigned()) {
        const auto number = element->get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return false;
        }
        value = static_cast<std::int64_t>(number);
        return true;
    }
    if (element->is_number_integer()) {
        value = element->get<std::int64_t>();
        return true;
    }
    return false;
}

bool OptionalString(const json& parent, const char* name, std::string& value) {
    const json* element = Present(parent, name);
    if (element == nullptr) {
        return true;
    }
    if (!element->is_string()) {
        return false;
    }
    value = element->get<std::string>();
    return true;
}

/// An array (or nothing): `value` is null when there is none.
bool OptionalArray(const json& parent, const char* name, const json*& value) {
    value = Present(parent, name);
    return value == nullptr || value->is_array();
}

bool OptionalObject(const json& parent, const char* name, const json*& value) {
    value = Present(parent, name);
    return value == nullptr || value->is_object();
}

/// An id as pctl reads one (.text 0x50f00): a string of one to sixteen hex digits, all consumed.
/// A number refuses.
bool ParseHex(std::string_view text, std::uint64_t& value) {
    if (text.empty() || text.size() > 16) {
        return false;
    }
    std::uint64_t out = 0;
    for (const char c : text) {
        int digit;
        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            digit = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            digit = c - 'A' + 10;
        } else {
            return false;
        }
        out = (out << 4) | static_cast<std::uint64_t>(digit);
    }
    value = out;
    return true;
}

bool OptionalHexId(const json& parent, const char* name, std::uint64_t& value) {
    value = 0;
    const json* element = Present(parent, name);
    if (element == nullptr) {
        return true;
    }
    return element->is_string() && ParseHex(element->get_ref<const std::string&>(), value);
}

std::string Base64UrlDecode(std::string_view text) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') {
            return c - 'A';
        }
        if (c >= 'a' && c <= 'z') {
            return c - 'a' + 26;
        }
        if (c >= '0' && c <= '9') {
            return c - '0' + 52;
        }
        if (c == '-' || c == '+') {
            return 62;
        }
        if (c == '_' || c == '/') {
            return 63;
        }
        return -1;
    };
    std::string out;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const char c : text) {
        if (c == '=') {
            break;
        }
        const int v = value(c);
        if (v < 0) {
            return {};
        }
        buffer = (buffer << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
        }
    }
    return out;
}

} // namespace

std::vector<std::pair<std::string, std::string>> RequestHeaders(std::string_view token,
                                                                std::string_view if_none_match) {
    std::vector<std::pair<std::string, std::string>> headers{
        {"User-Agent", std::string{UserAgent}},
        {"X-Nintendo-SDK-Version", std::string{SdkVersion}},
        {"Accept", "application/json"},
    };
    if (!token.empty()) {
        headers.emplace_back("Authorization", fmt::format("Bearer {}", token));
    }
    if (!if_none_match.empty()) {
        headers.emplace_back("If-None-Match", std::string{if_none_match});
    }
    return headers;
}

std::string SearchPath(std::string_view code) {
    return fmt::format("/moon/v1/devices?filter.links.pairingCode.code.$eq={}", code);
}

std::string DevicePath(std::uint64_t device_id, std::string_view tail) {
    return fmt::format("/moon/v1/devices/{:016x}{}", device_id, tail);
}

Description MapError(std::string_view body) {
    json document;
    if (!TryParse(body, document) || !document.is_object()) {
        return UnknownIdentity;
    }
    const auto type = document.find("type");
    if (type == document.end() || !type->is_string()) {
        return UnknownIdentity;
    }
    const auto& uri = type->get_ref<const std::string&>();
    if (uri.substr(0, ErrorBase.size()) != ErrorBase) {
        return UnknownIdentity;
    }
    const auto& identities = Identities();
    const auto it = identities.find(std::string_view{uri}.substr(ErrorBase.size()));
    return it != identities.end() ? it->second : UnknownIdentity;
}

Description ParseSearch(std::string_view body, std::uint64_t& device_id) {
    device_id = 0;
    json root;
    if (!TryParse(body, root)) {
        return Malformed;
    }
    std::int64_t count = 0;
    const json* items = nullptr;
    if (!root.is_object() || !OptionalInteger(root, "count", count) ||
        !OptionalArray(root, "items", items)) {
        return Refused;
    }
    bool first = true;
    if (items != nullptr) {
        for (const auto& item : *items) {
            std::uint64_t id = 0;
            if (!item.is_object() || !OptionalHexId(item, "id", id)) {
                return Refused;
            }
            if (first) {
                device_id = id;
                first = false;
            }
        }
    }
    // The executor checks the count, not the list (.text 0x5f328).
    return count == 1 ? Success : PairingCodeNotFound;
}

Description ParseOwners(std::string_view body, std::vector<Owner>& owners) {
    owners.clear();
    json root;
    if (!TryParse(body, root)) {
        return Malformed;
    }
    std::int64_t count = 0;
    const json* items = nullptr;
    if (!root.is_object() || !OptionalInteger(root, "count", count) ||
        !OptionalArray(root, "items", items)) {
        return Refused;
    }
    if (items == nullptr) {
        return Success;
    }
    for (const auto& item : *items) {
        Owner owner;
        const json* user = nullptr;
        if (!item.is_object() || !OptionalHexId(item, "deviceId", owner.device_id) ||
            !OptionalObject(item, "user", user)) {
            return Refused;
        }
        if (user != nullptr) {
            if (!OptionalHexId(*user, "nintendoAccountId", owner.nintendo_account_id) ||
                !OptionalString(*user, "nickname", owner.nickname)) {
                return Refused;
            }
            if (const auto icon = user->find("iconUri"); icon != user->end()) {
                // 23.0.1's parse table (0x2484a8): a non-nullable object.
                if (!icon->is_object() || !OptionalString(*icon, "small", owner.mii_url)) {
                    return Refused;
                }
            } else {
                const json* mii = nullptr;
                if (!OptionalObject(*user, "miiUri", mii) ||
                    (mii != nullptr && !OptionalString(*mii, "small", owner.mii_url))) {
                    return Refused;
                }
            }
        }
        if (owners.size() < MaxOwners) {
            owner.nickname = TruncateUtf8(owner.nickname, NicknameBytes);
            owners.push_back(std::move(owner));
        }
    }
    return Success;
}

Description ParseActivate(std::string_view body, std::string& etag,
                          std::int64_t& synchronized_at) {
    etag.clear();
    synchronized_at = 0;
    json root;
    if (!TryParse(body, root)) {
        return Malformed;
    }
    const json* sync = nullptr;
    if (!root.is_object() || !OptionalObject(root, "synchronizedParentalControlSetting", sync)) {
        return Refused;
    }
    if (sync != nullptr && (!OptionalString(*sync, "synchronizedEtag", etag) ||
                            !OptionalInteger(*sync, "synchronizedAt", synchronized_at) ||
                            etag.size() >= 96)) {
        return Refused;
    }
    return Success;
}

std::string ActivateBody(std::string_view language, std::string_view region,
                         std::string_view serial_number, std::string_view displayed_version,
                         std::uint32_t internal_version, std::int64_t now,
                         std::string_view notification_token) {
    nlohmann::ordered_json body = nlohmann::ordered_json::object();
    if (!language.empty()) {
        body["language"] = language;
    }
    if (!region.empty()) {
        body["region"] = region;
    }
    if (!serial_number.empty()) {
        body["serialNumber"] = serial_number;
    }
    if (!displayed_version.empty()) {
        body["firmwareVersion"] = {{"displayedVersion", displayed_version},
                                   {"internalVersion", internal_version}};
    }
    body["activated"] = true;
    body["lastOnlineCheckedAt"] = now;
    if (!notification_token.empty()) {
        body["notificationToken"] = notification_token;
    }
    return body.dump();
}

std::string ReceiptBody(std::string_view etag, std::int64_t synchronized_at, std::string_view pin,
                        std::string_view notification_token) {
    nlohmann::ordered_json body = nlohmann::ordered_json::object();
    // nx-pctl keeps it from any device POST: a console paired before gets pushes after a sync.
    if (!notification_token.empty()) {
        body["notificationToken"] = notification_token;
    }
    if (!pin.empty()) {
        body["synchronizedUnlockCode"] = pin;
    }
    body["synchronizedParentalControlSetting"] = {{"synchronizedEtag", etag},
                                                  {"synchronizedAt", synchronized_at}};
    return body.dump();
}

std::string_view RegionName(std::uint32_t region_code) {
    switch (region_code) {
    case 0:
        return "JAPAN";
    case 1:
        return "USA";
    case 2:
        return "EUROPE";
    case 3:
        return "AUSTRALIA";
    case 4:
        return "HONGKONG_TAIWAN_KOREA_SOUTHEASTASIA";
    default:
        return "UNDEFINED";
    }
}

std::optional<std::string> SerialFromToken(std::string_view token) {
    const auto first = token.find('.');
    if (first == std::string_view::npos) {
        return std::nullopt;
    }
    const auto second = token.find('.', first + 1);
    if (second == std::string_view::npos || token.find('.', second + 1) != std::string_view::npos) {
        return std::nullopt;
    }
    const std::string payload = Base64UrlDecode(token.substr(first + 1, second - first - 1));
    json claims;
    if (payload.empty() || !TryParse(payload, claims) || !claims.is_object()) {
        return std::nullopt;
    }
    const auto nintendo = claims.find("nintendo");
    if (nintendo == claims.end() || !nintendo->is_object()) {
        return std::nullopt;
    }
    const auto sn = nintendo->find("sn");
    if (sn == nintendo->end() || !sn->is_string()) {
        return std::nullopt;
    }
    return sn->get<std::string>();
}

std::string TruncateUtf8(std::string_view text, std::size_t bytes) {
    if (text.size() <= bytes) {
        return std::string{text};
    }
    std::size_t size = bytes;
    while (size > 0 && (static_cast<unsigned char>(text[size]) & 0xC0) == 0x80) {
        --size;
    }
    return std::string{text.substr(0, size)};
}

std::string LanguageName(std::uint64_t code) {
    std::string out;
    for (int i = 0; i < 8; ++i) {
        const char c = static_cast<char>((code >> (8 * i)) & 0xFF);
        if (c == '\0') {
            break;
        }
        out.push_back(c);
    }
    return out;
}

} // namespace Service::PCTL::Moon

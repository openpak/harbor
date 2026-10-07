// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// The parental-controls API on api-lp1.pctl.srv.nintendo.net ("moon"), spoken the way the 23.0.1
// pctl sysmodule speaks it (22.5.0 and 23.0.1 send the same routes, methods and bodies; 23.0.1
// changed only its SDK version and the owners list's picture key). OpenPak's nx-pctl answers it.
// Sources: the docs repo's switch/parental-controls-firmware-2026-09-26.md (§1 headers, §5b parse
// tables and error identities) and the first hardware pairing log.
//
// Every parse follows the console's typed parse table: a known key with the wrong JSON type ends
// the parse as 2142-0234, unknown keys are skipped, malformed JSON is 2142-0232. Pure functions:
// no network, no kernel, so check/pctl_check.cpp builds them alone. Nothing here logs.
// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Service::PCTL::Moon {

inline constexpr std::string_view Host = "api-lp1.pctl.srv.nintendo.net";

/// pctl's dauth client id: the u64 its token request passes (pctl 22.5.0 .text 0x4f530).
inline constexpr std::string_view DauthClientId = "dc656ea03b63cf68";

inline constexpr std::string_view UserAgent = "HAC-pctl/2.0/P00 (SDK 23.3.0.0; Add-on 23.3.0.0)";
inline constexpr std::string_view SdkVersion = "23.3.0.0";

/// The console keeps at most five owners per device (the search executor's count, .text 0x2f3ac).
inline constexpr std::size_t MaxOwners = 5;

/// The console copies at most 32 bytes of a nickname (study §5b).
inline constexpr std::size_t NicknameBytes = 0x20;

/// A pctl result description (2142-xxxx). 0 is success.
using Description = int;
inline constexpr Description Success = 0;
inline constexpr Description PairingCodeNotFound = 201;
inline constexpr Description Malformed = 232;
inline constexpr Description Refused = 234;
inline constexpr Description UnknownIdentity = 139;

struct Owner {
    std::uint64_t device_id = 0;
    std::uint64_t nintendo_account_id = 0;
    std::string nickname;
    std::string mii_url; ///< Empty when the server gives none.
};

/// The console's request headers (study §1): User-Agent, X-Nintendo-SDK-Version, Accept, then the
/// dauth token as the bearer and, when given, If-None-Match. The body, when there is one, goes as
/// application/json.
std::vector<std::pair<std::string, std::string>> RequestHeaders(std::string_view token,
                                                                std::string_view if_none_match);

/// "/moon/v1/devices?filter.links.pairingCode.code.$eq=<code>".
std::string SearchPath(std::string_view code);
/// "/moon/v1/devices/<16 hex><tail>".
std::string DevicePath(std::uint64_t device_id, std::string_view tail = {});

/// A refusal as the console classifies it: only the body's `type` counts, against the sixteen
/// identities (2142-2001..2017, no 2005); anything else, a body that is no moon error included,
/// is 2142-0139.
Description MapError(std::string_view body);

/// The pairing-code search: {count, items: [{id}]}. Exactly one hit (by count) is a device;
/// anything else is 2142-0201.
Description ParseSearch(std::string_view body, std::uint64_t& device_id);

/// The device's owners: {count, items: [{deviceId, user: {nintendoAccountId, nickname,
/// iconUri: {small}}}]}. The first five are kept. pctl 23.0.1 reads iconUri as a non-nullable
/// object; 22.5.0's nullable miiUri is read when iconUri is absent.
Description ParseOwners(std::string_view body, std::vector<Owner>& owners);

/// The activate answer: {synchronizedParentalControlSetting: {synchronizedEtag, synchronizedAt}}.
Description ParseActivate(std::string_view body, std::string& etag, std::int64_t& synchronized_at);

/// What the console tells the server about itself at activate, in the hardware body's key order.
/// Empty strings are left out. No timeZone and no parentalControlSetting (no local setting).
std::string ActivateBody(std::string_view language, std::string_view region,
                         std::string_view serial_number, std::string_view displayed_version,
                         std::uint32_t internal_version, std::int64_t now,
                         std::string_view notification_token);

/// The sync receipt POSTed to /devices/{id} after applying a setting: the etag now held and,
/// when one is set, the PIN in force as synchronizedUnlockCode.
std::string ReceiptBody(std::string_view etag, std::int64_t synchronized_at, std::string_view pin,
                        std::string_view notification_token);

/// The region names pctl writes, in nn::settings RegionCode order (rodata 0x13b924).
std::string_view RegionName(std::uint32_t region_code);

/// The `nintendo.sn` claim of a dauth device token: the serial dauth knows this device by.
std::optional<std::string> SerialFromToken(std::string_view token);

/// The longest prefix of `text` that fits `bytes` UTF-8 bytes, cut on a character.
std::string TruncateUtf8(std::string_view text, std::size_t bytes);

/// The language code set:sys holds (an s64 of ASCII, "en-GB"), as text.
std::string LanguageName(std::uint64_t code);

} // namespace Service::PCTL::Moon

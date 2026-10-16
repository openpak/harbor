// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later
// Derived from openpak-client 3fd9ba7 (friend_structs.cpp).

// The builders behind friend_structs.h, ported from the the earlier Ryujinx port's OpenPakFriends.cs
// and OpenPakFriends.Structs.cs.

#include "core/openpak/friend_structs.h"
#include "core/openpak/platform.h"

#include <algorithm>
#include <string>
#include <sstream>
#include <fstream>
#include <chrono>
#include <cstring>
#include <iterator>
#include <type_traits>

#include <nlohmann/json.hpp>

namespace OpenPak::Friends {

// No implicit padding anywhere: every gap is a named, zeroed member, so a value-initialised
// struct is all zeros except what a builder writes. (The request structs hold a union of
// differently sized members, which the trait cannot see through; their offsets are pinned above.)
static_assert(std::has_unique_object_representations_v<FriendImpl>);
static_assert(std::has_unique_object_representations_v<FriendForViewerImpl>);
static_assert(std::has_unique_object_representations_v<FriendForViewerImplV2>);
static_assert(std::has_unique_object_representations_v<FriendDetailedInfoImpl>);
static_assert(std::has_unique_object_representations_v<FriendSettingImpl>);
static_assert(std::has_unique_object_representations_v<FriendSettingImplV2>);
static_assert(std::has_unique_object_representations_v<ProfileImpl>);
static_assert(std::has_unique_object_representations_v<ProfileExtraImpl>);
static_assert(std::has_unique_object_representations_v<ProfileExtraImplV2>);
static_assert(std::has_unique_object_representations_v<BlockedUserImpl>);
static_assert(std::has_unique_object_representations_v<BlockedUserImplV2>);
static_assert(std::has_unique_object_representations_v<UserSettingImpl>);
static_assert(std::has_unique_object_representations_v<UserSettingImplV2>);
static_assert(std::has_unique_object_representations_v<FriendInvitationForViewerImpl>);
static_assert(std::has_unique_object_representations_v<FriendInvitationForViewerImplV2>);
static_assert(std::has_unique_object_representations_v<FriendInvitationGroupImpl>);
static_assert(std::has_unique_object_representations_v<FriendInvitationGroupImplV2>);
static_assert(std::has_unique_object_representations_v<UserPresenceImpl>);
static_assert(std::has_unique_object_representations_v<UserPresenceViewImpl>);
static_assert(std::has_unique_object_representations_v<ApplicationInfoV2>);
static_assert(std::has_unique_object_representations_v<SizedFriendFilter>);

namespace {

/// The length of the well-formed UTF-8 sequence at `bytes[i]`, or 0 when it is not one; in that
/// case `bad` is the length of its maximal ill-formed subpart (at least 1), which is what one
/// U+FFFD replaces. This is the Unicode table (§3.9, D92): no overlongs, no surrogates, nothing
/// past U+10FFFF -- the same bytes a strict .NET UTF8Encoding refuses.
std::size_t Utf8Sequence(std::string_view bytes, std::size_t i, std::size_t& bad) {
    const auto at = [&](std::size_t k) -> int {
        return i + k < bytes.size() ? static_cast<unsigned char>(bytes[i + k]) : -1;
    };
    const int lead = at(0);
    bad = 1;
    if (lead < 0x80) {
        return 1;
    }
    int length = 0;
    int low = 0x80;
    int high = 0xBF;
    if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        length = 3;
        low = lead == 0xE0 ? 0xA0 : 0x80;
        high = lead == 0xED ? 0x9F : 0xBF;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
        low = lead == 0xF0 ? 0x90 : 0x80;
        high = lead == 0xF4 ? 0x8F : 0xBF;
    } else {
        return 0;
    }
    for (int k = 1; k < length; ++k) {
        const int byte = at(static_cast<std::size_t>(k));
        const int min = k == 1 ? low : 0x80;
        const int max = k == 1 ? high : 0xBF;
        if (byte < min || byte > max) {
            bad = static_cast<std::size_t>(k);
            return 0;
        }
    }
    return static_cast<std::size_t>(length);
}

bool ValidUtf8(std::string_view bytes) {
    for (std::size_t i = 0; i < bytes.size();) {
        std::size_t bad = 0;
        const std::size_t length = Utf8Sequence(bytes, i, bad);
        if (length == 0) {
            return false;
        }
        i += length;
    }
    return true;
}

std::string Utf8Lossy(std::string_view bytes) {
    std::string out;
    out.reserve(bytes.size());
    for (std::size_t i = 0; i < bytes.size();) {
        std::size_t bad = 0;
        const std::size_t length = Utf8Sequence(bytes, i, bad);
        if (length == 0) {
            out += "\xEF\xBF\xBD";
            i += bad;
        } else {
            out.append(bytes.substr(i, length));
            i += length;
        }
    }
    return out;
}

/// TryHexText, as the reference parses a catalog id half: leading spaces, an optional '+', an
/// optional 0x, then the run of hex digits that follows (1 to 16 of them). What comes after
/// the run is not looked at.
bool TryHexText(std::string_view text, std::uint64_t& value) {
    value = 0;
    while (!text.empty() && text.front() == ' ') {
        text.remove_prefix(1);
    }
    if (!text.empty() && text.front() == '+') {
        text.remove_prefix(1);
    }
    if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text.remove_prefix(2);
    }
    std::size_t digits = 0;
    std::uint64_t parsed = 0;
    for (; digits < text.size(); ++digits) {
        const char c = text[digits];
        int nibble;
        if (c >= '0' && c <= '9') {
            nibble = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            nibble = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            nibble = c - 'A' + 10;
        } else {
            break;
        }
        parsed = (parsed << 4) | static_cast<std::uint64_t>(nibble);
    }
    if (digits == 0 || digits > 16) {
        return false;
    }
    value = parsed;
    return true;
}

template <std::size_t N>
std::span<char> Field(char (&field)[N]) {
    return std::span<char>(field, N);
}

void WriteLittleEndian(std::uint8_t* destination, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        destination[i] = static_cast<std::uint8_t>(value >> (8 * i));
    }
}

/// The route union: the external catalog id when the request carries one, else the NNID Mii
/// data when it carries that, else zeros. The two variants never mix on the wire -- each send
/// variant writes exactly one.
template <typename Union>
void WriteRoute(const Baas::Route& route, Union& destination) {
    std::uint64_t high = 0;
    std::uint64_t low = 0;
    if (TryCatalogId(route.catalog_id, high, low)) {
        WriteLittleEndian(destination.catalog_id, high);
        WriteLittleEndian(destination.catalog_id + 8, low);
        return;
    }
    FixedUtf8(Field(destination.nnid.mii_name), route.mii_name);
    FixedUtf8(Field(destination.nnid.mii_image_url_param), route.mii_image_url_param);
}

template <typename Impl, typename Request>
void FillRequest(Impl& impl, const Request& request, const Uid& user_id, int list_type) {
    impl.user_id = user_id;
    impl.request_id = request.id;
    impl.other_user_id = request.other_id;
    impl.nickname = ToNickname(request.nickname);
    impl.list_type = static_cast<std::uint32_t>(list_type);
    impl.channel = static_cast<std::uint32_t>(request.channel);
    impl.state = static_cast<std::uint32_t>(request.state);
    impl.route_application_id = request.route.application_id;
    impl.route_presence_group_id = request.route.presence_group_id;
    impl.created_at = request.created_at;
    impl.is_read = request.read ? 1 : 0;
    impl.is_valid = 1;
    FixedUtf8(Field(impl.thumbnail_url), request.thumbnail_url);
    FixedUtf8(Field(impl.route_name), request.route.name);
    FixedUtf8(Field(impl.route_language), request.route.language);
    WriteRoute(request.route, impl.route);
}

template <typename Impl>
void FillBlock(Impl& impl, const Baas::Block& block, const Uid& user_id) {
    impl.user_id = user_id;
    impl.network_user_id = block.id;
    impl.nickname = ToNickname(block.nickname);
    impl.reason = static_cast<std::uint32_t>(block.reason);
    impl.application_id = block.route.application_id;
    impl.presence_group_id = block.route.presence_group_id;
    impl.created_at = block.created_at;
    impl.is_valid = 1;
    FixedUtf8(Field(impl.thumbnail_url), block.thumbnail_url);
    FixedUtf8(Field(impl.route_name), block.route.name);
    FixedUtf8(Field(impl.route_language), block.route.language);
}

template <typename Impl>
void FillUserSetting(Impl& impl, const Baas::UserSetting& setting, const Uid& user_id) {
    impl.user_id = user_id;
    impl.presence_permission = static_cast<std::uint32_t>(setting.presence_permission);
    impl.play_log_permission = static_cast<std::uint32_t>(setting.play_log_permission);
    impl.friend_request_reception = setting.friend_request_reception ? 1 : 0;
    impl.friend_code_regenerable_at = setting.friend_code_regenerable_at;
    impl.network_user_id = setting.id;
    impl.nickname = ToNickname(setting.nickname);
    impl.is_valid = 1;
    FixedUtf8(Field(impl.friend_code.code), setting.friend_code);
    FixedUtf8(Field(impl.thumbnail_url), setting.thumbnail_url);
}

template <typename Impl>
void FillProfile(Impl& impl, const Baas::User& user) {
    impl.network_user_id = user.id;
    impl.nickname = ToNickname(user.nickname);
    impl.is_valid = 1;
    FixedUtf8(Field(impl.thumbnail_url), user.thumbnail_url);
}

template <typename Impl>
void FillInvitation(Impl& impl, const Baas::Invitation& invitation) {
    impl.invitation_id = invitation.id;
    impl.group_id = invitation.group_id;
    impl.sender_id = invitation.sender_id;
    impl.application_id = invitation.application_id;
    impl.application_group_id = invitation.application_group_id;
    const std::size_t size = (std::min)(invitation.application_data.size(), kApplicationDataSize);
    impl.application_data_size = static_cast<std::uint32_t>(size);
    impl.created_at = invitation.created_at;
    impl.is_read = invitation.read ? 1 : 0;
    impl.application_id_match = invitation.application_id_match ? 1 : 0;
    impl.is_valid = 1;
    std::copy_n(invitation.application_data.begin(), size, impl.application_data);
}

template <typename Impl>
void FillInvitationGroup(Impl& impl, const Baas::InvitationGroup& group) {
    impl.group_id = group.id;
    impl.sender_id = group.sender_id;
    impl.application_id = group.application_id;
    impl.application_group_id = group.application_group_id;
    const std::size_t size = (std::min)(group.application_data.size(), kApplicationDataSize);
    impl.application_data_size = static_cast<std::uint32_t>(size);
    impl.created_at = group.created_at;
    impl.application_id_match = group.application_id_match ? 1 : 0;
    impl.is_valid = 1;

    // At most sixteen receivers, as the module's parser keeps.
    const std::size_t receivers = (std::min)(group.receivers.size(), kMaxReceivers);
    std::copy_n(group.receivers.begin(), receivers, impl.receivers);
    impl.receiver_count = static_cast<std::int32_t>(receivers);

    // The sixteen 0xC0 message slots, in the module's language order.
    for (std::size_t index = 0; index < kMessageSlots && index < group.messages.size(); ++index) {
        FixedUtf8(Field(impl.messages[index]), group.messages[index]);
    }

    std::copy_n(group.application_data.begin(), size, impl.application_data);
}

void AppendJsonString(std::string& out, std::string_view value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    out += '"';
    for (const char c : value) {
        const auto byte = static_cast<unsigned char>(c);
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (byte < 0x20) {
                out += "\\u00";
                out += kHex[byte >> 4];
                out += kHex[byte & 0xF];
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

/// Collects the top-level string members of a JSON object in document order, duplicates
/// included, as JsonDocument.EnumerateObject yields them. Anything nested is walked over and
/// dropped. The depth limit is JsonDocument's default, 64.
class TopLevelStrings : public nlohmann::json::json_sax_t {
public:
    std::vector<std::pair<std::string, std::string>> pairs;

    bool null() override { return Scalar(); }
    bool boolean(bool) override { return Scalar(); }
    bool number_integer(number_integer_t) override { return Scalar(); }
    bool number_unsigned(number_unsigned_t) override { return Scalar(); }
    bool number_float(number_float_t, const string_t&) override { return Scalar(); }
    bool binary(binary_t&) override { return Scalar(); }
    bool string(string_t& value) override {
        if (depth_ == 1 && in_object_top_) {
            pairs.emplace_back(key_, value);
        }
        return true;
    }
    bool start_object(std::size_t) override { return Open(true); }
    bool end_object() override { return Close(); }
    bool start_array(std::size_t) override { return Open(false); }
    bool end_array() override { return Close(); }
    bool key(string_t& value) override {
        if (depth_ == 1) {
            key_ = value;
        }
        return true;
    }
    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override {
        return false;
    }

private:
    bool Scalar() { return true; }
    bool Open(bool object) {
        if (depth_ == 0) {
            in_object_top_ = object;
        }
        return ++depth_ <= 64;
    }
    bool Close() {
        --depth_;
        return true;
    }

    int depth_ = 0;
    bool in_object_top_ = false;
    std::string key_;
};

} // namespace

// ------------------------------------------------------------------------------------------------
// Text helpers
// ------------------------------------------------------------------------------------------------

std::size_t Utf8Fits(std::string_view encoded, std::size_t limit) {
    std::size_t length = (std::min)(encoded.size(), limit);
    while (length > 0 && length < encoded.size() &&
           (static_cast<unsigned char>(encoded[length]) & 0xC0) == 0x80) {
        --length;
    }
    return length;
}

void FixedUtf8(std::span<char> destination, std::string_view value) {
    std::fill(destination.begin(), destination.end(), '\0');
    if (value.empty() || destination.size() < 2) {
        return;
    }
    const std::size_t length = Utf8Fits(value, destination.size() - 1);
    std::copy_n(value.data(), length, destination.data());
}

Nickname ToNickname(std::string_view name) {
    Nickname nickname{};
    std::copy_n(name.data(), Utf8Fits(name, 32), nickname.name);
    return nickname;
}

std::string Text(std::span<const char> field) {
    const auto end = std::find(field.begin(), field.end(), '\0');
    return Utf8Lossy(std::string_view(field.data(), static_cast<std::size_t>(end - field.begin())));
}

std::string Text(std::span<const std::uint8_t> field) {
    return Text(std::span<const char>(reinterpret_cast<const char*>(field.data()), field.size()));
}

std::pair<std::string, std::string> ScreenName(const InAppScreenName& screen_name) {
    // The language is ASCII: a byte past 0x7F reads as '?', as the .NET ASCII decoder has it.
    std::string language;
    for (const char c : screen_name.language_code) {
        if (c == '\0') {
            break;
        }
        language += static_cast<unsigned char>(c) < 0x80 ? c : '?';
    }
    return {Text(std::span<const char>(screen_name.name)), language};
}

bool TryCatalogId(std::string_view catalog_id, std::uint64_t& high, std::uint64_t& low) {
    high = 0;
    low = 0;
    return catalog_id.size() == 32 && TryHexText(catalog_id.substr(0, 16), high) &&
           TryHexText(catalog_id.substr(16, 16), low);
}

std::string JsonString(std::string_view value) {
    std::string out;
    AppendJsonString(out, value);
    return out;
}

// ------------------------------------------------------------------------------------------------
// The appField <-> blob conversion
// ------------------------------------------------------------------------------------------------

bool ValidAppFieldKey(std::string_view key) {
    if (key.empty()) {
        return false;
    }
    return std::all_of(key.begin(), key.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_';
    });
}

std::string BlobToAppField(std::span<const std::uint8_t> blob) {
    std::string_view rest(reinterpret_cast<const char*>(blob.data()), blob.size());
    std::vector<std::pair<std::string_view, std::string_view>> pairs;

    while (!rest.empty() && rest.front() != '\0') {
        const std::size_t key_end = rest.find('\0');
        if (key_end == std::string_view::npos) {
            return "{}";
        }
        const std::string_view key = rest.substr(0, key_end);
        rest.remove_prefix(key_end + 1);

        const std::size_t value_end = rest.find('\0');
        if (value_end == std::string_view::npos) {
            return "{}";
        }
        const std::string_view value = rest.substr(0, value_end);
        rest.remove_prefix(value_end + 1);

        // The key is read as ASCII, so a byte past 0x7F is already not a letter or digit.
        if (!ValidUtf8(value) || !ValidAppFieldKey(key) ||
            std::any_of(pairs.begin(), pairs.end(), [&](const auto& pair) { return pair.first == key; })) {
            return "{}";
        }
        pairs.emplace_back(key, value);
    }

    std::string out = "{";
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        if (i != 0) {
            out += ',';
        }
        AppendJsonString(out, pairs[i].first);
        out += ':';
        AppendJsonString(out, pairs[i].second);
    }
    out += '}';
    return out;
}

void AppFieldToBlob(std::string_view app_field, std::span<std::uint8_t> blob) {
    std::fill(blob.begin(), blob.end(), std::uint8_t{0});

    if (app_field.empty() || app_field.front() != '{' || app_field.back() != '}') {
        return;
    }

    // Parsed whole before anything is written: text that does not parse leaves the blob zero.
    TopLevelStrings collected;
    if (!nlohmann::json::sax_parse(app_field.begin(), app_field.end(), &collected)) {
        return;
    }

    std::size_t offset = 0;
    for (const auto& [key, value] : collected.pairs) {
        if (!ValidAppFieldKey(key)) {
            continue;
        }
        if (offset + key.size() + value.size() + 2 > blob.size()) {
            break;
        }
        std::copy(key.begin(), key.end(), blob.begin() + static_cast<std::ptrdiff_t>(offset));
        offset += key.size() + 1;
        std::copy(value.begin(), value.end(), blob.begin() + static_cast<std::ptrdiff_t>(offset));
        offset += value.size() + 1;
    }
}

// ------------------------------------------------------------------------------------------------
// Friends
// ------------------------------------------------------------------------------------------------

bool Played(const Baas::Friend& friend_, std::uint64_t presence_group_id) {
    if (presence_group_id == 0) {
        return false;
    }
    return std::any_of(friend_.play_log.begin(), friend_.play_log.end(), [&](const Baas::PlayLog& entry) {
        return entry.presence_group_id == presence_group_id;
    });
}

bool Matches(const Baas::Friend& friend_, const SizedFriendFilter& filter,
             std::uint64_t own_presence_group_id) {
    switch (filter.presence_status) {
    case PresenceStatusFilter::Online:
        if (friend_.state != 1) {
            return false;
        }
        break;
    case PresenceStatusFilter::OnlinePlay:
        if (friend_.state != 2) {
            return false;
        }
        break;
    case PresenceStatusFilter::OnlineOrOnlinePlay:
        if (friend_.state == 0) {
            return false;
        }
        break;
    default:
        break;
    }

    if (filter.is_favorite_only && !friend_.is_favorite) {
        return false;
    }

    const bool in_own_group =
        friend_.application_id != 0 && friend_.presence_group_id == own_presence_group_id;

    if (filter.is_same_app_presence_only && !in_own_group) {
        return false;
    }
    if (filter.is_same_app_played_only && !in_own_group && !Played(friend_, own_presence_group_id)) {
        return false;
    }
    if (filter.is_arbitrary_app_played_only &&
        !(friend_.application_id != 0 && friend_.presence_group_id == filter.presence_group_id) &&
        !Played(friend_, filter.presence_group_id)) {
        return false;
    }
    return true;
}

std::vector<Baas::Friend> Filtered(const std::vector<Baas::Friend>& friends,
                                   const SizedFriendFilter& filter, int offset,
                                   std::uint64_t own_presence_group_id) {
    std::vector<Baas::Friend> matched;
    for (const Baas::Friend& friend_ : friends) {
        if (Matches(friend_, filter, own_presence_group_id)) {
            matched.push_back(friend_);
        }
    }
    if (offset > 0) {
        matched.erase(matched.begin(),
                      matched.begin() + (std::min)(static_cast<std::size_t>(offset), matched.size()));
    }
    return matched;
}

FriendImpl ToFriendImpl(const Baas::Friend& friend_, const Uid& user_id,
                        std::uint64_t own_presence_group_id, bool viewer) {
    const bool playing = friend_.state != 0;
    const bool known = playing || friend_.updated_at > 0;

    const std::uint64_t application_id = known ? friend_.application_id : 0;
    const std::uint64_t presence_group_id = known ? friend_.presence_group_id : 0;
    const bool same_group = application_id != 0 && presence_group_id == own_presence_group_id;

    FriendImpl impl{};
    impl.user_id = user_id;
    impl.network_user_id = friend_.id;
    impl.nickname = ToNickname(friend_.nickname);
    impl.presence.application_id = application_id;
    impl.presence.presence_group_id = presence_group_id;
    impl.presence.last_update_timestamp = friend_.updated_at;
    impl.presence.status = static_cast<PresenceStatus>(std::clamp(friend_.state, 0, 2));
    impl.presence.same_presence_group_application = same_group ? 1 : 0;
    impl.is_favourite = friend_.is_favorite ? 1 : 0;
    impl.is_new = friend_.is_newly ? 1 : 0;
    impl.is_valid = 1;

    if (playing && (same_group || viewer)) {
        AppFieldToBlob(friend_.app_field, impl.presence.app_key_value_storage);
    }
    return impl;
}

namespace {

/// The cached presence block from what 10101 would answer a viewer: offline keeps only the title
/// and the time, and nothing at all without a time.
ViewerPresenceImpl ToViewerPresence(const Baas::Friend& friend_, const FriendPresenceImpl& basis) {
    ViewerPresenceImpl presence{};
    const auto state = static_cast<std::uint8_t>(basis.status);
    if (state == 0 && basis.last_update_timestamp <= 0) {
        return presence;
    }
    presence.application_id = basis.application_id;
    presence.acd_index = friend_.acd_index;
    presence.presence_group_id = basis.presence_group_id;
    presence.updated_at = basis.last_update_timestamp;
    if (state == 0) {
        return presence;
    }
    presence.state = state;
    std::memcpy(presence.app_field, basis.app_key_value_storage, sizeof(presence.app_field));
    return presence;
}

template <typename Record>
Record ToViewerRecord(const Baas::Friend& friend_, const Uid& user_id,
                      std::uint64_t own_presence_group_id) {
    const FriendImpl basis = ToFriendImpl(friend_, user_id, own_presence_group_id, true);

    Record record{};
    record.user_id = user_id;
    record.network_user_id = friend_.id;
    record.nickname = basis.nickname;
    FixedUtf8(Field(record.thumbnail_url), friend_.thumbnail_url);
    record.presence = ToViewerPresence(friend_, basis.presence);
    record.is_favourite = friend_.is_favorite ? 1 : 0;
    record.is_new = friend_.is_newly ? 1 : 0;
    record.is_valid = 1;
    return record;
}

} // namespace

FriendForViewerImpl ToFriendForViewerImpl(const Baas::Friend& friend_, const Uid& user_id,
                                          std::uint64_t own_presence_group_id) {
    return ToViewerRecord<FriendForViewerImpl>(friend_, user_id, own_presence_group_id);
}

FriendForViewerImplV2 ToFriendForViewerImplV2(const Baas::Friend& friend_, const Uid& user_id,
                                              std::uint64_t own_presence_group_id) {
    auto record = ToViewerRecord<FriendForViewerImplV2>(friend_, user_id, own_presence_group_id);
    FixedUtf8(Field(record.friend_note), friend_.note);
    return record;
}

namespace {

/// MEASUREMENT (remove once the entry layout is known): when <data dir>/playlog-probe exists, each of
/// its lines is one row of the friend page's Play Activity, built from `offset:kind` tokens instead of
/// from the friend's real play log. Offsets are in the 0x28-byte entry (hex); kinds: first (i64, two
/// days ago), last (i64, one day ago), time (u32, 600), count (u32, 5), id (u64, the title again).
/// Rows cycle through four games the viewer can draw, so the subtitle is the whole result:
/// "First played 2 days ago" names the first-played field, "Played for 8 hours or more" the time.
bool ProbePlayLog(std::uint8_t* block, std::size_t& count) {
    std::ifstream file(Platform::DataDir() / "playlog-probe");
    if (!file) {
        return false;
    }
    const std::int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
    // Four games the viewer can draw a tile for (a repeated title makes the page fail some rows):
    // Splatoon 3, Animal Crossing, Don't Starve Together, Diablo II. Row r is game r % 4.
    constexpr std::uint64_t Titles[4] = {0x0100c2500fc20000ull, 0x01006f8002326000ull,
                                         0x010090100e334000ull, 0x0100726014352000ull};
    std::string line;
    std::size_t row = 0;
    while (row < kPlayLogEntries && std::getline(file, line)) {
        std::uint8_t entry[0x28] = {};
        const std::uint64_t Title = Titles[row % 4];
        std::memcpy(entry + 0x00, &Title, 8);
        std::memcpy(entry + 0x08, &Title, 8);
        std::istringstream tokens(line);
        std::string token;
        bool any = false;
        while (tokens >> token) {
            const auto colon = token.find(':');
            if (colon == std::string::npos) {
                continue;
            }
            const std::size_t offset = std::stoul(token.substr(0, colon), nullptr, 16);
            const std::string kind = token.substr(colon + 1);
            if (offset > 0x20) {
                continue;
            }
            if (kind == "first" || kind == "last" || kind == "id") {
                const std::int64_t value = kind == "first" ? now - 2 * 86400
                                           : kind == "last" ? now - 86400
                                                            : static_cast<std::int64_t>(Title);
                std::memcpy(entry + offset, &value, 8);
            } else if (offset <= 0x24) {
                const std::uint32_t value = kind == "time" ? 600 : kind == "count" ? 5 : 0;
                std::memcpy(entry + offset, &value, 4);
            }
            any = true;
        }
        if (any) {
            std::memcpy(block + row * sizeof(entry), entry, sizeof(entry));
            ++row;
        }
    }
    count = row;
    return row > 0;
}

} // namespace

FriendDetailedInfoImpl ToDetailedInfoImpl(const Baas::Friend& friend_, const Uid& user_id) {
    FriendDetailedInfoImpl impl{};
    impl.user_id = user_id;
    impl.network_user_id = friend_.id;
    impl.nickname = ToNickname(friend_.nickname);
    impl.channel = static_cast<std::uint32_t>(friend_.channel);
    impl.is_valid = 1;
    FixedUtf8(Field(impl.thumbnail_url), friend_.thumbnail_url);
    // "When You Became Friends": `createdAt` of the list entry (0 reads as 31/12/1999 00:00).
    impl.friends_since = friend_.created_at;
    // "Play Activity": `extras.<group>.playLog`, up to 20 titles. The valid flag stays clear with none,
    // which is how the module says there is nothing to list.
    //
    // The entry (0x28): title id, presence group id, then one 8-byte field holding the play count (low
    // 32 bits) and the play time (high 32 bits), then first played and last played (i64). Measured on
    // 2026-10-15 against a real Switch's page for the same friend: the other orders and the swapped
    // count/time packing gave different subtitles ("Played for a little while" where the console says
    // "First played 3 days ago").
    std::size_t count = std::min(friend_.play_log.size(), kPlayLogEntries);
    if (ProbePlayLog(impl.play_log, count)) {
        impl.is_play_log_valid = 1;
        return impl;
    }
    for (std::size_t i = 0; i < count; ++i) {
        const Baas::PlayLog& log = friend_.play_log[i];
        const std::uint64_t counts = static_cast<std::uint32_t>(std::clamp<std::int64_t>(log.total_play_count, 0, 0xFFFFFFFF));
        const std::uint64_t played = static_cast<std::uint32_t>(std::clamp<std::int64_t>(log.total_play_time, 0, 0xFFFFFFFF));
        const std::uint64_t entry[5] = {log.application_id, log.presence_group_id, counts | (played << 32),
                                        static_cast<std::uint64_t>(log.first_played_at),
                                        static_cast<std::uint64_t>(log.last_played_at)};
        std::memcpy(impl.play_log + i * sizeof(entry), entry, sizeof(entry));
    }
    impl.is_play_log_valid = count > 0 ? 1 : 0;
    return impl;
}

FriendSettingImpl ToFriendSettingImpl(const Baas::Friend& friend_, const Uid& user_id) {
    FriendSettingImpl impl{};
    impl.user_id = user_id;
    impl.network_user_id = friend_.id;
    impl.is_favourite = friend_.is_favorite ? 1 : 0;
    impl.is_new = friend_.is_newly ? 1 : 0;
    impl.is_online_notification = friend_.is_online_notification ? 1 : 0;
    return impl;
}

FriendSettingImplV2 ToFriendSettingImplV2(const Baas::Friend& friend_, const Uid& user_id) {
    FriendSettingImplV2 impl{};
    impl.user_id = user_id;
    impl.network_user_id = friend_.id;
    impl.is_favourite = friend_.is_favorite ? 1 : 0;
    impl.is_new = friend_.is_newly ? 1 : 0;
    impl.is_online_notification = friend_.is_online_notification ? 1 : 0;
    FixedUtf8(Field(impl.friend_note), friend_.note);
    return impl;
}

// ------------------------------------------------------------------------------------------------
// Users, blocks, own setting
// ------------------------------------------------------------------------------------------------

ProfileImpl ToProfileImpl(const Baas::User& user) {
    ProfileImpl impl{};
    FillProfile(impl, user);
    return impl;
}

ProfileExtraImpl ToProfileExtraImpl(const Baas::User& user) {
    ProfileExtraImpl impl{};
    FillProfile(impl, user);
    return impl;
}

ProfileExtraImplV2 ToProfileExtraImplV2(const Baas::User& user) {
    ProfileExtraImplV2 impl{};
    FillProfile(impl, user);
    return impl;
}

BlockedUserImpl ToBlockedUserImpl(const Baas::Block& block, const Uid& user_id) {
    BlockedUserImpl impl{};
    FillBlock(impl, block, user_id);
    return impl;
}

BlockedUserImplV2 ToBlockedUserImplV2(const Baas::Block& block, const Uid& user_id) {
    BlockedUserImplV2 impl{};
    FillBlock(impl, block, user_id);
    impl.acd_index = block.route.acd_index;
    return impl;
}

UserSettingImpl ToUserSettingImpl(const Baas::UserSetting& setting, const Uid& user_id) {
    UserSettingImpl impl{};
    FillUserSetting(impl, setting, user_id);
    return impl;
}

UserSettingImplV2 ToUserSettingImplV2(const Baas::UserSetting& setting, const Uid& user_id) {
    UserSettingImplV2 impl{};
    FillUserSetting(impl, setting, user_id);
    return impl;
}

// ------------------------------------------------------------------------------------------------
// Requests
// ------------------------------------------------------------------------------------------------

FriendRequestImpl ToRequestImpl(const Baas::Request& request, const Uid& user_id, int list_type) {
    FriendRequestImpl impl{};
    std::memset(&impl, 0, sizeof(impl)); // the whole union, not only its first member
    FillRequest(impl, request, user_id, list_type);
    return impl;
}

FriendRequestImplV2 ToRequestImplV2(const Baas::Request& request, const Uid& user_id,
                                    int list_type) {
    FriendRequestImplV2 impl{};
    std::memset(&impl, 0, sizeof(impl));
    FillRequest(impl, request, user_id, list_type);
    impl.route_acd_index = request.route.acd_index;
    return impl;
}

// ------------------------------------------------------------------------------------------------
// Invitations
// ------------------------------------------------------------------------------------------------

FriendInvitationForViewerImpl ToInvitationImpl(const Baas::Invitation& invitation) {
    FriendInvitationForViewerImpl impl{};
    FillInvitation(impl, invitation);
    return impl;
}

FriendInvitationForViewerImplV2 ToInvitationImplV2(const Baas::Invitation& invitation) {
    FriendInvitationForViewerImplV2 impl{};
    FillInvitation(impl, invitation);
    impl.acd_index = invitation.acd_index;
    return impl;
}

FriendInvitationGroupImpl ToInvitationGroupImpl(const Baas::InvitationGroup& group) {
    FriendInvitationGroupImpl impl{};
    FillInvitationGroup(impl, group);
    return impl;
}

FriendInvitationGroupImplV2 ToInvitationGroupImplV2(const Baas::InvitationGroup& group) {
    FriendInvitationGroupImplV2 impl{};
    FillInvitationGroup(impl, group);
    impl.acd_index = group.acd_index;
    return impl;
}

std::vector<std::pair<std::string, std::string>> InvitationMessages(
    const FriendInvitationGameModeDescription& description) {
    std::vector<std::pair<std::string, std::string>> messages;
    for (std::size_t index = 0; index < kMessageSlots; ++index) {
        std::string text = Text(std::span<const char>(description.messages[index]));
        if (!text.empty()) {
            messages.emplace_back(std::string(kMessageLanguages[index]), std::move(text));
        }
    }
    return messages;
}

// ------------------------------------------------------------------------------------------------
// Own presence
// ------------------------------------------------------------------------------------------------

UserPresenceViewImpl ToUserPresenceViewImpl(std::uint64_t application_id,
                                            std::uint64_t presence_group_id,
                                            std::int64_t updated_at, bool session_open,
                                            std::string_view app_field) {
    UserPresenceViewImpl view{};
    view.application_id = application_id;
    view.presence_group_id = presence_group_id;
    view.last_update_timestamp = updated_at;
    view.state = application_id == 0 ? 0u : session_open ? 2u : 1u;
    if (application_id != 0) {
        AppFieldToBlob(app_field, view.app_key_value_storage);
    }
    return view;
}

} // namespace OpenPak::Friends

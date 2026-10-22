// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later
// Derived from openpak-client 3fd9ba7 (baas_parsing.cpp).

// The friends module's parsers, as far as they decide what a reply means: which fields are
// required, what types they must have, and what a missing one does (friends contract §1.3).
// Unknown keys are ignored everywhere; a field of the wrong type counts as absent. Ported from
// Ryujinx.OpenPak/OpenPakBaas.Parsing.cs.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/logging.h"
#include "core/openpak/baas.h"

namespace OpenPak::Baas {

const std::array<const char*, 10> Channels{
    "NX_FACED", "FRIEND_CODE", "IN_APP", "NINTENDO_ACCOUNT", "3DS",
    "NNID",     "FACEBOOK",    "TWITTER", "WECHAT",          "CAMPUS",
};

const std::array<const char*, 16> MessageLanguages{
    "en-US", "en-GB", "ja", "fr", "de",      "es-419",  "es", "it",
    "nl",    "fr-CA", "pt", "ru", "zh-Hans", "zh-Hant", "ko", "pt-BR",
};

namespace {

constexpr std::array<const char*, 5> RequestStates{"PENDING", "CANCELED", "AUTHORIZED",
                                                   "REJECTED", "EXPIRED"};

/// What a missing child reads as: nothing, which is neither an object nor an array.
const Json& Absent() {
    static const Json absent;
    return absent;
}

const Json& Child(const Json& parent, const char* name) {
    if (!parent.is_object()) {
        return Absent();
    }
    const auto it = parent.find(name);
    return it != parent.end() ? *it : Absent();
}

const Json& Path(const Json& element, std::initializer_list<const char*> names) {
    const Json* at = &element;
    for (const char* name : names) {
        at = &Child(*at, name);
    }
    return *at;
}

bool Hex(const Json& parent, const char* name, std::uint64_t& value) {
    value = 0;
    return parent.is_object() && TryHex(Child(parent, name), value);
}

std::optional<std::string> Text(const Json& parent, const char* name) {
    const Json& element = Child(parent, name);
    if (!element.is_string()) {
        return std::nullopt;
    }
    return element.get<std::string>();
}

/// A JSON integer that fits an int64; a fraction, an exponent or a larger number is absent.
std::optional<std::int64_t> Integer(const Json& parent, const char* name) {
    const Json& element = Child(parent, name);
    if (element.is_number_unsigned()) {
        const auto value = element.get<std::uint64_t>();
        if (value > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(value);
    }
    if (element.is_number_integer()) {
        return element.get<std::int64_t>();
    }
    return std::nullopt;
}

/// A JSON integer that fits a uint64: a negative one is absent.
std::optional<std::uint64_t> Unsigned(const Json& parent, const char* name) {
    const Json& element = Child(parent, name);
    if (!element.is_number_unsigned()) {
        return std::nullopt;
    }
    return element.get<std::uint64_t>();
}

std::optional<bool> Flag(const Json& parent, const char* name) {
    const Json& element = Child(parent, name);
    if (!element.is_boolean()) {
        return std::nullopt;
    }
    return element.get<bool>();
}

/// The items array, or nothing to walk.
const Json& Items(const Json& root) {
    static const Json empty = Json::array();
    const Json& items = Child(root, "items");
    return items.is_array() ? items : empty;
}

bool Bracketed(const std::string& text, char open, char close) {
    return !text.empty() && text.front() == open && text.back() == close;
}

/// The route keys. `shared` carries appInfo and the catalog id, `named` the in-app name and the
/// NNID Mii data (§A.4.2: the inbox reads both from senderAndReceiver, the outbox the names from
/// sender). A route with nothing in it is the default one, as the earlier port's null is.
Route ParseRoute(const Json& shared, const Json& named) {
    Route route;
    Hex(shared, "route:appInfo:appId", route.application_id);
    Hex(shared, "route:appInfo:presenceGroupId", route.presence_group_id);
    route.acd_index = static_cast<std::uint8_t>(Integer(shared, "route:appInfo:acdIndex").value_or(0));
    route.catalog_id = Text(shared, "route:candidate:catalogId").value_or("");
    route.name = Text(named, "route:name").value_or("");
    route.language = Text(named, "route:name:language").value_or("");
    route.mii_name = Text(named, "route:nnid:miiName").value_or("");
    route.mii_image_url_param = Text(named, "route:nnid:miiImageUrlParam").value_or("");
    return route;
}

/// The first `extras.<group>.playLog` that is a "[...]" string, and which group held it. The
/// document's own order decides, which is why Json keeps it.
std::pair<std::string, std::string> PlayLogTextOf(const Json& extras) {
    if (!extras.is_object()) {
        return {};
    }

    for (const auto& [group, value] : extras.items()) {
        const std::optional<std::string> text = Text(value, "playLog");
        if (text && Bracketed(*text, '[', ']')) {
            return {*text, group};
        }
    }

    return {};
}

std::vector<PlayLog> PlayLogOf(const Json& extras) {
    return ParsePlayLog(PlayLogTextOf(extras).first);
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

/// Base64 as Convert.FromBase64String takes it: whitespace is ignored, the rest must be whole
/// quads of the standard alphabet with at most two '=' at the very end.
std::optional<std::vector<std::uint8_t>> Base64Strict(std::string_view text) {
    std::string clean;
    clean.reserve(text.size());
    for (const char c : text) {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            clean.push_back(c);
        }
    }

    if (clean.size() % 4 != 0) {
        return std::nullopt;
    }

    std::size_t padding = 0;
    while (padding < clean.size() && padding < 2 && clean[clean.size() - 1 - padding] == '=') {
        padding++;
    }

    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };

    std::vector<std::uint8_t> out;
    std::uint32_t accumulator = 0;
    int bits = 0;

    for (std::size_t i = 0; i < clean.size() - padding; i++) {
        const int digit = value(clean[i]);
        if (digit < 0) {
            return std::nullopt;
        }
        accumulator = (accumulator << 6) | static_cast<std::uint32_t>(digit);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((accumulator >> bits) & 0xFF));
        }
    }

    return out;
}

/// application_data: base64, or null for none. The key itself is required; false here when it is
/// absent or neither form.
bool ApplicationData(const Json& parent, std::vector<std::uint8_t>& data) {
    data.clear();

    const auto it = parent.find("application_data");
    if (it == parent.end()) {
        return false;
    }

    if (it->is_null()) {
        return true;
    }

    if (!it->is_string()) {
        return false;
    }

    auto decoded = Base64Strict(it->get_ref<const std::string&>());
    if (!decoded) {
        return false;
    }

    data = std::move(*decoded);
    if (data.size() > 0x400) {
        data.resize(0x400);
    }

    return true;
}

} // namespace

int ChannelOf(std::string_view channel) {
    for (std::size_t i = 0; i < Channels.size(); i++) {
        if (channel == Channels[i]) {
            return static_cast<int>(i) + 1;
        }
    }
    return 0;
}

bool TryHex(const Json& element, std::uint64_t& value) {
    value = 0;
    return element.is_string() && TryHexText(element.get_ref<const std::string&>(), value);
}

bool TryHexText(std::string_view text, std::uint64_t& value) {
    value = 0;

    while (!text.empty() && text.front() == ' ') {
        text.remove_prefix(1);
    }

    if (text.starts_with('+')) {
        text.remove_prefix(1);
    }

    if (text.starts_with("0x") || text.starts_with("0X")) {
        text.remove_prefix(2);
    }

    std::size_t digits = 0;
    while (digits < text.size() && std::isxdigit(static_cast<unsigned char>(text[digits]))) {
        digits++;
    }

    if (digits == 0 || digits > 16) {
        return false;
    }

    std::uint64_t parsed = 0;
    for (std::size_t i = 0; i < digits; i++) {
        const char c = text[i];
        const int digit = c <= '9' ? c - '0' : (std::tolower(static_cast<unsigned char>(c)) - 'a' + 10);
        parsed = (parsed << 4) | static_cast<std::uint64_t>(digit);
    }

    value = parsed;
    return true;
}

std::optional<Friend> ParseFriend(const Json& item) {
    const Json& friend_ = Child(item, "friend");

    const std::optional<std::string> nickname = Text(friend_, "nickname");
    const std::optional<std::string> thumbnail = Text(friend_, "thumbnailUrl");

    std::uint64_t id = 0;
    if (!Hex(item, "friendId", id) || !nickname || !thumbnail) {
        return std::nullopt;
    }

    const Json& presence = Child(friend_, "presence");
    const Json& app_info = Path(presence, {"extras", "friends"});
    const Json& self = Path(item, {"extras", "self"});

    Friend out;
    out.id = id;
    out.nickname = *nickname;
    out.thumbnail_url = *thumbnail;

    const std::string state = Text(presence, "state").value_or("");
    out.state = state == "ONLINE" ? 1 : state == "PLAYING" ? 2 : 0;

    out.updated_at = Integer(presence, "updatedAt").value_or(0);
    if (out.state == 0) {
        if (const auto logout_at = Integer(presence, "logoutAt")) {
            out.updated_at = *logout_at;
        }
    }

    Hex(app_info, "appInfo:appId", out.application_id);
    Hex(app_info, "appInfo:presenceGroupId", out.presence_group_id);
    out.acd_index = static_cast<std::uint8_t>(Integer(app_info, "appInfo:acdIndex").value_or(0));

    const std::optional<std::string> app_field = Text(app_info, "appField");
    if (app_field && Bracketed(*app_field, '{', '}')) {
        out.app_field = *app_field;
    }

    const Json& channels = Child(item, "channels");
    if (channels.is_array()) {
        for (const Json& value : channels) {
            if (value.is_string()) {
                out.channel = ChannelOf(value.get_ref<const std::string&>());
                break;
            }
        }
    }

    out.is_favorite = Flag(item, "isFavorite").value_or(false);
    out.is_newly = Flag(self, "isConfirmed") != true;
    out.is_online_notification = Flag(self, "isOnlineNotification").value_or(false);
    out.created_at = Integer(item, "createdAt").value_or(0);
    out.note = Text(item, "friendNote").value_or("");
    out.route = ParseRoute(self, self);
    out.play_log = PlayLogOf(Child(friend_, "extras"));

    return out;
}

std::vector<Friend> ParseFriendList(const Json& root) {
    std::vector<Friend> friends;

    for (const Json& item : Items(root)) {
        if (friends.size() == 300) {
            break;
        }

        if (auto parsed = ParseFriend(item)) {
            friends.push_back(std::move(*parsed));
        }
    }

    return friends;
}

std::vector<PlayLog> ParsePlayLog(std::string_view text) {
    if (text.empty() || text.front() != '[' || text.back() != ']') {
        return {};
    }

    const Json document = Json::parse(text, nullptr, false);
    if (document.is_discarded() || !document.is_array()) {
        return {};
    }

    std::vector<PlayLog> entries;

    for (const Json& entry : document) {
        if (entries.size() == 20) {
            break;
        }

        std::uint64_t application_id = 0;
        std::uint64_t group_id = 0;
        const auto count = Integer(entry, "totalPlayCount");
        const auto time = Integer(entry, "totalPlayTime");
        const auto first = Integer(entry, "firstPlayedAt");
        const auto last = Integer(entry, "lastPlayedAt");

        if (!Hex(entry, "appInfo:appId", application_id) ||
            !Hex(entry, "appInfo:presenceGroupId", group_id) || !count || !time || !first ||
            !last) {
            continue;
        }

        const auto existing =
            std::find_if(entries.begin(), entries.end(), [&](const PlayLog& known) {
                return known.application_id == application_id;
            });

        if (existing == entries.end()) {
            entries.push_back({
                application_id,
                static_cast<std::uint8_t>(Integer(entry, "appInfo:acdIndex").value_or(0)),
                group_id,
                *count,
                *time,
                *first,
                *last,
            });
            continue;
        }

        existing->total_play_count = (std::max)(existing->total_play_count, *count);
        existing->total_play_time = (std::max)(existing->total_play_time, *time);
        existing->first_played_at = (std::max)(existing->first_played_at, *first);
        existing->last_played_at = (std::max)(existing->last_played_at, *last);
    }

    return entries;
}

std::optional<Request> ParseRequest(const Json& item, bool inbox) {
    const std::string side = inbox ? "sender" : "receiver";
    const Json& other = Child(item, side.c_str());

    std::optional<std::string> channel;
    const Json& channels = Child(item, "channels");
    if (channels.is_array() && !channels.empty() && channels[0].is_string()) {
        channel = channels[0].get<std::string>();
    }

    const std::optional<std::string> state = Text(item, "state");
    const std::optional<std::string> nickname = Text(other, "nickname");
    const std::optional<std::string> thumbnail = Text(other, "thumbnailUrl");

    std::uint64_t id = 0;
    std::uint64_t other_id = 0;
    const char* missing = !Hex(item, "id", id)                             ? "id"
                          : !channel                                       ? "channels[0]"
                          : !state                                         ? "state"
                          : !Hex(item, (side + "Id").c_str(), other_id)    ? "the other id"
                          : !nickname || nickname->size() >= 0x21          ? "nickname"
                          : !thumbnail || thumbnail->size() >= 0xA0        ? "thumbnailUrl"
                                                                           : nullptr;
    if (missing != nullptr) {
        // Field names only: the values are a person's.
        LOG_DEBUG(WebService, "[OpenPak] A friend request item was dropped: {} {} is missing or "
                              "too long", side, missing);
        return std::nullopt;
    }

    const Json& extras = Child(item, "extras");
    const Json& shared = Child(extras, "senderAndReceiver");

    Request out;
    out.id = id;
    out.channel = ChannelOf(*channel);
    for (std::size_t i = 0; i < RequestStates.size(); i++) {
        if (EqualsIgnoreCase(*state, RequestStates[i])) {
            out.state = static_cast<int>(i) + 1;
            break;
        }
    }
    out.other_id = other_id;
    out.nickname = *nickname;
    out.thumbnail_url = *thumbnail;
    out.created_at = std::max<std::int64_t>(Integer(item, "createdAt").value_or(0), 0);
    out.read = inbox && Flag(Child(extras, "receiver"), "read") == true;
    out.route = ParseRoute(shared, inbox ? shared : Child(extras, "sender"));

    return out;
}

std::vector<Request> ParseRequestList(const Json& root, bool inbox) {
    std::vector<Request> requests;

    for (const Json& item : Items(root)) {
        if (auto request = ParseRequest(item, inbox)) {
            requests.push_back(std::move(*request));
        }
    }

    return requests;
}

std::pair<int, int> ParseRequestCount(const Json& root) {
    int total = 0;
    int read = 0;

    for (const Json& item : Items(root)) {
        total++;

        if (Flag(Path(item, {"extras", "receiver"}), "read") == true) {
            read++;
        }
    }

    return {total - read, read};
}

std::optional<User> ParseUser(const Json& item) {
    const std::optional<std::string> nickname = Text(item, "nickname");
    const std::optional<std::string> thumbnail = Text(item, "thumbnailUrl");

    std::uint64_t id = 0;
    if (!Hex(item, "id", id) || !nickname || !thumbnail) {
        return std::nullopt;
    }

    return User{id, *nickname, *thumbnail, PlayLogOf(Child(item, "extras"))};
}

std::vector<User> ParseUsers(const Json& root) {
    std::vector<User> users;

    for (const Json& item : Items(root)) {
        if (auto user = ParseUser(item)) {
            users.push_back(std::move(*user));
        }
    }

    return users;
}

std::optional<UserSetting> ParseUserSetting(const Json& root) {
    const std::optional<std::string> nickname = Text(root, "nickname");
    const std::optional<std::string> thumbnail = Text(root, "thumbnailUrl");

    std::uint64_t id = 0;
    if (!Hex(root, "id", id) || !nickname || !thumbnail) {
        return std::nullopt;
    }

    const Json& permissions = Child(root, "permissions");
    const Json& friend_code = Path(root, {"links", "friendCode"});

    const auto [play_log, group] = PlayLogTextOf(Child(root, "extras"));

    UserSetting out;
    out.id = id;
    out.nickname = *nickname;
    out.thumbnail_url = *thumbnail;

    const std::string presence = Text(permissions, "presence").value_or("");
    out.presence_permission = presence == "FAVORITE_FRIENDS" ? 1 : presence == "FRIENDS" ? 2 : 0;

    out.friend_request_reception = Flag(permissions, "friendRequestReception").value_or(false);
    out.friend_code = Text(friend_code, "id").value_or("");
    out.friend_code_regenerable_at = Integer(friend_code, "regenerableAt").value_or(0);

    out.play_log_permission = group == "self"              ? 1
                              : group == "favoriteFriends" ? 2
                              : group == "friends"         ? 3
                              : group == "everyone"        ? 5
                                                           : 0;
    out.play_log_text = play_log;
    out.play_log = ParsePlayLog(play_log);

    return out;
}

std::optional<std::vector<Block>> ParseBlocks(const Json& root) {
    std::vector<Block> blocks;

    for (const Json& item : Items(root)) {
        const Json& target = Child(item, "targetUser");
        const std::optional<std::string> nickname = Text(target, "nickname");

        std::uint64_t id = 0;
        if (blocks.size() == 100 || !Hex(item, "targetUserId", id) || !nickname) {
            return std::nullopt;
        }

        const Json& self = Path(item, {"extras", "self"});
        const std::string reason = Text(self, "reason").value_or("");

        Block block;
        block.id = id;
        block.nickname = *nickname;
        block.thumbnail_url = Text(target, "thumbnailUrl").value_or("");
        block.created_at = Integer(item, "createdAt").value_or(0);
        block.reason = reason == "BAD_FRIEND_REQUEST" ? 1
                       : reason == "BAD_FRIEND"       ? 2
                       : reason == "IN_APP"           ? 3
                       : reason == "IN_CAMPUS"        ? 4
                                                      : 0;
        block.route = ParseRoute(self, self);

        blocks.push_back(std::move(block));
    }

    return blocks;
}

std::optional<std::vector<Invitation>> ParseInvitations(const Json& root) {
    std::vector<Invitation> invitations;

    for (const Json& item : Items(root)) {
        const auto id = Unsigned(item, "id");
        const auto group_id = Unsigned(item, "invitation_group_id");

        Invitation invitation;
        if (!id || !group_id || !Hex(item, "sender_id", invitation.sender_id) ||
            !Hex(item, "application_id", invitation.application_id) ||
            !Hex(item, "application_group_id", invitation.application_group_id) ||
            !ApplicationData(item, invitation.application_data)) {
            return std::nullopt;
        }

        const auto read = Flag(item, "read");
        const auto created_at = Integer(item, "created_at");
        if (!read || !created_at || !Integer(item, "updated_at")) {
            return std::nullopt;
        }

        invitation.id = *id;
        invitation.group_id = *group_id;
        invitation.acd_index = static_cast<std::uint8_t>(
            std::clamp<std::int64_t>(Integer(item, "acd_index").value_or(0), 0, 255));
        invitation.created_at = *created_at;
        invitation.read = *read;
        invitation.application_id_match = Flag(item, "application_id_match").value_or(false);

        invitations.push_back(std::move(invitation));
    }

    return invitations;
}

std::optional<InvitationGroup> ParseInvitationGroup(const Json& root) {
    const Json& messages = Child(root, "messages");
    const Json& invitations = Child(root, "invitations");

    InvitationGroup group;
    const auto id = Unsigned(root, "id");
    if (!id || !Hex(root, "sender_id", group.sender_id) ||
        !Hex(root, "application_id", group.application_id) ||
        !Hex(root, "application_group_id", group.application_group_id) ||
        !ApplicationData(root, group.application_data)) {
        return std::nullopt;
    }

    const auto created_at = Integer(root, "created_at");
    if (!created_at || !Integer(root, "updated_at") || !messages.is_object() ||
        !invitations.is_array() || invitations.empty() || invitations.size() > 16) {
        return std::nullopt;
    }

    for (const Json& invitation : invitations) {
        std::uint64_t receiver = 0;
        if (!Hex(invitation, "receiver_id", receiver)) {
            return std::nullopt;
        }
        group.receivers.push_back(receiver);
    }

    group.id = *id;
    group.acd_index = static_cast<std::uint8_t>(
        std::clamp<std::int64_t>(Integer(root, "acd_index").value_or(0), 0, 255));

    for (const char* language : MessageLanguages) {
        const std::optional<std::string> text = Text(messages, language);
        group.messages.push_back(text && text->size() < 0xC0 ? *text : std::string{});
    }

    group.created_at = *created_at;
    group.application_id_match = Flag(root, "application_id_match").value_or(false);

    return group;
}

Relationship ParseRelationship(const Json& root) {
    const Json& sent = Child(root, "sentFriendRequestIds");

    return {
        Flag(root, "isFriend").value_or(false),
        Flag(root, "isBlocking").value_or(false),
        sent.is_array() && !sent.empty(),
    };
}

} // namespace OpenPak::Baas

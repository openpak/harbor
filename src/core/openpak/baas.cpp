// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later
// Derived from openpak-client 3fd9ba7 (baas.cpp).

// The friends module's calls to nx-baas and Five, and the caches they fill. Ported from
// Ryujinx.OpenPak/OpenPakBaas.cs and OpenPakBaas.Caches.cs; the parsers are in
// baas_parsing.cpp and the friend-request calls in baas_requests.cpp.

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "core/openpak/baas.h"
#include "core/openpak/baas_detail.h"
#include "core/openpak/platform_titles.h"
#include "common/logging.h"
#include "core/openpak/session.h"

namespace OpenPak::Baas {

namespace {

using detail::Dump;
using detail::Hex16;
using detail::JsonPatch;
using detail::JsonType;

/// The list sync's cooldown (§2.2, `cooldown_time_for_sync_friend_list`).
constexpr std::chrono::seconds ListCooldown{30};

template <typename T>
using Shared = std::shared_ptr<const std::vector<T>>;

template <typename T>
Shared<T> Empty() {
    return std::make_shared<const std::vector<T>>();
}

/// Everything this module remembers, behind one lock for the lists the guest reads and one for
/// the caches its poll fills, as the earlier port splits them. Lists are replaced whole and never
/// changed in place, so a reader holds its copy of the pointer without holding the lock.
struct State {
    std::mutex mutex;

    Transport test_transport;
    std::string user_id;

    Shared<Friend> friends = Empty<Friend>();
    std::optional<std::string> friends_signature;
    bool friends_synced = false;
    std::chrono::steady_clock::time_point friends_synced_at;

    Shared<Block> blocks = Empty<Block>();
    bool blocks_synced = false;

    std::optional<UserSetting> setting;

    Shared<Request> inbox_requests = Empty<Request>();
    Shared<Request> outbox_requests = Empty<Request>();
    int unread_requests = 0;

    std::function<void()> on_friend_list_changed;
    std::function<void()> on_friend_request_arrived;

    std::mutex cache_mutex;
    Shared<Invitation> invitations = Empty<Invitation>();
    std::map<std::uint64_t, InvitationGroup> groups;
    std::map<std::uint64_t, User> users;
    std::set<std::uint64_t> warming;
};

State& Get() {
    static State state;
    return state;
}

/// Signed out, or another profile: every cache belongs to the last user. Called with the main
/// lock held.
void ForgetLocked(State& state) {
    state.friends = Empty<Friend>();
    state.friends_signature.reset();
    state.friends_synced = false;
    state.friends_synced_at = {};
    state.blocks = Empty<Block>();
    state.blocks_synced = false;
    state.setting.reset();

    // The request boxes are the account's in the earlier port and are dropped at sign-out there;
    // here they live with the rest of the module's caches and go with the user they belong to.
    state.inbox_requests = Empty<Request>();
    state.outbox_requests = Empty<Request>();
    state.unread_requests = 0;

    std::lock_guard cache{state.cache_mutex};
    state.invitations = Empty<Invitation>();
    state.groups.clear();
    state.users.clear();
    state.warming.clear();
}

void RaiseFriendListChanged() {
    State& state = Get();
    std::function<void()> hook;
    {
        std::lock_guard lock{state.mutex};
        hook = state.on_friend_list_changed;
    }
    if (hook) {
        hook();
    }
}

// ---- results (§1.2) ----

int FromStatus(int status) {
    if (status >= 200 && status < 300) return Ok;
    if (status == 302) return 3302;
    if (status >= 300 && status < 400) return 3399;
    if (status == 400) return 3400;
    if (status == 401 || status == 402) return 3499;
    if (status == 403) return 3403;
    if (status == 404) return 3404;
    if (status >= 400 && status < 500) return 3499;
    if (status == 500) return 3500;
    if (status == 501 || status == 502) return 3599;
    if (status == 503) return 3503;
    if (status == 504) return 3504;
    if (status >= 500 && status < 600) return 3599;
    return Unknown;
}

const std::map<std::pair<int, std::string>, int>& BaasErrors() {
    static const std::map<std::pair<int, std::string>, int> errors{
        {{400, "invalid_params"}, 2001},
        {{400, "invalid_request"}, 2002},
        {{400, "invalid_operation"}, 2003},
        {{400, "sender_friend_capacity_is_full"}, 2004},
        {{400, "receiver_friend_capacity_is_full"}, 2005},
        {{401, "invalid_token"}, 2011},
        {{403, "insufficient_scope"}, 2021},
        {{403, "forbidden"}, 2022},
        {{403, "operation_is_not_permitted"}, 2023},
        {{404, "resource_is_not_found"}, 2031},
        {{405, "method_not_allowed"}, 2041},
        {{406, "not_acceptable_language"}, 2051},
        {{409, "resource_already_exists"}, 2061},
        {{409, "resource_duplicated"}, 2062},
        {{400, "receiver_must_be_different_from_sender"}, 5},
        {{400, "target_user_must_be_different_from_user"}, 5},
        {{400, "could_not_retrieve_sender"}, 2212},
        {{400, "could_not_retrieve_receiver"}, 2222},
        {{400, "could_not_retrieve_target_user"}, 2223},
        {{404, "deleted_user"}, 2224},
        {{400, "sender_and_receiver_are_already_friends"}, 2501},
        {{409, "sender_blocks_receiver"}, 2506},
        {{400, "could_not_send_request"}, 2507},
        {{400, "sender_is_in_cool_time"}, 2508},
        {{400, "friend_request_in_cool_time"}, 2509},
        {{409, "user_block_capacity_is_full"}, 2702},
        {{400, "invalid_friend_code_format"}, 2801},
        {{412, "precondition_failed"}, 2201},
        {{422, "friend_code_unregenerable_state"}, 2301},
        {{500, "internal_server_error"}, 2101},
        {{503, "under_maintenance"}, 2111},
    };
    return errors;
}

// ---- transport ----

/// A GET whose reply is parsed: the error, and the document when there is one.
std::pair<int, Json> GetJson(const char* host, const std::string& path, bool five) {
    const Reply reply = detail::Send("GET", host, path, {}, {});

    if (!reply.Success()) {
        return {five ? FiveError(reply) : BaasError(reply), Json{}};
    }

    Json document = Json::parse(reply.body, nullptr, false);
    if (document.is_discarded()) {
        return {Unknown, Json{}};
    }

    return {Ok, std::move(document)};
}

/// A JSON-Patch body, one op per request as the module sends it (§A.2).
std::string Patch(const std::string& op, const std::string& path, const Json& value) {
    Json patch = Json::array();
    patch.push_back(Json{{"op", op}, {"path", path}, {"value", value}});
    return Dump(patch);
}

/// What the module compares field by field: the items as they arrived. Anything different is a
/// changed list, and a list that did not change says nothing to anyone.
std::string Signature(const Json& root) {
    if (!root.is_object()) {
        return {};
    }
    const auto items = root.find("items");
    return items != root.end() && items->is_array() ? Dump(*items) : std::string{};
}

/// Put one friend back into the cache, from a reply that carried the whole relationship.
void Replace(const Friend& updated) {
    State& state = Get();
    std::lock_guard lock{state.mutex};

    auto friends = std::make_shared<std::vector<Friend>>(*state.friends);
    const auto it = std::find_if(friends->begin(), friends->end(),
                                 [&](const Friend& known) { return known.id == updated.id; });
    if (it == friends->end()) {
        return;
    }

    *it = updated;
    state.friends = std::move(friends);
}

void Remove(std::uint64_t id) {
    State& state = Get();
    std::lock_guard lock{state.mutex};

    auto friends = std::make_shared<std::vector<Friend>>(*state.friends);
    if (std::erase_if(*friends, [&](const Friend& known) { return known.id == id; }) > 0) {
        state.friends = std::move(friends);
    }
}

void StoreUser(const std::string& body) {
    const Json document = Json::parse(body, nullptr, false);
    if (document.is_discarded()) {
        // A reply that is not a user object leaves the last one standing.
        return;
    }

    if (auto setting = ParseUserSetting(document)) {
        State& state = Get();
        std::lock_guard lock{state.mutex};
        state.setting = std::move(*setting);
    }
}

std::string Base64(const std::vector<std::uint8_t>& data) {
    static constexpr char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        const std::uint32_t n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out.push_back(table[(n >> 18) & 63]);
        out.push_back(table[(n >> 12) & 63]);
        out.push_back(table[(n >> 6) & 63]);
        out.push_back(table[n & 63]);
    }
    if (i < data.size()) {
        std::uint32_t n = static_cast<std::uint32_t>(data[i]) << 16;
        if (i + 1 < data.size()) {
            n |= static_cast<std::uint32_t>(data[i + 1]) << 8;
        }
        out.push_back(table[(n >> 18) & 63]);
        out.push_back(table[(n >> 12) & 63]);
        out.push_back(i + 1 < data.size() ? table[(n >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

/// Uri.EscapeDataString: everything but the unreserved characters is percent-encoded.
std::string EscapeDataString(const std::string& text) {
    std::string out;
    for (const unsigned char c : text) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out += fmt::format("%{:02X}", c);
        }
    }
    return out;
}

} // namespace

// ---- transport ----

Reply detail::Send(const std::string& method, const std::string& host,
                   const std::string& path_and_query, const std::string& content_type,
                   const std::string& body) {
    State& state = Get();
    Transport transport;
    {
        std::lock_guard lock{state.mutex};
        if (state.user_id.empty()) {
            return {};
        }
        transport = state.test_transport;
    }

    try {
        if (transport) {
            return transport(method, host, path_and_query, content_type, body);
        }

        const Session::Response response =
            Session::SendAs(method, host, path_and_query, content_type, body);
        return {response.status, response.body};
    } catch (const std::exception& e) {
        LOG_DEBUG(WebService, "[OpenPak] {} {}: {}", method, host, e.what());
        return {};
    }
}

void SetTransportForTest(Transport transport) {
    State& state = Get();
    std::lock_guard lock{state.mutex};
    state.test_transport = std::move(transport);
}

// ---- results ----

int BaasError(const Reply& reply) {
    if (reply.Success()) {
        return Ok;
    }

    const Json root = Json::parse(reply.body, nullptr, false);

    // No body the parser understood, or one that is not an object: the status alone decides.
    if (!root.is_discarded() && root.is_object()) {
        const auto status = root.find("status");
        const auto error = root.find("errorCode");

        if (status != root.end() && status->is_number_integer() && error != root.end() &&
            error->is_string()) {
            const std::int64_t code = status->get<std::int64_t>();
            const std::string& text = error->get_ref<const std::string&>();

            if (code >= INT32_MIN && code <= INT32_MAX && text.size() <= 0x3F) {
                const auto found = BaasErrors().find({static_cast<int>(code), text});
                return found != BaasErrors().end() ? found->second
                                                   : FromStatus(static_cast<int>(code));
            }
        }
    }

    return FromStatus(reply.status);
}

int FiveError(const Reply& reply) {
    if (reply.Success()) {
        return Ok;
    }

    const Json root = Json::parse(reply.body, nullptr, false);

    // As above: fall back to the status.
    if (!root.is_discarded() && root.is_object()) {
        const auto error = root.find("error");

        if (error != root.end() && error->is_object()) {
            const auto code = error->find("code");
            const auto message = error->find("message");

            if (code != error->end() && code->is_string() && code->get_ref<const std::string&>().size() == 4 &&
                message != error->end() && message->is_string()) {
                static const std::map<std::string, int> codes{
                    {"0001", 2901}, {"0002", 2902}, {"0003", 2903}, {"0004", 2904},
                    {"0005", 2905}, {"0006", 2906}, {"0007", 2907}, {"0008", 2908},
                    {"0009", 2909}, {"0010", 2910},
                };
                const auto found = codes.find(code->get<std::string>());
                return found != codes.end() ? found->second : 2999;
            }
        }
    }

    return FromStatus(reply.status);
}

// ---- who is signed in ----

void Attach(const std::string& user_id) {
    State& state = Get();
    std::lock_guard lock{state.mutex};

    if (state.user_id != user_id) {
        ForgetLocked(state);
    }

    state.user_id = user_id;
}

void Detach() {
    State& state = Get();
    std::lock_guard lock{state.mutex};

    state.user_id.clear();
    ForgetLocked(state);
}

bool Ready() {
    State& state = Get();
    std::lock_guard lock{state.mutex};
    return !state.user_id.empty();
}

std::string UserId() {
    State& state = Get();
    std::lock_guard lock{state.mutex};
    return state.user_id;
}

void SetFriendListChangedHook(std::function<void()> hook) {
    State& state = Get();
    std::lock_guard lock{state.mutex};
    state.on_friend_list_changed = std::move(hook);
}

void NotifyFriendListChanged() {
    RaiseFriendListChanged();
}

void SetFriendRequestArrivedHook(std::function<void()> hook) {
    State& state = Get();
    std::lock_guard lock{state.mutex};
    state.on_friend_request_arrived = std::move(hook);
}

// ---- the friend list (§A.1, §A.2, §A.3) ----

bool FriendListAvailable() {
    State& state = Get();
    std::lock_guard lock{state.mutex};
    return state.friends_synced;
}

std::shared_ptr<const std::vector<Friend>> Friends() {
    State& state = Get();
    std::lock_guard lock{state.mutex};
    return state.friends;
}

std::optional<Friend> FindFriend(std::uint64_t id) {
    for (const Friend& known : *Friends()) {
        if (known.id == id) {
            return known;
        }
    }
    return std::nullopt;
}

void Update(std::uint64_t id, const std::function<void(Friend&)>& change) {
    State& state = Get();
    std::lock_guard lock{state.mutex};

    auto friends = std::make_shared<std::vector<Friend>>(*state.friends);
    const auto it = std::find_if(friends->begin(), friends->end(),
                                 [&](const Friend& known) { return known.id == id; });
    if (it == friends->end()) {
        return;
    }

    change(*it);
    state.friends = std::move(friends);
}

int SyncFriendList(bool force) {
    State& state = Get();
    std::string user_id;
    {
        std::lock_guard lock{state.mutex};
        user_id = state.user_id;

        if (user_id.empty()) {
            return Unknown;
        }

        if (!force && state.friends_synced &&
            std::chrono::steady_clock::now() - state.friends_synced_at < ListCooldown) {
            return Ok;
        }
    }

    const auto [error, root] =
        GetJson(BaasHost, fmt::format("/2.0.0/users/{}/friends?count=300", user_id), false);
    if (error != Ok) {
        return error;
    }

    auto friends = std::make_shared<const std::vector<Friend>>(ParseFriendList(root));
    // A friend is listed as playing a title, which may be one this machine has never heard of (a
    // platform's own, served by the server: Pier, Harbor ...). Ask the server to name each one now,
    // so the list that draws them has them, and tell it when they arrive. Titles installed here
    // are named locally first; the cache is only the fallback.
    for (const Friend& entry : *friends) {
        PlatformTitles::Prefetch(entry.application_id, [] { RaiseFriendListChanged(); });
    }
    std::string signature = Signature(root);
    bool changed;
    {
        std::lock_guard lock{state.mutex};
        changed = state.friends_signature != signature;
        state.friends = std::move(friends);
        state.friends_signature = std::move(signature);
        state.friends_synced = true;
        state.friends_synced_at = std::chrono::steady_clock::now();
    }

    if (changed) {
        RaiseFriendListChanged();
    }

    return Ok;
}

int PatchFriend(std::uint64_t friend_id, const std::string& op, const std::string& path,
                const Json& value) {
    const Reply reply =
        detail::Send("PATCH", BaasHost,
                     fmt::format("/2.0.0/users/{}/friends/{}", UserId(), Hex16(friend_id)),
                     JsonPatch, Patch(op, path, value));

    if (!reply.Success()) {
        return BaasError(reply);
    }

    const Json document = Json::parse(reply.body, nullptr, false);
    if (document.is_discarded()) {
        return FriendNotFound;
    }

    const std::optional<Friend> updated = ParseFriend(document);
    if (!updated) {
        return FriendNotFound;
    }

    Replace(*updated);

    // The entry the guest reads is not the one it read a moment ago, which is exactly what the
    // notification queue's list-update event says.
    RaiseFriendListChanged();

    return Ok;
}

int DeleteFriend(std::uint64_t friend_id) {
    const Reply reply = detail::Send(
        "DELETE", BaasHost, fmt::format("/2.0.0/users/{}/friends/{}", UserId(), Hex16(friend_id)),
        {}, {});

    if (reply.Success()) {
        SyncFriendList(true);
        return Ok;
    }

    const int error = BaasError(reply);

    if (error == 2031 || (error >= 2220 && error <= 2229)) {
        Remove(friend_id);
        RaiseFriendListChanged();
        return FriendNotFound;
    }

    return error;
}

// ---- users, relationships and blocks (§A.7, §A.8) ----

std::pair<int, std::vector<User>> FetchUsers(const std::vector<std::uint64_t>& ids) {
    std::vector<std::string> unique;

    for (const std::uint64_t id : ids) {
        const std::string text = Hex16(id);

        if (unique.size() == 100) {
            break;
        }

        if (std::find(unique.begin(), unique.end(), text) == unique.end()) {
            unique.push_back(text);
        }
    }

    if (unique.empty()) {
        return {Ok, {}};
    }

    const auto [error, root] = GetJson(
        BaasHost, fmt::format("/1.0.0/users?filter.id.$in={}", fmt::join(unique, ",")), false);

    if (error != Ok) {
        return {error, {}};
    }

    return {Ok, ParseUsers(root)};
}

std::pair<int, std::optional<User>> UserByFriendCode(const std::string& friend_code) {
    const auto [error, root] = GetJson(
        BaasHost,
        fmt::format("/1.0.0/users?filter.links.friendCode.id.$in={}", EscapeDataString(friend_code)),
        false);

    if (error != Ok) {
        return {error, std::nullopt};
    }

    std::vector<User> users = ParseUsers(root);
    if (users.empty()) {
        return {2221, std::nullopt};
    }

    return {Ok, std::move(users.front())};
}

std::pair<int, std::optional<Relationship>> FetchRelationship(std::uint64_t user_id) {
    const auto [error, root] = GetJson(
        BaasHost, fmt::format("/2.0.0/users/{}/relationships/{}", UserId(), Hex16(user_id)),
        false);

    if (error == 2031 || error == 3404) {
        return {2221, std::nullopt};
    }

    if (error != Ok) {
        return {error, std::nullopt};
    }

    return {Ok, ParseRelationship(root)};
}

bool BlockListAvailable() {
    State& state = Get();
    std::lock_guard lock{state.mutex};
    return state.blocks_synced;
}

std::shared_ptr<const std::vector<Block>> Blocks() {
    State& state = Get();
    std::lock_guard lock{state.mutex};
    return state.blocks;
}

int SyncBlockList() {
    const std::string user_id = UserId();
    if (user_id.empty()) {
        return Unknown;
    }

    const auto [error, root] =
        GetJson(BaasHost, fmt::format("/1.0.0/users/{}/blocks?count=100", user_id), false);
    if (error != Ok) {
        return error;
    }

    std::optional<std::vector<Block>> blocks = ParseBlocks(root);
    if (!blocks) {
        return Unknown;
    }

    State& state = Get();
    std::lock_guard lock{state.mutex};
    state.blocks = std::make_shared<const std::vector<Block>>(std::move(*blocks));
    state.blocks_synced = true;

    return Ok;
}

void detail::ReplaceBlocks(std::vector<Block> blocks) {
    State& state = Get();
    std::lock_guard lock{state.mutex};
    state.blocks = std::make_shared<const std::vector<Block>>(std::move(blocks));
}

// ---- the caller's own user (§A.6) ----

std::optional<UserSetting> OwnUserSetting() {
    State& state = Get();
    std::lock_guard lock{state.mutex};
    return state.setting;
}

int SyncUserSetting() {
    const std::string user_id = UserId();
    if (user_id.empty()) {
        return Unknown;
    }

    const auto [error, root] = GetJson(BaasHost, fmt::format("/1.0.0/users/{}", user_id), false);
    if (error != Ok) {
        return error;
    }

    std::optional<UserSetting> setting = ParseUserSetting(root);
    if (!setting) {
        return Unknown;
    }

    const bool has_code = !setting->friend_code.empty();
    {
        State& state = Get();
        std::lock_guard lock{state.mutex};
        state.setting = std::move(*setting);
    }

    if (!has_code) {
        return IssueFriendCode();
    }

    return Ok;
}

int PatchUser(const std::string& body) {
    const Reply reply = detail::Send("PATCH", BaasHost, fmt::format("/1.0.0/users/{}", UserId()),
                                     JsonPatch, body);

    if (!reply.Success()) {
        return BaasError(reply);
    }

    StoreUser(reply.body);

    return Ok;
}

int IssueFriendCode() {
    const Reply reply =
        detail::Send("POST", BaasHost, fmt::format("/1.0.0/users/{}/generate_code", UserId()),
                     detail::FormType, "type=NX");

    if (!reply.Success()) {
        return BaasError(reply);
    }

    StoreUser(reply.body);

    return Ok;
}

std::string PlayLogPermissionBody(int permission, const std::string& play_log) {
    static constexpr std::pair<int, const char*> groups[] = {
        {1, "/extras/self/playLog"},
        {2, "/extras/favoriteFriends/playLog"},
        {3, "/extras/friends/playLog"},
        {5, "/extras/everyone/playLog"},
    };

    Json patch = Json::array();

    for (const auto& [group, path] : groups) {
        patch.push_back(Json{{"op", "add"},
                             {"path", path},
                             {"value", group == permission ? play_log : std::string{}}});
    }

    return Dump(patch);
}

std::string UserPatchBody(const std::string& path, const Json& value) {
    return Patch("replace", path, value);
}

// ---- invitations (Five, invitations doc §1c-§3e) ----

std::pair<int, std::vector<Invitation>> FetchInvitations() {
    const auto [error, root] = GetJson(
        FiveHost, fmt::format("/v2/users/{}/invitations/inbox?invitation_types=friend", UserId()),
        true);

    if (error != Ok) {
        return {error, {}};
    }

    std::optional<std::vector<Invitation>> invitations = ParseInvitations(root);
    if (!invitations) {
        return {Unknown, {}};
    }

    return {Ok, std::move(*invitations)};
}

std::pair<int, std::optional<InvitationGroup>> FetchInvitationGroup(std::uint64_t group_id) {
    const auto [error, root] =
        GetJson(FiveHost, fmt::format("/v1/invitation_groups/{}", group_id), true);

    if (error != Ok) {
        return {error, std::nullopt};
    }

    std::optional<InvitationGroup> group = ParseInvitationGroup(root);
    if (!group) {
        return {Unknown, std::nullopt};
    }

    return {Ok, std::move(group)};
}

int SendInvitation(const std::vector<std::string>& receiver_ids, std::uint64_t application_id,
                   std::uint8_t acd_index, std::uint64_t application_group_id,
                   const std::vector<std::uint8_t>& application_data,
                   const std::vector<std::pair<std::string, std::string>>& messages,
                   bool application_id_match) {
    Json body = Json::object();
    body["receiver_ids"] = receiver_ids;
    body["invitation_type"] = "friend";
    body["application_id"] = Hex16(application_id);
    body["acd_index"] = acd_index;
    body["application_group_id"] = Hex16(application_group_id);

    if (!application_data.empty()) {
        body["application_data"] = Base64(application_data);
    }

    Json slots = Json::object();
    for (const auto& [language, text] : messages) {
        slots[language] = text;
    }
    body["messages"] = std::move(slots);
    body["application_id_match"] = application_id_match;

    const std::string text = Dump(body);

    // The module's body buffer is 0x2400 bytes and a longer one never leaves the console.
    if (text.size() > 0x2400) {
        return TooLarge;
    }

    const Reply reply = detail::Send("POST", FiveHost, "/v2/invitation_groups", JsonType, text);

    return reply.Success() ? Ok : FiveError(reply);
}

int ReadInvitations(const std::vector<std::uint64_t>& invitation_ids) {
    if (invitation_ids.empty()) {
        // The earlier port sends an empty string body with no type of its own, which its transport
        // turns into an empty application/json content.
        const Reply all = detail::Send(
            "PATCH", FiveHost, fmt::format("/v1/users/{}/invitations/mark_as_read", UserId()),
            JsonType, {});

        return all.Success() ? Ok : FiveError(all);
    }

    std::vector<std::string> ids;

    for (const std::uint64_t id : invitation_ids) {
        if (ids.size() == 100) {
            break;
        }

        ids.push_back(std::to_string(id));
    }

    const Reply reply = detail::Send("PATCH", FiveHost, "/v1/invitations", detail::FormType,
                                     fmt::format("read=true&ids={}", fmt::join(ids, "%2C")));

    return reply.Success() ? Ok : FiveError(reply);
}

// ---- caches (22000-22003, 10500/20500) ----

std::shared_ptr<const std::vector<Invitation>> Invitations() {
    State& state = Get();
    std::lock_guard lock{state.cache_mutex};
    return state.invitations;
}

std::optional<InvitationGroup> CachedInvitationGroup(std::uint64_t group_id) {
    State& state = Get();
    std::lock_guard lock{state.cache_mutex};
    const auto it = state.groups.find(group_id);
    return it != state.groups.end() ? std::optional{it->second} : std::nullopt;
}

void RememberUser(const User& user) {
    if (user.id == 0) {
        return;
    }

    State& state = Get();
    std::lock_guard lock{state.cache_mutex};
    state.users[user.id] = user;
}

std::optional<User> CachedUser(std::uint64_t id) {
    State& state = Get();
    std::lock_guard lock{state.cache_mutex};
    const auto it = state.users.find(id);
    return it != state.users.end() ? std::optional{it->second} : std::nullopt;
}

int SyncInvitations() {
    auto [error, invitations] = FetchInvitations();

    if (error != Ok) {
        return error;
    }

    State& state = Get();
    auto shared = std::make_shared<const std::vector<Invitation>>(std::move(invitations));
    {
        std::lock_guard lock{state.cache_mutex};
        state.invitations = shared;
    }

    for (const Invitation& invitation : *shared) {
        {
            std::lock_guard lock{state.cache_mutex};
            if (state.groups.contains(invitation.group_id)) {
                continue;
            }
        }

        auto [group_error, group] = FetchInvitationGroup(invitation.group_id);

        if (group_error == Ok && group) {
            std::lock_guard lock{state.cache_mutex};
            state.groups[group->id] = std::move(*group);
        }
    }

    return Ok;
}

void WarmUsers(const std::vector<std::uint64_t>& ids) {
    // Nobody signed in can ask: nothing is marked as asked either, so the ids are still wanted
    // once somebody is.
    if (!Ready()) {
        return;
    }

    State& state = Get();
    std::vector<std::uint64_t> wanted;
    {
        std::lock_guard lock{state.cache_mutex};
        for (const std::uint64_t id : ids) {
            if (id != 0 && !state.users.contains(id) && state.warming.insert(id).second) {
                wanted.push_back(id);
            }
        }
    }

    if (wanted.empty()) {
        return;
    }

    const auto [error, users] = FetchUsers(wanted);

    std::lock_guard lock{state.cache_mutex};

    if (error == Ok) {
        for (const User& user : users) {
            state.users[user.id] = user;
        }
    }

    // Either way the id may be asked for again: a failure that is never retried is a name that
    // never appears.
    for (const std::uint64_t id : wanted) {
        state.warming.erase(id);
    }
}

void SyncModuleCaches() {
    if (!Ready()) {
        return;
    }

    SyncBlockList();
    SyncUserSetting();
    SyncInvitations();
}

// ---- the request boxes ----

std::shared_ptr<const std::vector<Request>> InboxRequests() {
    State& state = Get();
    std::lock_guard lock{state.mutex};
    return state.inbox_requests;
}

std::shared_ptr<const std::vector<Request>> OutboxRequests() {
    State& state = Get();
    std::lock_guard lock{state.mutex};
    return state.outbox_requests;
}

void SetFriendRequests(std::vector<Request> inbox, std::vector<Request> outbox) {
    State& state = Get();
    bool arrived;
    std::function<void()> hook;
    {
        std::lock_guard lock{state.mutex};

        const int unread = static_cast<int>(std::count_if(
            inbox.begin(), inbox.end(), [](const Request& request) { return !request.read; }));

        arrived = unread > state.unread_requests;
        state.unread_requests = unread;

        state.inbox_requests = std::make_shared<const std::vector<Request>>(std::move(inbox));
        state.outbox_requests = std::make_shared<const std::vector<Request>>(std::move(outbox));
        hook = state.on_friend_request_arrived;
    }

    if (arrived && hook) {
        hook();
    }
}

void BackgroundSync() {
    RefreshFriendRequests();
    SyncFriendList(false);
    SyncModuleCaches();
}

namespace {

/// The worker behind RunInBackground. It lives as long as the process and is stopped and joined
/// when the static is torn down: a detached thread would outlive the state its tasks read.
struct Worker {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::function<void()>> tasks;
    bool stopping = false;
    std::thread thread;

    ~Worker() {
        {
            std::lock_guard lock{mutex};
            stopping = true;
            tasks.clear();
        }
        wake.notify_all();

        if (thread.joinable()) {
            thread.join();
        }
    }

    void Run() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock lock{mutex};
                wake.wait(lock, [this] { return stopping || !tasks.empty(); });
                if (stopping) {
                    return;
                }
                task = std::move(tasks.front());
                tasks.pop_front();
            }

            try {
                task();
            } catch (const std::exception& e) {
                LOG_WARNING(WebService, "[OpenPak] Friends background task failed: {}", e.what());
            } catch (...) {
                LOG_WARNING(WebService, "[OpenPak] Friends background task failed");
            }
        }
    }
};

Worker& GetWorker() {
    static Worker worker;
    return worker;
}

} // namespace

void RunInBackground(std::function<void()> task) {
    if (!task) {
        return;
    }

    Worker& worker = GetWorker();
    {
        std::lock_guard lock{worker.mutex};
        if (worker.stopping) {
            return;
        }
        worker.tasks.push_back(std::move(task));
        if (!worker.thread.joinable()) {
            worker.thread = std::thread([&worker] { worker.Run(); });
        }
    }
    worker.wake.notify_one();
}

} // namespace OpenPak::Baas

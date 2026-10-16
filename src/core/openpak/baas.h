// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later
// Derived from openpak-client 3fd9ba7 (baas.h).

// The friends sysmodule's own REST surface (friends contract 22.5.0 §A), spoken to nx-baas and
// to the Five invitation host, plus what it last answered. Ported from the earlier Ryujinx port
// (Ryujinx.OpenPak/OpenPakBaas*.cs and the request half of OpenPakSession.Requests.cs).
//
// The session that holds the pinned client and the user-scoped bearer plugs itself in through
// Attach; the guest's friends module calls these and reads the caches. Every id here is the BAAS
// user id the server hands out -- the id a friend request, an invitation and a relationship are
// all addressed by -- and never anything derived from it.
//
// Results are friends-module descriptions (2121-xxxx), 0 for success, mapped from the status
// and error body exactly as §1.2 maps them.
//
// Threading: every call that talks to the server is synchronous and blocks for a round trip or
// more, so it belongs on a background thread and never on the game's. The cache getters take a
// mutex for a moment and copy a pointer or a small value, and are safe from any thread. Hooks run
// on whichever thread made the call that raised them, with no lock held; they must not throw.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/openpak/baas_models.h"

namespace OpenPak::Baas {

/// JSON as the module reads and writes it: key order is kept, because a playLog group is picked
/// by the first one in the document and a body's keys go out in the module's own order.
using Json = nlohmann::ordered_json;

inline constexpr const char* BaasHost = "e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com";
inline constexpr const char* FiveHost = "app.lp1.five.nintendo.net";

// ---- results (§1.2) ----

/// Success.
inline constexpr int Ok = 0;

/// 2121-0002, a bad argument.
inline constexpr int InvalidArgument = 2;

/// 2121-0012, a body that does not fit the module's buffer.
inline constexpr int TooLarge = 12;

/// 2121-2411, "that friend is not in the cache".
inline constexpr int FriendNotFound = 2411;

/// 2121-3999, a status the module has no mapping for, and no reachable server.
inline constexpr int Unknown = 3999;

/// What one request produced: any 2xx is success, whatever the body (§1.2). Status 0 is a
/// request that could not be made at all.
struct Reply {
    int status = 0;
    std::string body;

    bool Success() const {
        return status >= 200 && status < 300;
    }
};

/// How a request reaches the server, with this user's bearer on it. `content_type` empty means
/// no body at all.
using Transport =
    std::function<Reply(const std::string& method, const std::string& host,
                        const std::string& path_and_query, const std::string& content_type,
                        const std::string& body)>;

/// A BAAS error body: $.status and $.errorCode together pick the result.
int BaasError(const Reply& reply);

/// A Five error body: $.error.code is four characters, and picks 2121-29xx.
int FiveError(const Reply& reply);

// ---- who is signed in ----

/// A signed-in session names its BAAS user. Caches from another user are dropped. The session
/// calls this after every sign-in; nothing else needs to.
void Attach(const std::string& user_id);

/// Signed out, or another profile: nothing of the last user may be read as this one's.
void Detach();

/// Whether there is a signed-in BAAS user to speak for.
bool Ready();

/// The caller's own BAAS user id, or empty.
std::string UserId();

/// Replace the session's transport, so tests can see every request and answer it themselves.
/// An empty function puts the session back.
void SetTransportForTest(Transport transport);

// ---- hooks ----

/// The friend list changed: a friend, a flag or a presence is not what it was. This is what the
/// guest's notification queue carries as a friend-list update.
void SetFriendListChangedHook(std::function<void()> hook);

/// Tell the friend list that what it draws changed (a title it asked the server to name arrived).
void NotifyFriendListChanged();

/// A request arrived that was not in the box before: the unread count went up, which is what the
/// module's inbox parse signals and what the guest's notification queue carries as a
/// new-friend-request event.
void SetFriendRequestArrivedHook(std::function<void()> hook);

// ---- the friend list (§A.1, §A.2, §A.3) ----

/// Whether a friend list has ever been fetched for this user.
bool FriendListAvailable();

/// The friend list as of the last sync. Never null; the list it points at never changes.
std::shared_ptr<const std::vector<Friend>> Friends();

/// One friend out of the cache.
std::optional<Friend> FindFriend(std::uint64_t id);

/// Change one friend in the cache, for the local half of a queued change (§A.2).
void Update(std::uint64_t id, const std::function<void(Friend&)>& change);

/// GET /2.0.0/users/<me>/friends?count=300. The server's list is authoritative: it replaces the
/// cache whole, and anything that changed raises the friend-list hook.
///
/// `force` clears the 30 s cooldown, as an explicit sync and every successful write do.
int SyncFriendList(bool force);

/// PATCH /2.0.0/users/<me>/friends/<id>, one op. The reply must be the whole relationship
/// object: the module replaces its cached entry with it, and a reply that is not one fails with
/// 2121-2411.
int PatchFriend(std::uint64_t friend_id, const std::string& op, const std::string& path,
                const Json& value);

/// DELETE /2.0.0/users/<me>/friends/<id> (§A.3). On success the list is synced inline -- the
/// friendship is gone because the next list no longer carries it. A friend the server does not
/// know is dropped locally and reported as 2121-2411.
int DeleteFriend(std::uint64_t friend_id);

// ---- users, relationships and blocks (§A.7, §A.8) ----

/// GET /1.0.0/users?filter.id.$in=... -- at most 100 de-duplicated ids, comma-joined.
std::pair<int, std::vector<User>> FetchUsers(const std::vector<std::uint64_t>& ids);

/// GET /1.0.0/users?filter.links.friendCode.id.$in=<code>. The code goes over the wire exactly as
/// it was typed. An empty result is 2121-2221.
std::pair<int, std::optional<User>> UserByFriendCode(const std::string& friend_code);

/// GET /2.0.0/users/<me>/relationships/<id>; a 404 is 2121-2221.
std::pair<int, std::optional<Relationship>> FetchRelationship(std::uint64_t user_id);

/// Whether a block list has ever been fetched for this user.
bool BlockListAvailable();

/// The block list as of the last sync. Never null.
std::shared_ptr<const std::vector<Block>> Blocks();

/// GET /1.0.0/users/<me>/blocks?count=100 (§A.8). One item without targetUserId or a nickname
/// fails the whole parse, as it does on the console.
int SyncBlockList();

/// The reason string a block carries, by the number the IPC call names it with (1-4), or null.
const char* BlockReason(int reason);

/// The POST body (§A.8): {"targetUserId":"%016llx","extras":{"self":{"reason":...}}}, plus the
/// route keys when the block came from inside a title (30401/30403).
std::string BlockBody(std::uint64_t target_id, const std::string& reason,
                      const std::optional<Route>& route);

/// POST /1.0.0/users/<me>/blocks. The block is cached before the request leaves; a success
/// re-syncs blocks, the friend list and the request boxes. Remaps 2031 -> 2213, 2061 -> 2701.
/// Blocks for a round trip or more: hand it to RunInBackground from a guest command.
int BlockUser(std::uint64_t target_id, int reason, const std::optional<Route>& route);

/// DELETE /1.0.0/users/<me>/blocks/<id>. One the server does not know (404, 2220-2229) is
/// removed locally and reported as 2121-2711. Blocks, as above.
int UnblockUser(std::uint64_t target_id);

// ---- the caller's own user (§A.6) ----

/// The caller's own user as of the last sync.
std::optional<UserSetting> OwnUserSetting();

/// GET /1.0.0/users/<me>, and the friend code it issues when there is none yet. The module's
/// steps 3 and 4 (an initial playLog and the local play-log queue) are not walked: which group
/// the module would pick is not established, and nothing here keeps a queue.
int SyncUserSetting();

/// PATCH /1.0.0/users/<me>. The reply is the whole user object and replaces what is held here.
int PatchUser(const std::string& body);

/// POST /1.0.0/users/<me>/generate_code with the literal body `type=NX` (§A.6). The reply is the
/// whole user, carrying the new code.
int IssueFriendCode();

/// The four `add` ops a play-log permission change is made of, always in the same order: the
/// chosen group keeps the log and the other three are emptied (§A.6).
std::string PlayLogPermissionBody(int permission, const std::string& play_log);

/// One `replace` op on the caller's own user.
std::string UserPatchBody(const std::string& path, const Json& value);

// ---- invitations (Five, invitations doc §1c-§3e) ----

/// GET /v2/users/<me>/invitations/inbox?invitation_types=friend: read and unread.
std::pair<int, std::vector<Invitation>> FetchInvitations();

/// GET /v1/invitation_groups/<id>: one group, with its receivers and messages.
std::pair<int, std::optional<InvitationGroup>> FetchInvitationGroup(std::uint64_t group_id);

/// POST /v2/invitation_groups with the body the module builds (invitations doc §3c), keys in its
/// order: an empty application_data is left out, and only the message slots the game filled are
/// sent. Any 2xx is success -- the reply is never read.
int SendInvitation(const std::vector<std::string>& receiver_ids, std::uint64_t application_id,
                   std::uint8_t acd_index, std::uint64_t application_group_id,
                   const std::vector<std::uint8_t>& application_data,
                   const std::vector<std::pair<std::string, std::string>>& messages,
                   bool application_id_match);

/// PATCH /v1/invitations, the form body the module sends: decimal ids joined by %2C, at most 100
/// of them. An empty list is the mark-all form instead.
int ReadInvitations(const std::vector<std::uint64_t>& invitation_ids);

// ---- the caches the guest reads synchronously (22000-22003, 10500/20500) ----
//
// Everything here is filled on the session's poll, never on the game's thread. A command that
// finds nothing cached answers with what it has and warms the cache for the next call, which is
// what a console's own "cache exists / ensure available" pair amounts to.

/// The invitation inbox as of the last poll, read and unread. Never null.
std::shared_ptr<const std::vector<Invitation>> Invitations();

/// One invitation group out of the cache, or nothing when it has not been fetched.
std::optional<InvitationGroup> CachedInvitationGroup(std::uint64_t group_id);

/// One user out of the cache.
/// Put a user we already know into the cache without asking BAAS for it. The local player is the
/// case that matters: they are never their own friend, so nothing else ever caches them, and a game
/// that asks for its own profile alongside its friends' gets an invalid slot back (measured on
/// Moving Out 2: three ids asked, two resolved). Their name comes with the login reply, so the
/// first lookup of a session can be answered from it.
void RememberUser(const User& user);

std::optional<User> CachedUser(std::uint64_t id);

/// The invitation inbox and every group it names (§A.5). Groups are fetched once each: a group is
/// immutable once sent, so the one cached is the one 22001 should answer with.
int SyncInvitations();

/// Fetch users the guest asked about and does not have cached, once per id. The call that asked
/// is answered from whatever is already there; the next one gets the rest, as a console's own user
/// cache behaves. This blocks for the fetch: a guest command hands it to a worker thread and
/// answers at once.
void WarmUsers(const std::vector<std::uint64_t>& ids);

/// The caches the guest's friends module reads and cannot fetch for itself, because a command
/// answered with an HTTP round trip is a game left waiting: the block list (§A.8), the caller's
/// own user (§A.6) and the invitation inbox with its groups (§A.5).
void SyncModuleCaches();

// ---- friend requests (§A.4) ----

/// The received request box as of the last poll, PENDING only: what the guest's 20201/20202
/// serve. Never null.
std::shared_ptr<const std::vector<Request>> InboxRequests();

/// The sent request box, as above. Never null.
std::shared_ptr<const std::vector<Request>> OutboxRequests();

/// Replace both request boxes with a fresh fetch, raising the arrival hook when the unread count
/// went up.
void SetFriendRequests(std::vector<Request> inbox, std::vector<Request> outbox);

/// What is waiting in the received and sent request boxes. One malformed item is skipped by the
/// parser, never the box: the rest are still requests somebody made.
void RefreshFriendRequests();

/// Answer or withdraw a request (30202-30204): PATCH the state, then re-read both boxes, as the
/// console's inline sync does after every such write. `state` is "CANCELED", "AUTHORIZED" or
/// "REJECTED".
void AnswerFriendRequest(std::uint64_t request_id, const std::string& state);

/// Mark a received request read (30205), then recount from the boxes.
void ReadFriendRequest(std::uint64_t request_id);

/// Send a request (30200/30201/10200): resolve the guest's id to the BAAS user the server names,
/// POST the variant body, and sync at once when the reply says the receiver had already asked
/// (AUTHORIZED).
void SendFriendRequest(const FriendRequestSend& send);

/// The receiver for an id a game passed itself. The friend list the guest reads is the server's,
/// so an id in it is already the BAAS user id a request or invitation is addressed to. Anything
/// else is still looked up through the website friend list by friend code, for a game that kept
/// an id from before. Always 16 hex digits.
std::string ReceiverId(std::uint64_t network_service_account_id);

/// One background beat's worth of the module's syncs, in the reference's order: the request
/// boxes, the friend list at its cooldown, then the module caches. The session's heartbeat calls
/// it every third beat and once after each sign-in.
void BackgroundSync();

/// Run `task` on the module's own worker thread and return at once: the fire-and-forget the
/// guest's writes need, because every call above blocks for a round trip and a guest's IPC thread
/// may never wait on one. Tasks run one at a time, in the order they were handed over; one that
/// throws is logged and dropped. Tasks still queued when the process exits are not run.
void RunInBackground(std::function<void()> task);

// ---- parsing (§1.3): which fields are required, what types they must have, and what a missing
// one does. Unknown keys are ignored everywhere; a field of the wrong type counts as absent. ----

/// The channels table (§A.1), 1-based.
extern const std::array<const char*, 10> Channels;

/// The invitation message slots, in the module's order (invitations doc §2b).
extern const std::array<const char*, 16> MessageLanguages;

/// A channel string as its 1-based table index; 0 when unknown.
int ChannelOf(std::string_view channel);

/// A 64-bit id as the module reads one (strtoull base 16): leading spaces, an optional '+' and
/// "0x", at most 16 digits; a '-' rejects it. Only a JSON string qualifies.
bool TryHex(const Json& element, std::uint64_t& value);

/// The same, for text that is not in a JSON document.
bool TryHexText(std::string_view text, std::uint64_t& value);

/// A friend-list item or a single-friend PATCH reply (§A.1, §A.2). Nothing when friendId,
/// friend.nickname or friend.thumbnailUrl is missing: the module skips that item.
std::optional<Friend> ParseFriend(const Json& item);

/// The whole friend list, at most 300 items, incomplete ones skipped.
std::vector<Friend> ParseFriendList(const Json& root);

/// A playLog string: a JSON array of at most 20 entries. Each needs appId, presenceGroupId and
/// the four counters; acdIndex is optional. A repeated appId merges, the larger counters winning.
std::vector<PlayLog> ParsePlayLog(std::string_view text);

/// One friend request (§A.4.1, §A.4.2, §A.4.4). `inbox` is receiver mode: the other party is the
/// sender. Nothing when a required field is missing or the nickname is 0x21 bytes or longer.
std::optional<Request> ParseRequest(const Json& item, bool inbox);

/// A request box page. Items missing a required field are left out.
std::vector<Request> ParseRequestList(const Json& root, bool inbox);

/// The badge (§A.4.3): unread is the items without extras.receiver.read true. {unread, read}.
std::pair<int, int> ParseRequestCount(const Json& root);

/// A flat user (§A.7); nothing without id, nickname and thumbnailUrl.
std::optional<User> ParseUser(const Json& item);

std::vector<User> ParseUsers(const Json& root);

/// The caller's own user (§A.6); nothing without id, nickname and thumbnailUrl.
std::optional<UserSetting> ParseUserSetting(const Json& root);

/// The block list (§A.8). Nothing -- the whole parse failed -- when an item lacks targetUserId or
/// targetUser.nickname, or there are more than 100 items.
std::optional<std::vector<Block>> ParseBlocks(const Json& root);

/// The invitation inbox (invitations doc §2a). Nothing when any item is incomplete: an
/// incomplete item fails the whole parse.
std::optional<std::vector<Invitation>> ParseInvitations(const Json& root);

/// One invitation group (invitations doc §2b); nothing when anything required is missing, or
/// invitations is empty or holds more than 16.
std::optional<InvitationGroup> ParseInvitationGroup(const Json& root);

/// GET relationships (§A.7): nothing is required.
Relationship ParseRelationship(const Json& root);

// ---- the friend-request calls, as the module makes them (§A.4): pure functions of their
// arguments, and testable as such. ----

/// A request box page: GET .../users/<me>/friend_requests/{inbox|outbox} (§A.4.2). The server
/// honours `offset`/`count` and returns only PENDING items; the console trusts the filter.
std::string RequestBoxUrl(const std::string& host, const std::string& user_id, bool inbox,
                          int offset, int count);

/// One request: PATCH .../friend_requests/<id> (§A.4.4).
std::string RequestUrl(const std::string& host, std::uint64_t request_id);

/// Answer or cancel a request (§A.4.4): `state` is "CANCELED" (one L), "AUTHORIZED" or
/// "REJECTED". Anything else throws std::invalid_argument.
std::string AnswerRequestBody(const std::string& state);

/// Mark a received request read (30205): the badge recounts afterwards (§A.4.4).
std::string ReadRequestBody();

/// Send a request (§A.4.1), plain variant: the channel, the two BAAS ids as %016llx, and the
/// sender-only extras. Over 0x400 bytes the call fails before it is sent.
std::string SendRequestBody(const std::string& channel, std::uint64_t sender_id,
                            std::uint64_t receiver_id);

/// Send a request (§A.4.1), app variant: `target_name` is the target's name as the sender saw it
/// (screen name #1, sender-only), `own_name` the sender's own in-app name (screen name #2,
/// visible to both).
std::string SendRequestBody(const std::string& channel, std::uint64_t sender_id,
                            std::uint64_t receiver_id, std::uint64_t application_id,
                            std::uint8_t acd_index, std::uint64_t presence_group_id,
                            const std::string& target_name, const std::string& target_language,
                            const std::string& own_name, const std::string& own_language);

/// Send a request (§A.4.1), catalog variant (30215): the route names an external application
/// catalog id rather than a title.
std::string SendRequestCatalogBody(const std::string& channel, std::uint64_t sender_id,
                                   std::uint64_t receiver_id, const std::string& catalog_id,
                                   const std::string& target_name,
                                   const std::string& target_language, const std::string& own_name,
                                   const std::string& own_language);

/// Send a request (§A.4.1), NNID variant (30217): the sender's own Mii name and image parameter,
/// in both halves of the extras.
std::string SendRequestNnidBody(const std::string& channel, std::uint64_t sender_id,
                                std::uint64_t receiver_id, const std::string& mii_name,
                                const std::string& mii_image_url_param);

/// A 128-bit external catalog id (%016llx%016llx) as its two halves, for the route union. False
/// when it is not 32 hex digits.
bool TryCatalogId(std::string_view catalog_id, std::uint64_t& hi, std::uint64_t& lo);

} // namespace OpenPak::Baas

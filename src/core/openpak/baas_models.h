// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later
// Derived from openpak-client 3fd9ba7 (baas_models.h).

// What the friends module's own BAAS and Five routes answer, as plain data. Ported from OpenPak's earlier
// Ryujinx port (Ryujinx.OpenPak/OpenPakBaasModels.cs); section numbers are the friends
// contract's (internal research notes) and the invitations doc's.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace OpenPak::Baas {

/// One `playLog` entry (§A.1): a title the person played, as the server counts it.
struct PlayLog {
    std::uint64_t application_id = 0;
    std::uint8_t acd_index = 0;
    std::uint64_t presence_group_id = 0;
    std::int64_t total_play_count = 0;
    std::int64_t total_play_time = 0;
    std::int64_t first_played_at = 0;
    std::int64_t last_played_at = 0;
};

/// Where a friendship, request or block came from: the `route:*` keys of `extras.self`,
/// `extras.sender` or `extras.senderAndReceiver`.
struct Route {
    std::uint64_t application_id = 0;
    std::uint8_t acd_index = 0;
    std::uint64_t presence_group_id = 0;
    std::string catalog_id;
    std::string name;
    std::string language;
    std::string mii_name;
    std::string mii_image_url_param;
};

/// One item of GET /2.0.0/users/<me>/friends (§A.1), or the reply to a PATCH of one (§A.2).
/// `id` is the friend's BAAS user id, the id every other call names them by.
struct Friend {
    std::uint64_t id = 0;
    std::string nickname;
    std::string thumbnail_url;

    int state = 0;              ///< ONLINE 1, PLAYING 2, anything else 0.
    std::int64_t updated_at = 0; ///< presence.updatedAt, or logoutAt when state is 0 and present.
    std::uint64_t application_id = 0;
    std::uint64_t presence_group_id = 0;
    std::uint8_t acd_index = 0;
    std::string app_field;      ///< The published appField, a JSON object as a string, or empty.

    bool is_favorite = false;
    bool is_newly = false;      ///< extras.self.isConfirmed absent or false.
    bool is_online_notification = false;
    std::int64_t created_at = 0;
    std::string note;
    int channel = 0;            ///< The channels table, 1-based; 0 when absent or unknown.
    Route route;
    std::vector<PlayLog> play_log;
};

/// One friend request (§A.4.2), seen from one side: `other_id` is the sender for the inbox and
/// the receiver for the outbox.
struct Request {
    std::uint64_t id = 0;
    int channel = 0;
    int state = 0;
    std::uint64_t other_id = 0;
    std::string nickname;
    std::string thumbnail_url;
    std::int64_t created_at = 0;
    bool read = false;
    Route route;
};

/// A flat user from GET /1.0.0/users?filter… (§A.7).
struct User {
    std::uint64_t id = 0;
    std::string nickname;
    std::string thumbnail_url;
    std::vector<PlayLog> play_log;
};

/// The caller's own user, GET /1.0.0/users/<me> (§A.6).
struct UserSetting {
    std::uint64_t id = 0;
    std::string nickname;
    std::string thumbnail_url;
    int presence_permission = 0; ///< SELF 0, FAVORITE_FRIENDS 1, FRIENDS 2; anything else 0.
    int play_log_permission = 0; ///< self 1, favoriteFriends 2, friends 3, everyone 5; 0 unset.
    bool friend_request_reception = false;
    std::string friend_code;
    std::int64_t friend_code_regenerable_at = 0;
    std::string play_log_text; ///< The playLog string as the server holds it, "[…]", or empty.
    std::vector<PlayLog> play_log;
};

/// One item of GET /1.0.0/users/<me>/blocks (§A.8).
struct Block {
    std::uint64_t id = 0;
    std::string nickname;
    std::string thumbnail_url;
    std::int64_t created_at = 0;
    int reason = 0; ///< BAD_FRIEND_REQUEST 1, BAD_FRIEND 2, IN_APP 3, IN_CAMPUS 4; 0 otherwise.
    Route route;
};

/// A request the guest asked to send (§A.4.1): who it goes to, on which channel, and -- for the
/// application variants -- the title and the two in-app screen names. `target_id` is the
/// network-service account id as the guest gave it; the session resolves it to the BAAS user.
struct FriendRequestSend {
    std::uint64_t target_id = 0;
    std::string channel;
    std::uint64_t application_id = 0;
    std::uint8_t acd_index = 0;
    std::uint64_t presence_group_id = 0;
    std::string target_name;
    std::string target_language;
    std::string own_name;
    std::string own_language;
    std::string catalog_id; ///< 32 hex digits for a catalog route (30215), or empty.
    std::string mii_name;   ///< The sender's own Mii, for an NNID route (30217).
    std::string mii_image_url_param;
};

/// The relationship to one user, GET /2.0.0/users/<me>/relationships/<id> (§A.7).
struct Relationship {
    bool is_friend = false;
    bool is_blocking = false;
    bool is_request_sent = false;
};

/// One item of the Five invitation inbox (invitations doc §2a).
struct Invitation {
    std::uint64_t id = 0;
    std::uint64_t group_id = 0;
    std::uint64_t sender_id = 0;
    std::uint64_t application_id = 0;
    std::uint64_t application_group_id = 0;
    std::uint8_t acd_index = 0;
    std::vector<std::uint8_t> application_data;
    std::int64_t created_at = 0;
    bool read = false;
    bool application_id_match = false;
};

/// GET /v1/invitation_groups/<id> (invitations doc §2b).
struct InvitationGroup {
    std::uint64_t id = 0;
    std::uint64_t sender_id = 0;
    std::vector<std::uint64_t> receivers;
    std::uint64_t application_id = 0;
    std::uint64_t application_group_id = 0;
    std::uint8_t acd_index = 0;
    std::vector<std::string> messages; ///< 16 slots in the module's language order.
    std::vector<std::uint8_t> application_data;
    std::int64_t created_at = 0;
    bool application_id_match = false;
};

} // namespace OpenPak::Baas

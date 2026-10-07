// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later
// Derived from openpak-client 3fd9ba7 (baas_blocks.cpp).

// Blocking and unblocking, as the friends module does it (friends contract §A.8): 30400
// BlockUser, 30401/30403 BlockUserWithApplicationInfo(V2) and 30402 UnblockUser. Ported from
// Ryujinx.OpenPak/OpenPakBaas.Blocks.cs.

#include <chrono>

#include <fmt/format.h>

#include "core/openpak/baas.h"
#include "core/openpak/baas_detail.h"

namespace OpenPak::Baas {

namespace {

using detail::Hex16;

/// The block the guest asked for, in the cache before the request leaves: a title may read the
/// list or the relationship straight after. The sync that follows replaces it.
void AddLocally(std::uint64_t target_id, int reason, const std::optional<Route>& route) {
    const auto blocks = Blocks();
    if (blocks->size() >= 100) {
        return;
    }
    for (const Block& block : *blocks) {
        if (block.id == target_id) {
            return;
        }
    }

    Block added;
    added.id = target_id;
    if (const auto known = FindFriend(target_id)) {
        added.nickname = known->nickname;
        added.thumbnail_url = known->thumbnail_url;
    } else if (const auto user = CachedUser(target_id)) {
        added.nickname = user->nickname;
        added.thumbnail_url = user->thumbnail_url;
    }
    added.created_at = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    added.reason = reason;
    if (route) {
        added.route = *route;
    }

    std::vector<Block> next = *blocks;
    next.push_back(std::move(added));
    detail::ReplaceBlocks(std::move(next));
}

void RemoveLocally(std::uint64_t target_id) {
    std::vector<Block> next = *Blocks();
    std::erase_if(next, [target_id](const Block& block) { return block.id == target_id; });
    detail::ReplaceBlocks(std::move(next));
}

/// Blocks, then the friend list (a block ends a friendship), then the request boxes, which is
/// the order the console re-syncs in after either write.
void AfterWrite() {
    SyncBlockList();
    SyncFriendList(true);
    RefreshFriendRequests();
}

} // namespace

const char* BlockReason(int reason) {
    switch (reason) {
    case 1:
        return "BAD_FRIEND_REQUEST";
    case 2:
        return "BAD_FRIEND";
    case 3:
        return "IN_APP";
    case 4:
        return "IN_CAMPUS";
    default:
        return nullptr;
    }
}

std::string BlockBody(std::uint64_t target_id, const std::string& reason,
                      const std::optional<Route>& route) {
    Json self = Json::object();
    self["reason"] = reason;
    if (route) {
        self["route:appInfo:appId"] = Hex16(route->application_id);
        self["route:appInfo:acdIndex"] = route->acd_index;
        self["route:appInfo:presenceGroupId"] = Hex16(route->presence_group_id);
        self["route:name"] = route->name;
        self["route:name:language"] = route->language;
    }

    Json body = Json::object();
    body["targetUserId"] = Hex16(target_id);
    body["extras"] = Json{{"self", std::move(self)}};
    return detail::Dump(body);
}

int BlockUser(std::uint64_t target_id, int reason, const std::optional<Route>& route) {
    if (!Ready()) {
        return Unknown;
    }

    const char* reason_text = BlockReason(reason);
    if (reason_text == nullptr || target_id == 0) {
        return InvalidArgument;
    }

    AddLocally(target_id, reason, route);

    const Reply reply =
        detail::Send("POST", BaasHost, fmt::format("/1.0.0/users/{}/blocks", UserId()),
                     detail::JsonType, BlockBody(target_id, reason_text, route));

    if (!reply.Success()) {
        // The optimistic entry goes: what the server holds is what is blocked.
        SyncBlockList();

        const int error = BaasError(reply);
        return error == 2031 ? 2213 : error == 2061 ? 2701 : error;
    }

    AfterWrite();
    return Ok;
}

int UnblockUser(std::uint64_t target_id) {
    if (!Ready()) {
        return Unknown;
    }

    RemoveLocally(target_id);

    const Reply reply = detail::Send(
        "DELETE", BaasHost, fmt::format("/1.0.0/users/{}/blocks/{}", UserId(), Hex16(target_id)),
        {}, {});

    if (reply.Success()) {
        AfterWrite();
        return Ok;
    }

    const int error = BaasError(reply);
    if (error == 2031 || error == 3404 || (error >= 2220 && error <= 2229)) {
        return 2711;
    }

    SyncBlockList();
    return error;
}

} // namespace OpenPak::Baas

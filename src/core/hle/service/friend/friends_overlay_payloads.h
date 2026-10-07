// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).
//
// What the friends sysmodule hands the overlay applet (ovln, source "overlay") and what its
// INotificationService queue holds, as plain data so friends_selfcheck.cpp can check the layouts
// without the rest of core. Offsets are from the payload start; the 0x80-byte message adds the
// u32 tag and u32 size in front (Service::PSC::MakeNotification), so "+0x10" here is the
// firmware notes' "+0x18".
//
// Firmware 22.5.0 friends (invite-re ovln-notes.md §2) and 23.0.1 overlayDisp (overlay-notes.md
// §4.1):
//   7001 list summary  0x20: Uid, s32 online friends, s32 requests unread, s32 requests read,
//                            s32 unread invitations
//   7002 friend online 0x20: Uid, u64 friend NSA id, s32 seconds since the friend's previous
//                            presence time (saturated), u32 0
//   7003 friend request 0x10: Uid
//   7004 invitation    0x38: Uid, u64 invitation id, u64 group id, u64 sender NSA id,
//                            u64 application id, s64 created_at (POSIX seconds)

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <type_traits>

namespace Service::Friend::Overlay {

inline constexpr std::uint32_t TagListSummary = 7001;
inline constexpr std::uint32_t TagFriendOnline = 7002;
inline constexpr std::uint32_t TagFriendRequest = 7003;
inline constexpr std::uint32_t TagFriendInvitation = 7004;

/// The module's one sender: queue 16, option 1 (append; a full queue fails the send, which the
/// module ignores).
inline constexpr std::uint32_t QueueLength = 16;
inline constexpr std::uint64_t SendOption = 1;

/// overlayDisp shows "is now online" only for a friend offline longer than this.
inline constexpr std::int32_t OnlinePopupMinimumOffline = 1799;

/// An account Uid exactly as it lies in guest memory.
using UidBytes = std::array<std::uint8_t, 0x10>;

namespace detail {
template <typename T, std::size_t N>
void Put(std::array<std::uint8_t, N>& out, std::size_t offset, T value) {
    static_assert(std::is_trivially_copyable_v<T>);
    std::memcpy(out.data() + offset, &value, sizeof(value));
}
} // namespace detail

inline std::array<std::uint8_t, 0x20> ListSummaryPayload(const UidBytes& user, std::int32_t online,
                                                         std::int32_t requests_unread,
                                                         std::int32_t requests_read,
                                                         std::int32_t invitations_unread) {
    std::array<std::uint8_t, 0x20> out{};
    std::memcpy(out.data(), user.data(), user.size());
    detail::Put(out, 0x10, online);
    detail::Put(out, 0x14, requests_unread);
    detail::Put(out, 0x18, requests_read);
    detail::Put(out, 0x1c, invitations_unread);
    return out;
}

/// `offline_seconds` is now minus the friend's cached presence time; it saturates to s32.
inline std::array<std::uint8_t, 0x20> FriendOnlinePayload(const UidBytes& user,
                                                          std::uint64_t friend_id,
                                                          std::int64_t offline_seconds) {
    std::array<std::uint8_t, 0x20> out{};
    std::memcpy(out.data(), user.data(), user.size());
    detail::Put(out, 0x10, friend_id);
    const std::int64_t clamped =
        std::clamp<std::int64_t>(offline_seconds, std::numeric_limits<std::int32_t>::min(),
                                 std::numeric_limits<std::int32_t>::max());
    detail::Put(out, 0x18, static_cast<std::int32_t>(clamped));
    detail::Put(out, 0x1c, std::uint32_t{0});
    return out;
}

inline std::array<std::uint8_t, 0x10> FriendRequestPayload(const UidBytes& user) {
    std::array<std::uint8_t, 0x10> out{};
    std::memcpy(out.data(), user.data(), user.size());
    return out;
}

inline std::array<std::uint8_t, 0x38> InvitationPayload(const UidBytes& user,
                                                        std::uint64_t invitation_id,
                                                        std::uint64_t group_id,
                                                        std::uint64_t sender_id,
                                                        std::uint64_t application_id,
                                                        std::int64_t created_at) {
    std::array<std::uint8_t, 0x38> out{};
    std::memcpy(out.data(), user.data(), user.size());
    detail::Put(out, 0x10, invitation_id);
    detail::Put(out, 0x18, group_id);
    detail::Put(out, 0x20, sender_id);
    detail::Put(out, 0x28, application_id);
    detail::Put(out, 0x30, created_at);
    return out;
}

/// set:sys AccountNotificationSettings +0x14 (friend presence overlay permission): 3 all friends,
/// 2 favourites only; 0 or no record for the user reads as 3.
inline bool FriendOnlineAllowed(std::uint8_t permission, bool favourite) {
    if (permission == 0) {
        permission = 3;
    }
    return permission == 3 || (permission == 2 && favourite);
}

// ------------------------------------------------------------------------------------------------
// INotificationService's queue (friends 22.5.0 0x192670; ovln-notes.md §2.6)
// ------------------------------------------------------------------------------------------------

enum class NotificationType : std::uint32_t {
    Invalid = 0,
    FriendListUpdate = 1,
    NewFriendRequest = 0x65,
};

struct SizedNotificationInfo {
    NotificationType type;
    std::uint32_t padding;
    std::uint64_t network_service_account_id;
};
static_assert(sizeof(SizedNotificationInfo) == 0x10);

/// One listener's queue. A list update resets the queue to [1] (plus [0x65] when a request is
/// pending) and is deduplicated until popped; a new request is deduplicated until popped and only
/// reaches a port with the viewer bit; an overflow at 100 collapses to the list update.
class NotificationQueue {
public:
    static constexpr std::size_t Capacity = 100;

    explicit NotificationQueue(bool receives_requests_) : receives_requests{receives_requests_} {}

    void FriendListUpdate() {
        if (has_list_update) {
            return;
        }
        Collapse();
    }

    /// False when this port does not take requests (nothing changed, no signal).
    bool NewFriendRequest() {
        if (!receives_requests) {
            return false;
        }
        if (has_new_request) {
            return true;
        }
        if (queue.size() >= Capacity) {
            has_new_request = true;
            Collapse();
            return true;
        }
        queue.push_back({NotificationType::NewFriendRequest, 0, 0});
        has_new_request = true;
        return true;
    }

    /// The next entry, or false when the queue is empty.
    bool Pop(SizedNotificationInfo& out) {
        if (queue.empty()) {
            out = {};
            return false;
        }
        out = queue.front();
        queue.pop_front();
        if (out.type == NotificationType::FriendListUpdate) {
            has_list_update = false;
        } else if (out.type == NotificationType::NewFriendRequest) {
            has_new_request = false;
        }
        return true;
    }

    void Clear() {
        queue.clear();
        has_list_update = false;
        has_new_request = false;
    }

    std::size_t Size() const {
        return queue.size();
    }

private:
    void Collapse() {
        queue.clear();
        queue.push_back({NotificationType::FriendListUpdate, 0, 0});
        has_list_update = true;
        if (has_new_request) {
            queue.push_back({NotificationType::NewFriendRequest, 0, 0});
        }
    }

    bool receives_requests;
    std::deque<SizedNotificationInfo> queue;
    bool has_list_update = false;
    bool has_new_request = false;
};

} // namespace Service::Friend::Overlay

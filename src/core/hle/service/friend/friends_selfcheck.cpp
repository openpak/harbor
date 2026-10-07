// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Self-check for the friends module's overlay payloads and notification queue. Eden's unit tests
// are off in the Harbor build, so this is a standalone program:
//
//   g++ -std=c++23 -I <src> friends_selfcheck.cpp -o friends_selfcheck && ./friends_selfcheck
//
// It prints "friends self-check: OK" and exits 0, or names the failed check and exits 1. The
// expected offsets are the firmware's message offsets (invite-re ovln-notes.md §2.1, which count
// the 8-byte tag/size header) and overlayDisp's payload offsets (overlay-notes.md §4.1).

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/hle/service/friend/friends_overlay_payloads.h"
#include "core/hle/service/psc/ovln/ovln_types.h"

using namespace Service::Friend::Overlay;
using Service::PSC::GetNotificationPayloadSize;
using Service::PSC::GetNotificationTag;
using Service::PSC::MakeNotification;
using Service::PSC::OverlayNotification;

namespace {

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAILED line %d: %s\n", __LINE__, #cond);                         \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)

UidBytes TestUid() {
    UidBytes uid{};
    for (std::size_t i = 0; i < uid.size(); ++i) {
        uid[i] = static_cast<std::uint8_t>(0xA0 + i);
    }
    return uid;
}

template <typename T>
T At(const OverlayNotification& message, std::size_t message_offset) {
    T value{};
    std::memcpy(&value, reinterpret_cast<const std::uint8_t*>(message.data()) + message_offset,
                sizeof(T));
    return value;
}

bool UidAt(const OverlayNotification& message, std::size_t message_offset) {
    const UidBytes uid = TestUid();
    return std::memcmp(reinterpret_cast<const std::uint8_t*>(message.data()) + message_offset,
                       uid.data(), uid.size()) == 0;
}

void Payloads() {
    // 7001: +0x08 Uid; +0x18 online; +0x1C unread requests; +0x20 read requests; +0x24 invitations.
    {
        const auto payload = ListSummaryPayload(TestUid(), 3, 2, 1, 4);
        const auto message = MakeNotification(TagListSummary, payload);
        CHECK(GetNotificationTag(message) == 7001);
        CHECK(GetNotificationPayloadSize(message) == 0x20);
        CHECK(UidAt(message, 0x08));
        CHECK(At<std::int32_t>(message, 0x18) == 3);
        CHECK(At<std::int32_t>(message, 0x1C) == 2);
        CHECK(At<std::int32_t>(message, 0x20) == 1);
        CHECK(At<std::int32_t>(message, 0x24) == 4);
    }
    // 7002: +0x08 Uid; +0x18 u64 friend; +0x20 s32 seconds, saturated; +0x24 u32 0.
    {
        const auto payload = FriendOnlinePayload(TestUid(), 0x1122334455667788ULL, 1800);
        const auto message = MakeNotification(TagFriendOnline, payload);
        CHECK(GetNotificationTag(message) == 7002);
        CHECK(GetNotificationPayloadSize(message) == 0x20);
        CHECK(UidAt(message, 0x08));
        CHECK(At<std::uint64_t>(message, 0x18) == 0x1122334455667788ULL);
        CHECK(At<std::int32_t>(message, 0x20) == 1800);
        CHECK(At<std::uint32_t>(message, 0x24) == 0);
        CHECK(At<std::int32_t>(message, 0x20) > OnlinePopupMinimumOffline);

        const auto huge = FriendOnlinePayload(TestUid(), 1, 0x1'0000'0000LL);
        std::int32_t seconds{};
        std::memcpy(&seconds, huge.data() + 0x18, sizeof(seconds));
        CHECK(seconds == 0x7fffffff);
    }
    // 7003: +0x08 Uid, size 0x10.
    {
        const auto message = MakeNotification(TagFriendRequest, FriendRequestPayload(TestUid()));
        CHECK(GetNotificationTag(message) == 7003);
        CHECK(GetNotificationPayloadSize(message) == 0x10);
        CHECK(UidAt(message, 0x08));
    }
    // 7004: +0x08 Uid; +0x18 invitation; +0x20 group; +0x28 sender; +0x30 application;
    // +0x38 created_at.
    {
        const auto payload = InvitationPayload(TestUid(), 11, 22, 33, 0x0100000000010000ULL,
                                               1'790'000'000LL);
        const auto message = MakeNotification(TagFriendInvitation, payload);
        CHECK(GetNotificationTag(message) == 7004);
        CHECK(GetNotificationPayloadSize(message) == 0x38);
        CHECK(UidAt(message, 0x08));
        CHECK(At<std::uint64_t>(message, 0x18) == 11);
        CHECK(At<std::uint64_t>(message, 0x20) == 22);
        CHECK(At<std::uint64_t>(message, 0x28) == 33);
        CHECK(At<std::uint64_t>(message, 0x30) == 0x0100000000010000ULL);
        CHECK(At<std::int64_t>(message, 0x38) == 1'790'000'000LL);
    }
    // The sender: queue 16, option 1.
    CHECK(QueueLength == 16);
    CHECK(SendOption == 1);
}

void Permission() {
    // AccountNotificationSettings +0x14: 0 (or no record) reads as 3.
    CHECK(FriendOnlineAllowed(0, false));
    CHECK(FriendOnlineAllowed(3, false));
    CHECK(FriendOnlineAllowed(2, true));
    CHECK(!FriendOnlineAllowed(2, false));
    CHECK(!FriendOnlineAllowed(1, true));
}

void Queue() {
    SizedNotificationInfo info{};

    // Empty: Pop fails (the service answers 2121-0015).
    {
        NotificationQueue queue{true};
        CHECK(!queue.Pop(info));
        CHECK(info.type == NotificationType::Invalid);
    }
    // A list update is deduplicated until popped.
    {
        NotificationQueue queue{true};
        queue.FriendListUpdate();
        queue.FriendListUpdate();
        CHECK(queue.Size() == 1);
        CHECK(queue.Pop(info) && info.type == NotificationType::FriendListUpdate);
        CHECK(!queue.Pop(info));
        queue.FriendListUpdate();
        CHECK(queue.Size() == 1);
    }
    // A request, then a list update: [1, 0x65].
    {
        NotificationQueue queue{true};
        CHECK(queue.NewFriendRequest());
        CHECK(queue.NewFriendRequest());
        CHECK(queue.Size() == 1);
        queue.FriendListUpdate();
        CHECK(queue.Size() == 2);
        CHECK(queue.Pop(info) && info.type == NotificationType::FriendListUpdate);
        CHECK(queue.Pop(info) && info.type == NotificationType::NewFriendRequest);
        CHECK(!queue.Pop(info));
    }
    // friend:u and friend:s never get 0x65.
    {
        NotificationQueue queue{false};
        CHECK(!queue.NewFriendRequest());
        CHECK(queue.Size() == 0);
    }
    // Clear empties everything and the flags with it.
    {
        NotificationQueue queue{true};
        queue.FriendListUpdate();
        queue.NewFriendRequest();
        queue.Clear();
        CHECK(queue.Size() == 0);
        queue.FriendListUpdate();
        CHECK(queue.Size() == 1);
    }
    CHECK(sizeof(SizedNotificationInfo) == 0x10);
    CHECK(NotificationQueue::Capacity == 100);
}

} // namespace

int main() {
    Payloads();
    Permission();
    Queue();
    std::printf("friends self-check: OK\n");
    return 0;
}

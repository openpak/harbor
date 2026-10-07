// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// The friend records the viewer commands write, as friends 23.0.1 writes them (Ghidra
// addresses; ELF vaddr = address - 0x100000):
//   20105/20106 GetFriendListForViewer / UpdateFriendInfoForViewer: 0x200 per friend (0x19b990)
//   20108/20109 GetFriendListForViewerV2 / UpdateFriendInfoForViewerV2: 0x220 (0x19ae90, 0x19bb90)
// Both carry the friend's cached presence block as copied raw by 0x1947e0: 0xE8 bytes, not the
// 0xE0 FriendPresenceImpl that 10101/10102 build. qlaunch's HOME Friend List reads 20108, and
// overlayDisp reads the "is now online" nickname from 20109.

#pragma once

#include <cstddef>
#include <cstdint>

#include "core/openpak/friend_structs.h"

namespace Service::Friend {

/// The cached presence block (parser 0x1b0020/0x1b0170). With state 0 only the first 0x20 bytes
/// are kept, and only while updated_at > 0.
struct ViewerPresence {
    std::uint64_t application_id;    // presence.extras.friends.appInfo:appId
    std::uint32_t unknown_0x08;      // never parsed
    std::uint8_t acd_index;          // appInfo:acdIndex
    std::uint8_t pad_0x0d[3];
    std::uint64_t presence_group_id; // appInfo:presenceGroupId
    std::int64_t updated_at;
    std::uint8_t state; // 0 offline, 1 online, 2 playing
    std::uint8_t pf;    // presence.pf; meaning not established, not carried by the cache
    std::uint8_t pad_0x22[6];
    std::uint8_t app_field[::OpenPak::Friends::kAppKeyValueSize];
};
static_assert(sizeof(ViewerPresence) == 0xE8);
static_assert(offsetof(ViewerPresence, acd_index) == 0x0C);
static_assert(offsetof(ViewerPresence, presence_group_id) == 0x10);
static_assert(offsetof(ViewerPresence, updated_at) == 0x18);
static_assert(offsetof(ViewerPresence, state) == 0x20);
static_assert(offsetof(ViewerPresence, pf) == 0x21);
static_assert(offsetof(ViewerPresence, app_field) == 0x28);

/// 20105/20106.
struct FriendForViewerV1 {
    ::OpenPak::Friends::Uid user_id;
    std::uint64_t network_user_id;
    ::OpenPak::Friends::Nickname nickname;
    std::uint8_t pad_0x39[7];
    char thumbnail_url[::OpenPak::Friends::kThumbnailUrlSize];
    ViewerPresence presence;
    std::uint8_t is_favourite;
    std::uint8_t is_new;
    std::uint8_t pad_0x1ca[6];
    std::uint8_t is_valid;
    std::uint8_t pad_0x1d1[0x2F];
};
static_assert(sizeof(FriendForViewerV1) == 0x200);
static_assert(offsetof(FriendForViewerV1, thumbnail_url) == 0x40);
static_assert(offsetof(FriendForViewerV1, presence) == 0xE0);
static_assert(offsetof(FriendForViewerV1, is_favourite) == 0x1C8);
static_assert(offsetof(FriendForViewerV1, is_new) == 0x1C9);
static_assert(offsetof(FriendForViewerV1, is_valid) == 0x1D0);

/// 20108/20109: V1 up to the presence, then the friend note, the flags moved behind it.
struct FriendForViewerV2 {
    ::OpenPak::Friends::Uid user_id;
    std::uint64_t network_user_id;
    ::OpenPak::Friends::Nickname nickname;
    std::uint8_t pad_0x39[7];
    char thumbnail_url[::OpenPak::Friends::kThumbnailUrlSize];
    ViewerPresence presence;
    char friend_note[0x51]; // $.items[*].friendNote
    std::uint8_t is_favourite;
    std::uint8_t is_new;
    std::uint8_t pad_0x21b[4];
    std::uint8_t is_valid;
};
static_assert(sizeof(FriendForViewerV2) == 0x220);
static_assert(offsetof(FriendForViewerV2, presence) == 0xE0);
static_assert(offsetof(FriendForViewerV2, friend_note) == 0x1C8);
static_assert(offsetof(FriendForViewerV2, is_favourite) == 0x219);
static_assert(offsetof(FriendForViewerV2, is_new) == 0x21A);
static_assert(offsetof(FriendForViewerV2, is_valid) == 0x21F);

} // namespace Service::Friend

// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later
// Derived from openpak-client 3fd9ba7 (friend_structs.h).

// The guest's friends structs (friends contract §B.3, presence doc §1-3, invitations doc §2) and
// the builders that fill them from the BAAS data in baas_models.h. Ported from the Ryujinx
// reference: Ryujinx.Horizon/Sdk/Friends/*.cs, Friends/Detail/*.cs, OpenPakFriends.cs and
// OpenPakFriends.Structs.cs.
//
// Nothing here knows about any emulator: every struct is a standard-layout POD of <cstdint>
// fields and char arrays whose bytes are exactly what the guest reads, so the per-emulator IPC
// glue only has to memcpy them into its output buffers. The sizes matter on their own: a struct
// with the wrong size is the wrong element stride, and a guest buffer filled at the wrong stride
// is read as garbage. Every size and every offset the audit pins is a static_assert below.
//
// Text fields are char arrays, NUL-terminated, holding UTF-8. Flags are std::uint8_t 0/1 (the
// module's bools are one byte). Padding is spelled out as named byte arrays so each offset is
// visible in the declaration, and is always zero in what the builders return.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/openpak/baas_models.h"

namespace OpenPak::Friends {

// ------------------------------------------------------------------------------------------------
// Shared fields
// ------------------------------------------------------------------------------------------------

/// An account Uid as the guest passes it: sixteen bytes, `high` at +0 and `low` at +8 (the
/// Ryujinx Uid's field order). The structs carry the *caller's own* Uid, copied verbatim.
struct Uid {
    std::uint64_t high;
    std::uint64_t low;
    friend bool operator==(const Uid&, const Uid&) = default;
};
static_assert(sizeof(Uid) == 0x10);

/// An account nickname: char[0x21], at most 32 UTF-8 bytes plus the terminator.
struct Nickname {
    char name[0x21];
};
static_assert(sizeof(Nickname) == 0x21 && alignof(Nickname) == 1);

/// The friend's presence status as the read side reports it.
enum class PresenceStatus : std::uint32_t {
    Offline = 0,
    Online = 1,
    OnlinePlay = 2,
};

/// A thumbnail URL field: char[0xA0], NUL-terminated.
inline constexpr std::size_t kThumbnailUrlSize = 0xA0;
/// An in-app screen name or route name field: char[0x40].
inline constexpr std::size_t kRouteNameSize = 0x40;
/// A language tag field: char[8].
inline constexpr std::size_t kLanguageSize = 8;
/// A private friend note: char[0x51], at most 20 characters (30130).
inline constexpr std::size_t kFriendNoteSize = 0x51;
/// The presence key/value blob: `key\0value\0` pairs, zero-padded.
inline constexpr std::size_t kAppKeyValueSize = 0xC0;
/// An invitation's application data.
inline constexpr std::size_t kApplicationDataSize = 0x400;
/// One invitation message slot, and how many there are.
inline constexpr std::size_t kMessageSlotSize = 0xC0;
inline constexpr std::size_t kMessageSlots = 16;
/// At most sixteen receivers in an invitation group, as the module's parser accepts.
inline constexpr std::size_t kMaxReceivers = 16;

/// A play-log block is 20 entries of 0x28 (V1) or 0x30 (V2, with acdIndex). The fields inside
/// one entry -- appId, presenceGroupId and the four counters -- have no established offsets in
/// the audit, so the block is carried as bytes and left zero rather than filled with a guess: a
/// guessed layout a game reads as play counts is worse than a block of zeros with the valid flag
/// unset.
inline constexpr std::size_t kPlayLogEntries = 20;
inline constexpr std::size_t kPlayLogBlockSize = kPlayLogEntries * 0x28;
inline constexpr std::size_t kPlayLogBlockSizeV2 = kPlayLogEntries * 0x30;

/// The invitation message slots, in the module's order (invitations doc §2b).
inline constexpr std::string_view kMessageLanguages[kMessageSlots] = {
    "en-US", "en-GB", "ja", "fr", "de", "es-419", "es", "it",
    "nl", "fr-CA", "pt", "ru", "zh-Hans", "zh-Hant", "ko", "pt-BR",
};

// ------------------------------------------------------------------------------------------------
// Small value types
// ------------------------------------------------------------------------------------------------

/// A title and its presence group, as the V1 commands carry it.
struct ApplicationInfo {
    std::uint64_t application_id;
    std::uint64_t presence_group_id;
};
static_assert(sizeof(ApplicationInfo) == 0x10);

/// The title an invitation or a friend request came from, with the acd index (30218 / 30901,
/// invitations doc §2d). The u32 at +8 is never serialised or parsed.
struct ApplicationInfoV2 {
    std::uint64_t application_id;
    std::uint32_t unknown;
    std::uint8_t acd_index;
    std::uint8_t pad_0x0d[3];
    std::uint64_t presence_group_id;
};
static_assert(sizeof(ApplicationInfoV2) == 0x18);
static_assert(offsetof(ApplicationInfoV2, unknown) == 0x08);
static_assert(offsetof(ApplicationInfoV2, acd_index) == 0x0C);
static_assert(offsetof(ApplicationInfoV2, presence_group_id) == 0x10);

/// A 128-bit external application catalog id (friends contract §B.3), written on the wire as
/// `%016llx%016llx`: the high half first.
struct ExternalApplicationCatalogId {
    std::uint64_t high;
    std::uint64_t low;
};
static_assert(sizeof(ExternalApplicationCatalogId) == 0x10);

/// A friend code as the module carries one: char[0x20], NUL-terminated, exactly the characters
/// the server issued (friends contract §A.6 -- no prefix, no normalisation).
struct FriendCode {
    char code[0x20];
};
static_assert(sizeof(FriendCode) == 0x20);

/// The private note a person keeps on one friend (30130, friends contract §A.2): char[0x51], at
/// most 20 characters, in the command's raw data rather than a buffer.
struct FriendNote {
    char note[kFriendNoteSize];
    std::uint8_t pad_0x51[7];
};
static_assert(sizeof(FriendNote) == 0x58);

/// The sixteen per-language invitation messages a game passes (invitations doc §2c):
/// char[16][0xC0], in the module's language order.
struct FriendInvitationGameModeDescription {
    char messages[kMessageSlots][kMessageSlotSize];
};
static_assert(sizeof(FriendInvitationGameModeDescription) == 0xC00);

/// One invitation's id, one invitation group's id, one request's id (invitations doc §2b).
struct FriendInvitationId {
    std::uint64_t id;
};
struct FriendInvitationGroupId {
    std::uint64_t id;
};
struct RequestId {
    std::uint64_t id;
};
static_assert(sizeof(FriendInvitationId) == 8 && sizeof(FriendInvitationGroupId) == 8 &&
              sizeof(RequestId) == 8);

/// An in-app screen name: the UTF-8 name, char[0x40], and the ASCII language tag, char[8].
struct InAppScreenName {
    char name[0x40];
    char language_code[kLanguageSize];
};
static_assert(sizeof(InAppScreenName) == 0x48);
static_assert(offsetof(InAppScreenName, language_code) == 0x40);

/// An NNID Mii image parameter: char[0x10] (friends contract §B.3).
struct MiiImageUrlParam {
    char param[0x10];
};
static_assert(sizeof(MiiImageUrlParam) == 0x10);

/// An NNID Mii name: char[0x20], at most 10 characters (friends contract §B.3).
struct MiiName {
    char name[0x20];
};
static_assert(sizeof(MiiName) == 0x20);

/// What the caller is to another user (20501, friends contract §B.3). +1 is always 0.
struct Relationship {
    std::uint8_t is_friend;
    std::uint8_t unknown_0x01;
    std::uint8_t is_blocking;
    std::uint8_t is_request_sent;
    std::uint8_t pad_0x04[4];
};
static_assert(sizeof(Relationship) == 8);
static_assert(offsetof(Relationship, is_blocking) == 2);
static_assert(offsetof(Relationship, is_request_sent) == 3);

/// A URL field: char[0xA0].
struct Url {
    char path[0xA0];
};
static_assert(sizeof(Url) == 0xA0);

/// A play-history registration key (10700 / 10701).
struct PlayHistoryRegistrationKey {
    std::uint16_t type;
    std::uint8_t key_index;
    std::uint8_t user_id_bool;
    std::uint8_t unknown_bool;
    std::uint8_t reserved[11];
    Uid uuid;
    std::uint8_t hmac_hash[32];
};
static_assert(sizeof(PlayHistoryRegistrationKey) == 0x40);
static_assert(offsetof(PlayHistoryRegistrationKey, uuid) == 0x10);
static_assert(offsetof(PlayHistoryRegistrationKey, hmac_hash) == 0x20);

// Shapes the reference only sizes, carried as bytes so the glue has the right stride.
struct ExternalApplicationCatalog {
    std::uint8_t raw[0x4B8];
};
struct FacedFriendRequestRegistrationKey {
    std::uint8_t raw[0x40];
};
struct NintendoNetworkIdUserInfo {
    std::uint8_t raw[0x38];
};
struct PlayHistoryStatistics {
    std::uint8_t raw[0x10];
};
struct SnsAccountLinkage {
    std::uint8_t raw[0x8];
};
struct SnsAccountProfile {
    std::uint8_t raw[0x380];
};
struct WebPageUrl {
    std::uint8_t raw[0x1000];
};
static_assert(sizeof(ExternalApplicationCatalog) == 0x4B8);
static_assert(sizeof(FacedFriendRequestRegistrationKey) == 0x40);
static_assert(sizeof(NintendoNetworkIdUserInfo) == 0x38);
static_assert(sizeof(PlayHistoryStatistics) == 0x10);
static_assert(sizeof(SnsAccountLinkage) == 0x8);
static_assert(sizeof(SnsAccountProfile) == 0x380);
static_assert(sizeof(WebPageUrl) == 0x1000);

/// Which presence a FriendFilter asks for.
enum class PresenceStatusFilter : std::uint32_t {
    None = 0,
    Online = 1,
    OnlinePlay = 2,
    OnlineOrOnlinePlay = 3,
};

/// The FriendFilter the list commands take (presence doc §3.2).
struct SizedFriendFilter {
    PresenceStatusFilter presence_status;
    std::uint8_t is_favorite_only;
    std::uint8_t is_same_app_presence_only;
    std::uint8_t is_same_app_played_only;
    std::uint8_t is_arbitrary_app_played_only;
    std::uint64_t presence_group_id;
};
static_assert(sizeof(SizedFriendFilter) == 0x10);
static_assert(offsetof(SizedFriendFilter, presence_group_id) == 0x08);

// ------------------------------------------------------------------------------------------------
// Presence
// ------------------------------------------------------------------------------------------------

/// A friend's presence as the guest reads it (FriendImpl +0x40). Not the write-side
/// UserPresenceImpl: the two share a size and the blob offset, and this one starts with the
/// friend's title, not a Uid.
struct FriendPresenceImpl {
    std::uint64_t application_id;
    std::uint64_t presence_group_id;
    std::int64_t last_update_timestamp;
    PresenceStatus status;
    std::uint8_t same_presence_group_application;
    std::uint8_t pad_0x1d[3];
    std::uint8_t app_key_value_storage[kAppKeyValueSize];
};
static_assert(sizeof(FriendPresenceImpl) == 0xE0);
static_assert(offsetof(FriendPresenceImpl, last_update_timestamp) == 0x10);
static_assert(offsetof(FriendPresenceImpl, status) == 0x18);
static_assert(offsetof(FriendPresenceImpl, same_presence_group_application) == 0x1C);
static_assert(offsetof(FriendPresenceImpl, app_key_value_storage) == 0x20);

/// The caller's own presence, as nn::Friends::UserPresence builds it and UpdateUserPresence
/// (10610) receives it.
struct UserPresenceImpl {
    Uid user_id;
    std::uint64_t unused;
    /// 0 none, 1 DeclareOpenOnlinePlaySession, 2 DeclareCloseOnlinePlaySession.
    std::uint8_t online_play_declaration;
    std::uint8_t pad_0x19[7];
    /// key\0value\0 pairs, zero-padded; sys_description is SetDescription's.
    std::uint8_t app_key_value_storage[kAppKeyValueSize];
};
static_assert(sizeof(UserPresenceImpl) == 0xE0);
static_assert(offsetof(UserPresenceImpl, online_play_declaration) == 0x18);
static_assert(offsetof(UserPresenceImpl, app_key_value_storage) == 0x20);

/// The caller's own presence slot as 20600 returns it (friends contract §B.3). The state is the
/// console's own -- 0 INACTIVE, 1 ONLINE, 2 PLAYING -- not a friend's status enum.
struct UserPresenceViewImpl {
    std::uint64_t application_id;
    std::uint64_t presence_group_id;
    std::int64_t last_update_timestamp;
    std::uint32_t state;
    std::uint8_t pad_0x1c[4];
    std::uint8_t app_key_value_storage[kAppKeyValueSize];
};
static_assert(sizeof(UserPresenceViewImpl) == 0xE0);
static_assert(offsetof(UserPresenceViewImpl, state) == 0x18);
static_assert(offsetof(UserPresenceViewImpl, app_key_value_storage) == 0x20);

// ------------------------------------------------------------------------------------------------
// Friends
// ------------------------------------------------------------------------------------------------

/// One friend as 10101 / 10102 return them (presence doc §3.2).
struct FriendImpl {
    Uid user_id;
    std::uint64_t network_user_id;
    Nickname nickname;
    std::uint8_t pad_0x39[7];
    FriendPresenceImpl presence;
    std::uint8_t is_favourite;
    std::uint8_t is_new;
    std::uint8_t unknown_0x122[6];
    std::uint8_t is_valid;
    std::uint8_t pad_0x129[0xD7];
};
static_assert(sizeof(FriendImpl) == 0x200);
static_assert(offsetof(FriendImpl, network_user_id) == 0x10);
static_assert(offsetof(FriendImpl, nickname) == 0x18);
static_assert(offsetof(FriendImpl, presence) == 0x40);
static_assert(offsetof(FriendImpl, is_favourite) == 0x120);
static_assert(offsetof(FriendImpl, is_new) == 0x121);
static_assert(offsetof(FriendImpl, is_valid) == 0x128);

/// A friend's cached presence block as the viewer records carry it, copied raw by friends
/// 23.0.1 0x1947e0 (parser 0x1b0020/0x1b0170; Ghidra addresses, ELF vaddr = address - 0x100000):
/// 0xE8 bytes, not the 0xE0 FriendPresenceImpl that 10101/10102 build. With state 0 only the
/// first 0x20 bytes are kept, and only while updated_at > 0.
struct ViewerPresenceImpl {
    std::uint64_t application_id;    ///< presence.extras.friends.appInfo:appId
    std::uint32_t unknown_0x08;      ///< never parsed
    std::uint8_t acd_index;          ///< appInfo:acdIndex
    std::uint8_t pad_0x0d[3];
    std::uint64_t presence_group_id; ///< appInfo:presenceGroupId
    std::int64_t updated_at;
    std::uint8_t state; ///< 0 offline, 1 online, 2 playing
    std::uint8_t pf;    ///< presence.pf; meaning not established, not carried by the cache
    std::uint8_t pad_0x22[6];
    std::uint8_t app_field[kAppKeyValueSize];
};
static_assert(sizeof(ViewerPresenceImpl) == 0xE8);
static_assert(offsetof(ViewerPresenceImpl, acd_index) == 0x0C);
static_assert(offsetof(ViewerPresenceImpl, presence_group_id) == 0x10);
static_assert(offsetof(ViewerPresenceImpl, updated_at) == 0x18);
static_assert(offsetof(ViewerPresenceImpl, state) == 0x20);
static_assert(offsetof(ViewerPresenceImpl, pf) == 0x21);
static_assert(offsetof(ViewerPresenceImpl, app_field) == 0x28);

/// One friend as a viewer port reads them (20105 / 20106; friends 23.0.1 0x19b990): the thumbnail
/// URL in the record and the cached presence block, its blob never filtered.
struct FriendForViewerImpl {
    Uid user_id;
    std::uint64_t network_user_id;
    Nickname nickname;
    std::uint8_t pad_0x39[7];
    char thumbnail_url[kThumbnailUrlSize];
    ViewerPresenceImpl presence;
    std::uint8_t is_favourite;
    std::uint8_t is_new;
    std::uint8_t pad_0x1ca[6];
    std::uint8_t is_valid;
    std::uint8_t pad_0x1d1[0x2F];
};
static_assert(sizeof(FriendForViewerImpl) == 0x200);
static_assert(offsetof(FriendForViewerImpl, thumbnail_url) == 0x40);
static_assert(offsetof(FriendForViewerImpl, presence) == 0xE0);
static_assert(offsetof(FriendForViewerImpl, is_favourite) == 0x1C8);
static_assert(offsetof(FriendForViewerImpl, is_new) == 0x1C9);
static_assert(offsetof(FriendForViewerImpl, is_valid) == 0x1D0);

/// The same as 20108 / 20109 write it (friends 23.0.1 0x19ae90, 0x19bb90): V1 up to the
/// presence, then the friend note, the flags moved behind it. qlaunch's 23.0.1 Friend List reads
/// 20108; overlayDisp takes the online pop-up's nickname from 20109.
struct FriendForViewerImplV2 {
    Uid user_id;
    std::uint64_t network_user_id;
    Nickname nickname;
    std::uint8_t pad_0x39[7];
    char thumbnail_url[kThumbnailUrlSize];
    ViewerPresenceImpl presence;
    char friend_note[0x51]; ///< $.items[*].friendNote
    std::uint8_t is_favourite;
    std::uint8_t is_new;
    std::uint8_t pad_0x21b[4];
    std::uint8_t is_valid;
};
static_assert(sizeof(FriendForViewerImplV2) == 0x220);
static_assert(offsetof(FriendForViewerImplV2, presence) == 0xE0);
static_assert(offsetof(FriendForViewerImplV2, friend_note) == 0x1C8);
static_assert(offsetof(FriendForViewerImplV2, is_favourite) == 0x219);
static_assert(offsetof(FriendForViewerImplV2, is_new) == 0x21A);
static_assert(offsetof(FriendForViewerImplV2, is_valid) == 0x21F);

/// One friend's profile page, as 20102 and 20107 return it (friends contract §B.3).
///
/// The play-log block at +0xE0 and the route block at +0x4C0 are left zero: the block's stride
/// is known but not the fields inside an entry, and the route block's layout is an inference.
/// Their valid flags stay clear, which is how the module marks "nothing here".
struct FriendDetailedInfoImpl {
    Uid user_id;
    std::uint64_t network_user_id;
    Nickname nickname;
    std::uint8_t pad_0x39[7];
    char thumbnail_url[kThumbnailUrlSize];
    std::uint8_t play_log[kPlayLogBlockSize];
    std::uint8_t is_play_log_valid;
    std::uint8_t pad_0x401[0xB7];
    /// The channels table (§A.1), 1-based; 0 when the server named none.
    std::uint32_t channel;
    std::uint8_t pad_0x4bc[0x64];
    std::uint8_t is_valid;
    std::uint8_t pad_0x521[0x2DF];
};
static_assert(sizeof(FriendDetailedInfoImpl) == 0x800);
static_assert(offsetof(FriendDetailedInfoImpl, thumbnail_url) == 0x40);
static_assert(offsetof(FriendDetailedInfoImpl, play_log) == 0xE0);
static_assert(offsetof(FriendDetailedInfoImpl, is_play_log_valid) == 0x400);
static_assert(offsetof(FriendDetailedInfoImpl, channel) == 0x4B8);
static_assert(offsetof(FriendDetailedInfoImpl, is_valid) == 0x520);

/// The per-friend flags as 20110 returns them (friends contract §B.3).
struct FriendSettingImpl {
    Uid user_id;
    std::uint64_t network_user_id;
    std::uint8_t is_favourite;
    std::uint8_t is_new;
    std::uint8_t is_online_notification;
    std::uint8_t pad_0x1b[0x25];
};
static_assert(sizeof(FriendSettingImpl) == 0x40);
static_assert(offsetof(FriendSettingImpl, is_favourite) == 0x18);
static_assert(offsetof(FriendSettingImpl, is_new) == 0x19);
static_assert(offsetof(FriendSettingImpl, is_online_notification) == 0x1A);

/// The same plus the private note (20111).
struct FriendSettingImplV2 {
    Uid user_id;
    std::uint64_t network_user_id;
    char friend_note[kFriendNoteSize];
    std::uint8_t is_favourite;
    std::uint8_t is_new;
    std::uint8_t is_online_notification;
    std::uint8_t pad_0x6c[0x14];
};
static_assert(sizeof(FriendSettingImplV2) == 0x80);
static_assert(offsetof(FriendSettingImplV2, friend_note) == 0x18);
static_assert(offsetof(FriendSettingImplV2, is_favourite) == 0x69);
static_assert(offsetof(FriendSettingImplV2, is_new) == 0x6A);
static_assert(offsetof(FriendSettingImplV2, is_online_notification) == 0x6B);

// ------------------------------------------------------------------------------------------------
// Users
// ------------------------------------------------------------------------------------------------

/// One user as 10500 returns them (friends contract §B.3): the flat user of
/// `GET /1.0.0/users?filter.id.$in=…`, with no relationship in it.
struct ProfileImpl {
    std::uint64_t network_user_id;
    Nickname nickname;
    std::uint8_t pad_0x29[7];
    char thumbnail_url[kThumbnailUrlSize];
    std::uint8_t is_valid;
    std::uint8_t pad_0xd1[0x2F];
};
static_assert(sizeof(ProfileImpl) == 0x100);
static_assert(offsetof(ProfileImpl, nickname) == 0x08);
static_assert(offsetof(ProfileImpl, thumbnail_url) == 0x30);
static_assert(offsetof(ProfileImpl, is_valid) == 0xD0);

/// A user with their play log, as 20500 and 30500 return them (friends contract §B.3):
/// ProfileImpl plus the 20 x 0x28 play-log block at +0xD0.
struct ProfileExtraImpl {
    std::uint64_t network_user_id;
    Nickname nickname;
    std::uint8_t pad_0x29[7];
    char thumbnail_url[kThumbnailUrlSize];
    std::uint8_t play_log[kPlayLogBlockSize];
    std::uint8_t is_valid;
    std::uint8_t pad_0x3f1[0xF];
};
static_assert(sizeof(ProfileExtraImpl) == 0x400);
static_assert(offsetof(ProfileExtraImpl, thumbnail_url) == 0x30);
static_assert(offsetof(ProfileExtraImpl, play_log) == 0xD0);
static_assert(offsetof(ProfileExtraImpl, is_valid) == 0x3F0);

/// The same with acdIndex in every play-log entry (20502 / 30501): 20 x 0x30.
struct ProfileExtraImplV2 {
    std::uint64_t network_user_id;
    Nickname nickname;
    std::uint8_t pad_0x29[7];
    char thumbnail_url[kThumbnailUrlSize];
    std::uint8_t play_log[kPlayLogBlockSizeV2];
    std::uint8_t is_valid;
    std::uint8_t pad_0x491[0x17];
};
static_assert(sizeof(ProfileExtraImplV2) == 0x4A8);
static_assert(offsetof(ProfileExtraImplV2, play_log) == 0xD0);
static_assert(offsetof(ProfileExtraImplV2, is_valid) == 0x490);

/// One blocked user as 20400 returns them (friends contract §B.3): the block's target, why it
/// was made, and the title it was made from.
struct BlockedUserImpl {
    Uid user_id;
    std::uint64_t network_user_id;
    Nickname nickname;
    std::uint8_t pad_0x39[7];
    char thumbnail_url[kThumbnailUrlSize];
    std::uint32_t reason;
    std::uint8_t pad_0xe4[4];
    std::uint64_t application_id;
    std::uint64_t presence_group_id;
    char route_name[kRouteNameSize];
    char route_language[kLanguageSize];
    std::int64_t created_at;
    std::uint8_t is_valid;
    std::uint8_t pad_0x149[0xB7];
};
static_assert(sizeof(BlockedUserImpl) == 0x200);
static_assert(offsetof(BlockedUserImpl, thumbnail_url) == 0x40);
static_assert(offsetof(BlockedUserImpl, reason) == 0xE0);
static_assert(offsetof(BlockedUserImpl, application_id) == 0xE8);
static_assert(offsetof(BlockedUserImpl, presence_group_id) == 0xF0);
static_assert(offsetof(BlockedUserImpl, route_name) == 0xF8);
static_assert(offsetof(BlockedUserImpl, route_language) == 0x138);
static_assert(offsetof(BlockedUserImpl, created_at) == 0x140);
static_assert(offsetof(BlockedUserImpl, is_valid) == 0x148);

/// The same with the route acdIndex at +0xF4, everything after it shifted by 8 (20402).
struct BlockedUserImplV2 {
    Uid user_id;
    std::uint64_t network_user_id;
    Nickname nickname;
    std::uint8_t pad_0x39[7];
    char thumbnail_url[kThumbnailUrlSize];
    std::uint32_t reason;
    std::uint8_t pad_0xe4[4];
    std::uint64_t application_id;
    std::uint32_t unknown_0xf0;
    std::uint8_t acd_index;
    std::uint8_t pad_0xf5[3];
    std::uint64_t presence_group_id;
    char route_name[kRouteNameSize];
    char route_language[kLanguageSize];
    std::int64_t created_at;
    std::uint8_t is_valid;
    std::uint8_t pad_0x151[0xAF];
};
static_assert(sizeof(BlockedUserImplV2) == 0x200);
static_assert(offsetof(BlockedUserImplV2, application_id) == 0xE8);
static_assert(offsetof(BlockedUserImplV2, acd_index) == 0xF4);
static_assert(offsetof(BlockedUserImplV2, presence_group_id) == 0xF8);
static_assert(offsetof(BlockedUserImplV2, route_name) == 0x100);
static_assert(offsetof(BlockedUserImplV2, route_language) == 0x140);
static_assert(offsetof(BlockedUserImplV2, created_at) == 0x148);
static_assert(offsetof(BlockedUserImplV2, is_valid) == 0x150);

/// The caller's own user as 20800 returns it (friends contract §B.3): the privacy switches, the
/// friend code and the profile behind `GET /1.0.0/users/<me>`.
struct UserSettingImpl {
    Uid user_id;
    /// 0 SELF, 1 FAVORITE_FRIENDS, 2 FRIENDS.
    std::uint32_t presence_permission;
    /// 1 self, 2 favoriteFriends, 3 friends, 5 everyone.
    std::uint32_t play_log_permission;
    std::uint8_t friend_request_reception;
    std::uint8_t pad_0x19[7];
    FriendCode friend_code;
    std::int64_t friend_code_regenerable_at;
    std::uint64_t network_user_id;
    Nickname nickname;
    std::uint8_t pad_0x71[7];
    char thumbnail_url[kThumbnailUrlSize];
    std::uint8_t play_log[kPlayLogBlockSize];
    std::uint8_t is_valid;
    std::uint8_t pad_0x439[0x3C7];
};
static_assert(sizeof(UserSettingImpl) == 0x800);
static_assert(offsetof(UserSettingImpl, presence_permission) == 0x10);
static_assert(offsetof(UserSettingImpl, play_log_permission) == 0x14);
static_assert(offsetof(UserSettingImpl, friend_request_reception) == 0x18);
static_assert(offsetof(UserSettingImpl, friend_code) == 0x20);
static_assert(offsetof(UserSettingImpl, friend_code_regenerable_at) == 0x40);
static_assert(offsetof(UserSettingImpl, network_user_id) == 0x48);
static_assert(offsetof(UserSettingImpl, nickname) == 0x50);
static_assert(offsetof(UserSettingImpl, thumbnail_url) == 0x78);
static_assert(offsetof(UserSettingImpl, play_log) == 0x118);
static_assert(offsetof(UserSettingImpl, is_valid) == 0x438);

/// The same through +0x118, with acdIndex in every play-log entry (20802): 20 x 0x30, so the
/// valid flag lands at +0x4D8. The buffer is still 0x800.
struct UserSettingImplV2 {
    Uid user_id;
    std::uint32_t presence_permission;
    std::uint32_t play_log_permission;
    std::uint8_t friend_request_reception;
    std::uint8_t pad_0x19[7];
    FriendCode friend_code;
    std::int64_t friend_code_regenerable_at;
    std::uint64_t network_user_id;
    Nickname nickname;
    std::uint8_t pad_0x71[7];
    char thumbnail_url[kThumbnailUrlSize];
    std::uint8_t play_log[kPlayLogBlockSizeV2];
    std::uint8_t is_valid;
    std::uint8_t pad_0x4d9[0x327];
};
static_assert(sizeof(UserSettingImplV2) == 0x800);
static_assert(offsetof(UserSettingImplV2, thumbnail_url) == 0x78);
static_assert(offsetof(UserSettingImplV2, play_log) == 0x118);
static_assert(offsetof(UserSettingImplV2, is_valid) == 0x4D8);

// ------------------------------------------------------------------------------------------------
// Friend requests
// ------------------------------------------------------------------------------------------------

/// The route union of a friend request: the external catalog id (0x10), or the NNID Mii name
/// (0x20) and image parameter (0x10) at +0x20. The two variants never mix on the wire -- each
/// send variant writes exactly one.
struct FriendRequestNnidRoute {
    char mii_name[0x20];
    char mii_image_url_param[0x10];
};
union FriendRequestRouteUnion {
    std::uint8_t catalog_id[0x10];
    FriendRequestNnidRoute nnid;
    std::uint8_t raw[0xA0];
};
union FriendRequestRouteUnionV2 {
    std::uint8_t catalog_id[0x10];
    FriendRequestNnidRoute nnid;
    std::uint8_t raw[0x98];
};
static_assert(sizeof(FriendRequestRouteUnion) == 0xA0);
static_assert(sizeof(FriendRequestRouteUnionV2) == 0x98);
static_assert(offsetof(FriendRequestNnidRoute, mii_image_url_param) == 0x20);

/// One friend request as the guest reads it (20201, friends contract §B.3): the V1 shape, with
/// the V1 route ApplicationInfo (`appId, groupId`) at +0xF8.
struct FriendRequestImpl {
    Uid user_id;
    std::uint64_t request_id;
    std::uint64_t other_user_id;
    Nickname nickname;
    std::uint8_t pad_0x41[7];
    char thumbnail_url[kThumbnailUrlSize];
    std::uint32_t list_type;
    std::uint32_t channel;
    std::uint32_t state;
    std::uint8_t pad_0xf4[4];
    std::uint64_t route_application_id;
    std::uint64_t route_presence_group_id;
    char route_name[kRouteNameSize];
    char route_language[kLanguageSize];
    std::int64_t created_at;
    std::uint8_t is_read;
    std::uint8_t is_valid;
    std::uint8_t pad_0x15a[6];
    FriendRequestRouteUnion route;
};
static_assert(sizeof(FriendRequestImpl) == 0x200);
static_assert(offsetof(FriendRequestImpl, request_id) == 0x010);
static_assert(offsetof(FriendRequestImpl, other_user_id) == 0x018);
static_assert(offsetof(FriendRequestImpl, nickname) == 0x020);
static_assert(offsetof(FriendRequestImpl, thumbnail_url) == 0x048);
static_assert(offsetof(FriendRequestImpl, list_type) == 0x0E8);
static_assert(offsetof(FriendRequestImpl, channel) == 0x0EC);
static_assert(offsetof(FriendRequestImpl, state) == 0x0F0);
static_assert(offsetof(FriendRequestImpl, route_application_id) == 0x0F8);
static_assert(offsetof(FriendRequestImpl, route_presence_group_id) == 0x100);
static_assert(offsetof(FriendRequestImpl, route_name) == 0x108);
static_assert(offsetof(FriendRequestImpl, route_language) == 0x148);
static_assert(offsetof(FriendRequestImpl, created_at) == 0x150);
static_assert(offsetof(FriendRequestImpl, is_read) == 0x158);
static_assert(offsetof(FriendRequestImpl, is_valid) == 0x159);
static_assert(offsetof(FriendRequestImpl, route) == 0x160);

/// One friend request as the guest reads it (20202, friends contract §B.3): the V2 shape, with
/// the V2 route ApplicationInfo (`appId`, `acdIndex` at +0xC, `groupId`) at +0xF8 and
/// everything after it shifted by 8.
struct FriendRequestImplV2 {
    Uid user_id;
    std::uint64_t request_id;
    std::uint64_t other_user_id;
    Nickname nickname;
    std::uint8_t pad_0x41[7];
    char thumbnail_url[kThumbnailUrlSize];
    std::uint32_t list_type;
    std::uint32_t channel;
    std::uint32_t state;
    std::uint8_t pad_0xf4[4];
    std::uint64_t route_application_id;
    std::uint32_t route_unknown;
    std::uint8_t route_acd_index;
    std::uint8_t pad_0x105[3];
    std::uint64_t route_presence_group_id;
    char route_name[kRouteNameSize];
    char route_language[kLanguageSize];
    std::int64_t created_at;
    std::uint8_t is_read;
    std::uint8_t is_valid;
    std::uint8_t pad_0x162[6];
    FriendRequestRouteUnionV2 route;
};
static_assert(sizeof(FriendRequestImplV2) == 0x200);
static_assert(offsetof(FriendRequestImplV2, route_application_id) == 0x0F8);
static_assert(offsetof(FriendRequestImplV2, route_unknown) == 0x100);
static_assert(offsetof(FriendRequestImplV2, route_acd_index) == 0x104);
static_assert(offsetof(FriendRequestImplV2, route_presence_group_id) == 0x108);
static_assert(offsetof(FriendRequestImplV2, route_name) == 0x110);
static_assert(offsetof(FriendRequestImplV2, route_language) == 0x150);
static_assert(offsetof(FriendRequestImplV2, created_at) == 0x158);
static_assert(offsetof(FriendRequestImplV2, is_read) == 0x160);
static_assert(offsetof(FriendRequestImplV2, is_valid) == 0x161);
static_assert(offsetof(FriendRequestImplV2, route) == 0x168);

// ------------------------------------------------------------------------------------------------
// Invitations
// ------------------------------------------------------------------------------------------------

/// One waiting invitation as 22000 returns it (invitations doc §2a). The application data is
/// the raw bytes the sender passed, already base64-decoded.
struct FriendInvitationForViewerImpl {
    std::uint64_t invitation_id;
    std::uint64_t group_id;
    std::uint64_t sender_id;
    std::uint64_t application_id;
    std::uint64_t application_group_id;
    std::uint32_t application_data_size;
    std::uint8_t pad_0x2c[4];
    std::int64_t created_at;
    std::uint8_t is_read;
    std::uint8_t application_id_match;
    std::uint8_t is_valid;
    std::uint8_t pad_0x3b[0xA5];
    std::uint8_t application_data[kApplicationDataSize];
    std::uint8_t pad_0x4e0[0x20];
};
static_assert(sizeof(FriendInvitationForViewerImpl) == 0x500);
static_assert(offsetof(FriendInvitationForViewerImpl, application_group_id) == 0x20);
static_assert(offsetof(FriendInvitationForViewerImpl, application_data_size) == 0x28);
static_assert(offsetof(FriendInvitationForViewerImpl, created_at) == 0x30);
static_assert(offsetof(FriendInvitationForViewerImpl, is_read) == 0x38);
static_assert(offsetof(FriendInvitationForViewerImpl, application_id_match) == 0x39);
static_assert(offsetof(FriendInvitationForViewerImpl, is_valid) == 0x3A);
static_assert(offsetof(FriendInvitationForViewerImpl, application_data) == 0xE0);

/// The same with the acd index, everything after it shifted by 8 (22002).
struct FriendInvitationForViewerImplV2 {
    std::uint64_t invitation_id;
    std::uint64_t group_id;
    std::uint64_t sender_id;
    std::uint64_t application_id;
    std::uint32_t unknown;
    std::uint8_t acd_index;
    std::uint8_t pad_0x25[3];
    std::uint64_t application_group_id;
    std::uint32_t application_data_size;
    std::uint8_t pad_0x34[4];
    std::int64_t created_at;
    std::uint8_t is_read;
    std::uint8_t application_id_match;
    std::uint8_t is_valid;
    std::uint8_t pad_0x43[0x9D];
    std::uint8_t application_data[kApplicationDataSize];
    std::uint8_t pad_0x4e0[0x20];
};
static_assert(sizeof(FriendInvitationForViewerImplV2) == 0x500);
static_assert(offsetof(FriendInvitationForViewerImplV2, unknown) == 0x20);
static_assert(offsetof(FriendInvitationForViewerImplV2, acd_index) == 0x24);
static_assert(offsetof(FriendInvitationForViewerImplV2, application_group_id) == 0x28);
static_assert(offsetof(FriendInvitationForViewerImplV2, application_data_size) == 0x30);
static_assert(offsetof(FriendInvitationForViewerImplV2, created_at) == 0x38);
static_assert(offsetof(FriendInvitationForViewerImplV2, is_read) == 0x40);
static_assert(offsetof(FriendInvitationForViewerImplV2, application_id_match) == 0x41);
static_assert(offsetof(FriendInvitationForViewerImplV2, is_valid) == 0x42);
static_assert(offsetof(FriendInvitationForViewerImplV2, application_data) == 0xE0);

/// One invitation group in full, as 22001 returns it (invitations doc §2b): who it went to, the
/// title it is for, the sixteen language message slots and the application data. The message
/// slots are in the module's language order, kMessageLanguages.
struct FriendInvitationGroupImpl {
    std::uint64_t group_id;
    std::uint64_t sender_id;
    std::uint64_t receivers[kMaxReceivers];
    std::int32_t receiver_count;
    std::uint8_t pad_0x94[4];
    std::uint64_t application_id;
    std::uint64_t application_group_id;
    char messages[kMessageSlots][kMessageSlotSize];
    std::uint32_t application_data_size;
    std::uint8_t pad_0xcac[4];
    std::int64_t created_at;
    std::uint8_t application_id_match;
    std::uint8_t is_valid;
    std::uint8_t pad_0xcba[0xA6];
    std::uint8_t application_data[kApplicationDataSize];
    std::uint8_t pad_0x1160[0x2A0];
};
static_assert(sizeof(FriendInvitationGroupImpl) == 0x1400);
static_assert(offsetof(FriendInvitationGroupImpl, receivers) == 0x10);
static_assert(offsetof(FriendInvitationGroupImpl, receiver_count) == 0x90);
static_assert(offsetof(FriendInvitationGroupImpl, application_id) == 0x98);
static_assert(offsetof(FriendInvitationGroupImpl, application_group_id) == 0xA0);
static_assert(offsetof(FriendInvitationGroupImpl, messages) == 0xA8);
static_assert(offsetof(FriendInvitationGroupImpl, application_data_size) == 0xCA8);
static_assert(offsetof(FriendInvitationGroupImpl, created_at) == 0xCB0);
static_assert(offsetof(FriendInvitationGroupImpl, application_id_match) == 0xCB8);
static_assert(offsetof(FriendInvitationGroupImpl, is_valid) == 0xCB9);
static_assert(offsetof(FriendInvitationGroupImpl, application_data) == 0xD60);

/// The same with the acd index at +0xA4, the tail after it shifted by 8 (22003).
struct FriendInvitationGroupImplV2 {
    std::uint64_t group_id;
    std::uint64_t sender_id;
    std::uint64_t receivers[kMaxReceivers];
    std::int32_t receiver_count;
    std::uint8_t pad_0x94[4];
    std::uint64_t application_id;
    std::uint32_t unknown;
    std::uint8_t acd_index;
    std::uint8_t pad_0xa5[3];
    std::uint64_t application_group_id;
    char messages[kMessageSlots][kMessageSlotSize];
    std::uint32_t application_data_size;
    std::uint8_t pad_0xcb4[4];
    std::int64_t created_at;
    std::uint8_t application_id_match;
    std::uint8_t is_valid;
    std::uint8_t pad_0xcc2[0x9E];
    std::uint8_t application_data[kApplicationDataSize];
    std::uint8_t pad_0x1160[0x2A0];
};
static_assert(sizeof(FriendInvitationGroupImplV2) == 0x1400);
static_assert(offsetof(FriendInvitationGroupImplV2, unknown) == 0xA0);
static_assert(offsetof(FriendInvitationGroupImplV2, acd_index) == 0xA4);
static_assert(offsetof(FriendInvitationGroupImplV2, application_group_id) == 0xA8);
static_assert(offsetof(FriendInvitationGroupImplV2, messages) == 0xB0);
static_assert(offsetof(FriendInvitationGroupImplV2, application_data_size) == 0xCB0);
static_assert(offsetof(FriendInvitationGroupImplV2, created_at) == 0xCB8);
static_assert(offsetof(FriendInvitationGroupImplV2, application_id_match) == 0xCC0);
static_assert(offsetof(FriendInvitationGroupImplV2, is_valid) == 0xCC1);
static_assert(offsetof(FriendInvitationGroupImplV2, application_data) == 0xD60);

// ------------------------------------------------------------------------------------------------
// Text helpers
// ------------------------------------------------------------------------------------------------

/// How many of `encoded`'s bytes fit in `limit` without cutting a character in half. A cut
/// lands mid-sequence when the first byte left out is a continuation byte, and only then is
/// anything backed off: a name that fits whole keeps its last character, multi-byte or not.
std::size_t Utf8Fits(std::string_view encoded, std::size_t limit);

/// A string into a fixed char field, truncated on a character boundary with room left for the
/// terminator -- as the module keeps at most N-1 bytes of an N-byte field. The field is zeroed
/// first; a field shorter than 2 bytes stays empty.
void FixedUtf8(std::span<char> destination, std::string_view value);

/// A display name into the fixed 33-byte field, truncated on a character boundary: 32 bytes
/// plus the terminator. A name cut mid-sequence would decode to a replacement character on the
/// console rather than to a shorter name.
Nickname ToNickname(std::string_view name);

/// A fixed NUL-terminated char field as the string it holds (the whole field when it holds no
/// NUL). Malformed UTF-8 is replaced by U+FFFD, as a .NET UTF-8 GetString does, so the result is
/// always valid UTF-8 and safe to put in a JSON body.
std::string Text(std::span<const char> field);
std::string Text(std::span<const std::uint8_t> field);

/// An in-app screen name as a request route carries it: the UTF-8 name and the ASCII language
/// tag.
std::pair<std::string, std::string> ScreenName(const InAppScreenName& screen_name);

/// A 128-bit external catalog id (`%016llx%016llx`) as its two halves, for the route union.
/// False when it is not 32 characters or a half does not read as hex.
bool TryCatalogId(std::string_view catalog_id, std::uint64_t& high, std::uint64_t& low);

// ------------------------------------------------------------------------------------------------
// The appField <-> 0xC0 key/value blob
// ------------------------------------------------------------------------------------------------

/// A blob key: non-empty, ASCII letters, digits and '_' only.
bool ValidAppFieldKey(std::string_view key);

/// The 0xC0 key\0value\0 blob as the JSON object the module publishes for appField: "{}" when
/// empty, and "{}" when it does not validate (a key that is not [A-Za-z0-9_]+, a repeated key,
/// a value that is not strict UTF-8, or a pair without its terminator) -- the module sends ""
/// there, but an empty appField makes a friend's presence_updated parse fail, and "{}" never
/// does. Keys keep their blob order; values are written as raw UTF-8 rather than \u escapes, as
/// the console writes them.
std::string BlobToAppField(std::span<const std::uint8_t> blob);

/// A published appField back into the blob. It must be a JSON object (a string that starts
/// with '{' and ends with '}'); anything else is skipped, as the module's list parser skips it.
/// Only top-level string members with a valid key are kept, in document order; pairs that do not
/// fit are left out (and nothing after the first that does not). The blob is zeroed first, and
/// stays zero when the text does not parse.
void AppFieldToBlob(std::string_view app_field, std::span<std::uint8_t> blob);

/// A JSON string literal, quotes included, with the escaping BlobToAppField uses.
std::string JsonString(std::string_view value);

// ------------------------------------------------------------------------------------------------
// Builders
//
// The Uid a builder takes is the caller's own, copied into +0; the friend is named by the
// network user id. The request id and the other party's id are the BAAS ids the server speaks,
// so a cancel/accept/reject names the same request the box listed. `is_valid` is what the
// sysmodule's parser counts, so a struct written without it is one the console will not show;
// every builder sets it.
// ------------------------------------------------------------------------------------------------

/// Whether that presence group appears in the friend's play log (§A.1). 0 never does.
bool Played(const Baas::Friend& friend_, std::uint64_t presence_group_id);

/// The FriendFilter, as the module applies it (presence doc §3.2). `own_presence_group_id` is
/// the caller's own running title's group (PresenceState::Application().second).
///
/// Online is 1 alone, OnlinePlay is 2 alone: a friend in a title without a declared session is
/// only ever 1, and a game asking for 2 must not be told otherwise. The same-app filters compare
/// against the caller's own presence group, not against anything in the filter; only the
/// arbitrary-app one names a group of its own.
bool Matches(const Baas::Friend& friend_, const SizedFriendFilter& filter,
             std::uint64_t own_presence_group_id);

/// The friends that pass `filter`, in the server's own order so that paging by `offset` is
/// stable between calls.
std::vector<Baas::Friend> Filtered(const std::vector<Baas::Friend>& friends,
                                   const SizedFriendFilter& filter, int offset,
                                   std::uint64_t own_presence_group_id);

/// One friend as the guest's struct (10101 / 10102; contract §B.3, presence doc §3.2).
///
/// A friend whose status is 0 carries only the time -- and the title, when that time is known --
/// and never the blob. The blob is zeroed for a caller outside the friend's presence group
/// unless the port carries the viewer bit (`viewer`), which friend:u and friend:s do not.
FriendImpl ToFriendImpl(const Baas::Friend& friend_, const Uid& user_id,
                        std::uint64_t own_presence_group_id, bool viewer);

/// One friend as a viewer port reads them (20105 / 20106, and 20108 / 20109 with the note): the
/// thumbnail URL is in the record, and the presence blob is never filtered -- a viewer is trusted
/// with it.
FriendForViewerImpl ToFriendForViewerImpl(const Baas::Friend& friend_, const Uid& user_id,
                                          std::uint64_t own_presence_group_id);
FriendForViewerImplV2 ToFriendForViewerImplV2(const Baas::Friend& friend_, const Uid& user_id,
                                              std::uint64_t own_presence_group_id);

/// One friend's profile page (20102 / 20107).
FriendDetailedInfoImpl ToDetailedInfoImpl(const Baas::Friend& friend_, const Uid& user_id);

/// The per-friend flags (20110), and the same plus the private note (20111).
FriendSettingImpl ToFriendSettingImpl(const Baas::Friend& friend_, const Uid& user_id);
FriendSettingImplV2 ToFriendSettingImplV2(const Baas::Friend& friend_, const Uid& user_id);

/// One flat user as 10500 returns them; with a (zero) play-log block (20500 / 30500); and with
/// the wider play-log entries (20502 / 30501).
ProfileImpl ToProfileImpl(const Baas::User& user);
ProfileExtraImpl ToProfileExtraImpl(const Baas::User& user);
ProfileExtraImplV2 ToProfileExtraImplV2(const Baas::User& user);

/// One blocked user as 20400 returns them, and the same with the route acd index (20402).
BlockedUserImpl ToBlockedUserImpl(const Baas::Block& block, const Uid& user_id);
BlockedUserImplV2 ToBlockedUserImplV2(const Baas::Block& block, const Uid& user_id);

/// The caller's own user as 20800 returns it, and with the wider play-log entries (20802).
UserSettingImpl ToUserSettingImpl(const Baas::UserSetting& setting, const Uid& user_id);
UserSettingImplV2 ToUserSettingImplV2(const Baas::UserSetting& setting, const Uid& user_id);

/// One friend request as the guest's V1 struct (20201) and V2 struct (20202, the route carries
/// acdIndex). `list_type` is the box it came from: 1 sent, 2 received.
///
/// The route union holds the external catalog id when the request carries one, else the NNID
/// Mii data when it carries that, else zeros.
FriendRequestImpl ToRequestImpl(const Baas::Request& request, const Uid& user_id, int list_type);
FriendRequestImplV2 ToRequestImplV2(const Baas::Request& request, const Uid& user_id,
                                    int list_type);

/// One waiting invitation as 22000 returns it, and with the acd index (22002). At most 0x400
/// bytes of application data are kept.
FriendInvitationForViewerImpl ToInvitationImpl(const Baas::Invitation& invitation);
FriendInvitationForViewerImplV2 ToInvitationImplV2(const Baas::Invitation& invitation);

/// One invitation group in full, as 22001 returns it, and with the acd index (22003). At most
/// sixteen receivers and sixteen message slots are kept.
FriendInvitationGroupImpl ToInvitationGroupImpl(const Baas::InvitationGroup& group);
FriendInvitationGroupImplV2 ToInvitationGroupImplV2(const Baas::InvitationGroup& group);

/// The message slots a game filled, as the send body carries them (invitations doc §3c): the
/// module's language key and the text, and only for the slots that hold one.
std::vector<std::pair<std::string, std::string>> InvitationMessages(
    const FriendInvitationGameModeDescription& description);

/// The caller's own presence slot (20600) from the running title and what it declared: PLAYING
/// (2) while a title has declared an online-play session, ONLINE (1) while one runs without, and
/// INACTIVE (0) when none does. The blob is filled only while a title runs.
UserPresenceViewImpl ToUserPresenceViewImpl(std::uint64_t application_id,
                                            std::uint64_t presence_group_id,
                                            std::int64_t updated_at, bool session_open,
                                            std::string_view app_field);

} // namespace OpenPak::Friends

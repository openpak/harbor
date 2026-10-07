// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: GPL-2.0-or-later

// Derived from the old OpenPak Eden fork's friend/openpak_friends.cpp (ddbcea330f and its
// follow-ups), moved onto Harbor's core/openpak layer, with Harbor's additions (10110 from the
// picture cache, 10502/10503, 20900, the pushed 22010 count) and every id taken out of the log.
// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base): FriendService.cs.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <vector>

#include "common/logging.h"
#include "core/core.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_readable_event.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/friend/friend_pictures.h"
#include "core/hle/service/friend/friend_service.h"
#include "core/hle/service/friend/friends_overlay_payloads.h"
#include "core/hle/service/friend/friends_session.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/service.h"
#include "core/openpak/async_worker.h"
#include "core/openpak/baas.h"
#include "core/openpak/friend_structs.h"
#include "core/openpak/platform.h"
#include "core/openpak/presence_state.h"
#include "core/openpak/session.h"

namespace Service::Friend {

namespace {

namespace Baas = ::OpenPak::Baas;
namespace Fr = ::OpenPak::Friends;
namespace Session = ::OpenPak::Session;

using Fr::PresenceState;
using Fr::Uid;

/// A NetworkServiceAccountId: the friend's BAAS user id, which is what every id the guest's
/// friend list serves is, and what requests, invitations and relationships are addressed by.
using NetworkServiceAccountId = u64;

/// 2121-0002, the module's bad argument.
constexpr Result ResultInvalidArgument{ErrorModule::Friends, 2};

/// 2121-0015: Pop on an empty notification queue.
constexpr Result ResultNotificationQueueEmpty{ErrorModule::Friends, 15};

/// 2121-0090: this port may not call that command (friends contract §B.1).
constexpr Result ResultPermissionDenied{ErrorModule::Friends, 90};

/// A friends-module description (baas.h) as a Result; 0 is success.
Result FromDescription(int description) {
    return description == 0 ? ResultSuccess
                            : Result{ErrorModule::Friends, static_cast<u32>(description)};
}

/// The bits a port carries (FriendsServicePermissionLevel in the reference).
enum PermissionMask : u32 {
    UserMask = 1,
    ViewerMask = 2,
    ManagerMask = 4,
    SystemMask = 8,
};

u32 PermissionOf(std::string_view port_name) {
    if (port_name == "friend:a") {
        return ~0U;
    }
    if (port_name == "friend:m") {
        return UserMask | ViewerMask | ManagerMask;
    }
    if (port_name == "friend:v") {
        return UserMask | ViewerMask;
    }
    if (port_name == "friend:s") {
        return UserMask | SystemMask;
    }
    return UserMask;
}

bool IsNull(const Uid& user) {
    return user.high == 0 && user.low == 0;
}

Common::UUID ToUuid(const Uid& user) {
    Common::UUID uuid{};
    static_assert(sizeof(uuid) == sizeof(user));
    std::memcpy(&uuid, &user, sizeof(user));
    return uuid;
}

/// The key OpenPak names users by: the Uid's RawString, as acc's profile source gives it.
std::string KeyOf(const Uid& user) {
    return ToUuid(user).RawString();
}

/// Whether OpenPak serves this user's friends: the user acc has open, signed in on the console
/// server and linked to a Nintendo Account. The friend graph the guest sees is the server's
/// (contract §A.1) and nothing else; any other user a title names is offline, never handed the
/// open one's friends.
bool AvailableFor(const Uid& user) {
    if (IsNull(user) || !Session::Enabled() || !Baas::Ready()) {
        return false;
    }
    const std::string key = KeyOf(user);
    return key == ::OpenPak::Platform::ProfileId() && Session::Linked(key);
}

/// The caller's own presence group: the running title's NACP group, which is what the module
/// compares a friend's against. 0 when nothing runs.
u64 OwnPresenceGroupId() {
    return PresenceState::Instance().OwnPresenceGroupId();
}

/// The friends that pass `filter`, in the server's own order so that paging by `offset` is
/// stable between calls.
std::vector<Baas::Friend> Filtered(const Fr::SizedFriendFilter& filter, s32 offset) {
    return Fr::Filtered(*Baas::Friends(), filter, offset, OwnPresenceGroupId());
}

/// One user for the profile commands: a friend out of the list cache, else whatever the user
/// cache has. Both carry the same three fields the flat user shape requires.
std::optional<Baas::User> User(u64 id) {
    if (const auto found = Baas::FindFriend(id)) {
        return Baas::User{found->id, found->nickname, found->thumbnail_url, found->play_log};
    }
    if (const auto cached = Baas::CachedUser(id)) {
        return cached;
    }
    // Last resort for the local player, who is never their own friend: their own user as of the
    // last sync. Answering an invalid slot for yourself is what a game notices first.
    if (const auto own = Baas::OwnUserSetting(); own && own->id == id) {
        return Baas::User{own->id, own->nickname, own->thumbnail_url, {}};
    }
    return std::nullopt;
}

/// Ask for the ids nothing has cached yet, off this thread (§A.7). The call that asked is
/// answered with what is already there; the next one gets the rest.
void Warm(std::span<const NetworkServiceAccountId> ids) {
    std::vector<u64> wanted;
    for (const u64 id : ids) {
        if (!User(id)) {
            wanted.push_back(id);
        }
    }

    if (!wanted.empty()) {
        Baas::RunInBackground([wanted = std::move(wanted)] { Baas::WarmUsers(wanted); });
    }
}

/// The IPC channel number as the channel string the POST carries. The table is the module's
/// 1-based one (contract §A.1); anything else goes out as FRIEND_CODE rather than failing a
/// request the person asked to send.
std::string RequestChannel(s32 channel) {
    if (channel >= 1 && channel <= static_cast<s32>(Baas::Channels.size())) {
        return Baas::Channels[static_cast<std::size_t>(channel - 1)];
    }

    LOG_WARNING(Service_Friend, "[OpenPak] Unknown friend-request channel {}; sending as FRIEND_CODE",
                channel);
    return "FRIEND_CODE";
}

/// Fire and forget: a request's POST is a network call and the caller is the game's thread.
void SendFriendRequestInBackground(Baas::FriendRequestSend send) {
    Baas::RunInBackground([send = std::move(send)] { Baas::SendFriendRequest(send); });
}

/// One friend is no longer "newly" (§A.2): the flag is cleared locally at once -- the module does
/// that before it sends anything -- and the PATCH replaces the cached entry with the relationship
/// the server answers with.
void DropNewly(u64 friend_id) {
    Baas::Update(friend_id, [](Baas::Friend& one) { one.is_newly = false; });
    Baas::RunInBackground([friend_id] {
        Baas::PatchFriend(friend_id, "add", "/extras/self/isConfirmed", true);
    });
}

/// What Eden's LoadUserSetting stub always answered, for a user OpenPak does not speak for.
Fr::UserSettingImpl OfflineUserSetting(const Uid& user) {
    Fr::UserSettingImpl setting{};
    setting.user_id = user;
    setting.presence_permission = 2;
    setting.play_log_permission = 5;
    setting.friend_request_reception = 1;
    setting.friend_code_regenerable_at = 99999999999;
    std::strcpy(setting.friend_code.code, "0000-0000-0000");
    return setting;
}

// ------------------------------------------------------------------------------------------------
// INotificationService
// ------------------------------------------------------------------------------------------------

/// One listener of the friends module (friends 22.5.0 0x192670). The queue is per user and per
/// port: a new friend request only reaches a port with the viewer bit. Changes arrive on the
/// session's signalling thread; the guest's commands on its IPC thread.
class NotificationService final : public ServiceFramework<NotificationService>,
                                  public NotificationListener {
public:
    explicit NotificationService(Core::System& system_, std::shared_ptr<FriendsSession> session_,
                                 const Common::UUID& user, u32 permission)
        : ServiceFramework{system_, "INotificationService"}, session{std::move(session_)},
          user_key{user.RawString()}, queue{(permission & ViewerMask) != 0},
          service_context{system_, "INotificationService"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, D<&NotificationService::GetEvent>, "GetEvent"},
            {1, D<&NotificationService::Clear>, "Clear"},
            {2, D<&NotificationService::Pop>, "Pop"},
        };
        // clang-format on

        RegisterHandlers(functions);

        notification_event = service_context.CreateEvent("INotificationService:NotifyEvent");
        session->Register(this);
    }

    ~NotificationService() override {
        // Unregistered first: once this returns, the signalling thread cannot reach the event.
        session->Unregister(this);
        service_context.CloseEvent(notification_event);
    }

    void OnFriendListUpdate(const std::string& user) override {
        if (user != user_key) {
            return;
        }
        std::scoped_lock lock{mutex};
        queue.FriendListUpdate();
        notification_event->Signal(system.Kernel());
    }

    void OnNewFriendRequest(const std::string& user) override {
        if (user != user_key) {
            return;
        }
        std::scoped_lock lock{mutex};
        if (queue.NewFriendRequest()) {
            notification_event->Signal(system.Kernel());
        }
    }

private:
    Result GetEvent(OutCopyHandle<Kernel::KReadableEvent> out_event) {
        LOG_DEBUG(Service_Friend, "called");
        *out_event = &notification_event->GetReadableEvent();
        R_SUCCEED();
    }

    Result Clear() {
        LOG_DEBUG(Service_Friend, "called");
        std::scoped_lock lock{mutex};
        queue.Clear();
        R_SUCCEED();
    }

    Result Pop(Out<Overlay::SizedNotificationInfo> out_info) {
        LOG_DEBUG(Service_Friend, "called");
        std::scoped_lock lock{mutex};
        R_UNLESS(queue.Pop(*out_info), ResultNotificationQueueEmpty);
        R_SUCCEED();
    }

    std::shared_ptr<FriendsSession> session;
    const std::string user_key;

    std::mutex mutex;
    Overlay::NotificationQueue queue;

    KernelHelpers::ServiceContext service_context;
    Kernel::KEvent* notification_event;
};

// ------------------------------------------------------------------------------------------------
// IFriendService
// ------------------------------------------------------------------------------------------------

class FriendService final : public ServiceFramework<FriendService> {
public:
    explicit FriendService(Core::System& system_, std::shared_ptr<FriendsSession> session_,
                           u32 permission_)
        : ServiceFramework{system_, "IFriendService"}, permission{permission_},
          service_context{system_, "IFriendService"}, session{std::move(session_)} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, D<&FriendService::GetCompletionEvent>, "GetCompletionEvent"},
            {1, D<&FriendService::Cancel>, "Cancel"},
            {10100, D<&FriendService::GetFriendListIds>, "GetFriendListIds"},
            {10101, D<&FriendService::GetFriendList>, "GetFriendList"},
            {10102, D<&FriendService::UpdateFriendInfo>, "UpdateFriendInfo"},
            {10110, D<&FriendService::GetFriendProfileImage>, "GetFriendProfileImage"},
            {10111, nullptr, "GetFriendProfileImageWithImageSize"}, // 18.0.0+
            {10120, D<&FriendService::CheckFriendListAvailability>, "CheckFriendListAvailability"},
            {10121, D<&FriendService::EnsureFriendListAvailable>, "EnsureFriendListAvailable"},
            {10200, D<&FriendService::SendFriendRequestForApplication>, "SendFriendRequestForApplication"},
            {10211, D<&FriendService::AddFacedFriendRequestForApplication>, "AddFacedFriendRequestForApplication"},
            {10400, D<&FriendService::GetBlockedUserListIds>, "GetBlockedUserListIds"},
            {10420, D<&FriendService::CheckBlockedUserListAvailability>, "CheckBlockedUserListAvailability"},
            {10421, D<&FriendService::EnsureBlockedUserListAvailable>, "EnsureBlockedUserListAvailable"},
            {10500, D<&FriendService::GetProfileList>, "GetProfileList"},
            {10501, nullptr, "GetProfileListV2"}, // 18.0.0+
            {10502, D<&FriendService::GetProfileList>, "GetProfileListV23"}, // 23.0.0+
            {10503, D<&FriendService::Unknown10503>, "Unknown10503"}, // 23.0.0+
            {10600, D<&FriendService::DeclareOpenOnlinePlaySession>, "DeclareOpenOnlinePlaySession"},
            {10601, D<&FriendService::DeclareCloseOnlinePlaySession>, "DeclareCloseOnlinePlaySession"},
            {10610, D<&FriendService::UpdateUserPresence>, "UpdateUserPresence"},
            {10700, D<&FriendService::GetPlayHistoryRegistrationKey>, "GetPlayHistoryRegistrationKey"},
            {10701, D<&FriendService::GetPlayHistoryRegistrationKeyWithNetworkServiceAccountId>, "GetPlayHistoryRegistrationKeyWithNetworkServiceAccountId"},
            {10702, D<&FriendService::AddPlayHistory>, "AddPlayHistory"},
            {11000, D<&FriendService::GetProfileImageUrl>, "GetProfileImageUrl"},
            {11001, nullptr, "GetProfileImageUrlV2"}, // 18.0.0+
            {20100, D<&FriendService::GetFriendCount>, "GetFriendCount"},
            {20101, D<&FriendService::GetNewlyFriendCount>, "GetNewlyFriendCount"},
            {20102, D<&FriendService::GetFriendDetailedInfo>, "GetFriendDetailedInfo"},
            {20103, D<&FriendService::SyncFriendList>, "SyncFriendList"},
            {20104, D<&FriendService::SyncFriendList>, "RequestSyncFriendList"},
            {20105, D<&FriendService::GetFriendListForViewer>, "GetFriendListForViewerV1"},
            {20106, D<&FriendService::UpdateFriendInfoForViewer>, "UpdateFriendInfoForViewerV1"},
            {20107, D<&FriendService::GetFriendDetailedInfo>, "GetFriendDetailedInfoV2"}, // 20.0.0+
            {20108, D<&FriendService::GetFriendListForViewerV2>, "GetFriendListForViewerV2"}, // 22.0.0+
            {20109, D<&FriendService::UpdateFriendInfoForViewerV2>, "UpdateFriendInfoForViewerV2"}, // 22.0.0+
            {20110, D<&FriendService::LoadFriendSetting>, "LoadFriendSettingV1"},
            {20111, D<&FriendService::LoadFriendSettingV2>, "LoadFriendSettingV2"}, // 22.0.0+
            {20200, D<&FriendService::GetReceivedFriendRequestCount>, "GetReceivedFriendRequestCount"},
            {20201, D<&FriendService::GetFriendRequestList>, "GetFriendRequestListV1"},
            {20202, D<&FriendService::GetFriendRequestListV2>, "GetFriendRequestListV2"}, // 20.0.0+
            {20203, D<&FriendService::GetReceivedFriendRequestPushCount>, "GetFriendRequestReceivedNotificationCount"}, // 22.0.0+
            {20300, D<&FriendService::GetFriendCandidateList>, "GetFriendCandidateList"},
            {20301, D<&FriendService::GetNintendoNetworkIdInfo>, "GetNintendoNetworkIdInfo"},
            {20302, D<&FriendService::GetSnsAccountLinkage>, "GetSnsAccountLinkage"}, // 5.0.0-19.0.1
            {20303, D<&FriendService::GetSnsAccountProfile>, "GetSnsAccountProfile"}, // 5.0.0-19.0.1
            {20304, D<&FriendService::GetSnsAccountFriendList>, "GetSnsAccountFriendList"}, // 5.0.0-19.0.1
            {20400, D<&FriendService::GetBlockedUserList>, "GetBlockedUserListV1"},
            {20401, D<&FriendService::SyncBlockedUserList>, "SyncBlockedUserList"},
            {20402, D<&FriendService::GetBlockedUserListV2>, "GetBlockedUserListV2"}, // 20.0.0+
            {20500, D<&FriendService::GetProfileExtraList>, "GetProfileExtraListV1"},
            {20501, D<&FriendService::GetRelationship>, "GetRelationship"},
            {20502, D<&FriendService::GetProfileExtraListV2>, "GetProfileExtraListV2"}, // 19.0.0+
            {20600, D<&FriendService::GetUserPresenceView>, "GetUserPresenceViewV1"},
            {20601, D<&FriendService::GetUserPresenceView>, "GetUserPresenceViewV2"}, // 19.0.0+
            {20700, D<&FriendService::GetPlayHistoryList>, "GetPlayHistoryListV1"},
            {20701, D<&FriendService::GetPlayHistoryStatistics>, "GetPlayHistoryStatistics"},
            {20702, nullptr, "GetPlayHistoryListV2"}, // 19.0.0+
            {20800, D<&FriendService::LoadUserSetting>, "LoadUserSettingV1"},
            {20801, D<&FriendService::SyncUserSetting>, "SyncUserSetting"},
            {20802, D<&FriendService::LoadUserSettingV2>, "LoadUserSettingV2"}, // 19.0.0+
            {20900, D<&FriendService::RequestListSummaryOverlayNotification>, "RequestListSummaryOverlayNotification"},
            {21000, D<&FriendService::GetExternalApplicationCatalog>, "GetExternalApplicationCatalog"},
            {22000, D<&FriendService::GetReceivedFriendInvitationList>, "GetReceivedFriendInvitationListV1"},
            {22001, D<&FriendService::GetReceivedFriendInvitationDetailedInfo>, "GetReceivedFriendInvitationDetailedInfoV1"},
            {22002, D<&FriendService::GetReceivedFriendInvitationListV2>, "GetReceivedFriendInvitationListV2"}, // 19.0.0+
            {22003, D<&FriendService::GetReceivedFriendInvitationDetailedInfoV2>, "GetReceivedFriendInvitationDetailedInfoV2"}, // 19.0.0+
            {22010, D<&FriendService::GetReceivedFriendInvitationCountCache>, "GetReceivedFriendInvitationCountCache"},
            {30100, D<&FriendService::DropFriendNewlyFlags>, "DropFriendNewlyFlags"},
            {30101, D<&FriendService::DeleteFriend>, "DeleteFriend"},
            {30110, D<&FriendService::DropFriendNewlyFlag>, "DropFriendNewlyFlag"},
            {30120, D<&FriendService::ChangeFriendFavoriteFlag>, "ChangeFriendFavoriteFlag"},
            {30121, D<&FriendService::ChangeFriendOnlineNotificationFlag>, "ChangeFriendOnlineNotificationFlag"},
            {30130, D<&FriendService::ChangeFriendNote>, "SetFriendNote"}, // 22.0.0+
            {30131, D<&FriendService::SendPendingFriendChangeUnchecked>, "RequestUploadPendingNote"}, // 22.0.0+
            {30190, D<&FriendService::SendPendingFriendChange>, "RequestSyncLocalUpdates"}, // 22.0.0+
            {30200, D<&FriendService::SendFriendRequest>, "SendFriendRequest"},
            {30201, D<&FriendService::SendFriendRequestWithApplicationInfo>, "SendFriendRequestWithApplicationInfoV1"},
            {30202, D<&FriendService::CancelFriendRequest>, "CancelFriendRequest"},
            {30203, D<&FriendService::AcceptFriendRequest>, "AcceptFriendRequest"},
            {30204, D<&FriendService::RejectFriendRequest>, "RejectFriendRequest"},
            {30205, D<&FriendService::ReadFriendRequest>, "ReadFriendRequest"},
            {30210, D<&FriendService::GetFacedFriendRequestRegistrationKey>, "GetFacedFriendRequestRegistrationKey"},
            {30211, D<&FriendService::AddFacedFriendRequest>, "AddFacedFriendRequest"},
            {30212, D<&FriendService::CancelFacedFriendRequest>, "CancelFacedFriendRequest"},
            {30213, D<&FriendService::GetFacedFriendRequestProfileImage>, "GetFacedFriendRequestProfileImage"},
            {30214, D<&FriendService::GetFacedFriendRequestProfileImageFromPath>, "GetFacedFriendRequestProfileImageFromPath"},
            {30215, D<&FriendService::SendFriendRequestWithExternalApplicationCatalogId>, "SendFriendRequestWithExternalApplicationCatalogId"},
            {30216, D<&FriendService::ResendFacedFriendRequest>, "ResendFacedFriendRequest"},
            {30217, D<&FriendService::SendFriendRequestWithNintendoNetworkIdInfo>, "SendFriendRequestWithNintendoNetworkIdInfo"},
            {30218, D<&FriendService::SendFriendRequestWithApplicationInfoV2>, "SendFriendRequestWithApplicationInfoV2"}, // 20.0.0+
            {30300, D<&FriendService::GetSnsAccountLinkPageUrl>, "GetSnsAccountLinkPageUrl"}, // 5.0.0-19.0.1
            {30301, D<&FriendService::UnlinkSnsAccount>, "UnlinkSnsAccount"}, // 5.0.0-19.0.1
            {30400, D<&FriendService::BlockUser>, "BlockUser"},
            {30401, D<&FriendService::BlockUserWithApplicationInfo>, "BlockUserWithApplicationInfoV1"},
            {30402, D<&FriendService::UnblockUser>, "UnblockUser"},
            {30403, D<&FriendService::BlockUserWithApplicationInfoV2>, "BlockUserWithApplicationInfoV2"}, // 20.0.0+
            {30500, D<&FriendService::GetProfileExtraFromFriendCode>, "GetProfileExtraFromFriendCodeV1"},
            {30501, nullptr, "GetProfileExtraFromFriendCodeV2"}, // 19.0.0+
            {30700, D<&FriendService::DeletePlayHistory>, "DeletePlayHistory"},
            {30701, nullptr, "AddPlayHistoryWithApplication"}, // 19.0.0+
            {30810, D<&FriendService::ChangePresencePermission>, "ChangePresencePermission"},
            {30811, D<&FriendService::ChangeFriendRequestReception>, "ChangeFriendRequestReception"},
            {30812, D<&FriendService::ChangePlayLogPermission>, "ChangePlayLogPermission"},
            {30820, D<&FriendService::IssueFriendCode>, "IssueFriendCode"},
            {30830, D<&FriendService::ClearPlayLog>, "ClearPlayLog"},
            {30900, &FriendService::SendFriendInvitation, "SendFriendInvitationV1"},
            {30901, &FriendService::SendFriendInvitationV2, "SendFriendInvitationV2"}, // 19.0.0+
            {30910, D<&FriendService::ReadFriendInvitation>, "ReadFriendInvitation"},
            {30911, D<&FriendService::ReadAllFriendInvitations>, "ReadAllFriendInvitations"},
            {31000, nullptr, "OpenUser"}, // 19.0.0+
            {40100, D<&FriendService::DeleteFriendListCache>, "DeleteFriendListCache"},
            {40400, D<&FriendService::DeleteBlockedUserListCache>, "DeleteBlockedUserListCache"},
            {49900, D<&FriendService::DeleteNetworkServiceAccountCache>, "DeleteNetworkServiceAccountCache"},
        };
        // clang-format on

        RegisterHandlers(functions);

        completion_event = service_context.CreateEvent("IFriendService:CompletionEvent");

        // Created NOT signalled, as the friends module creates it (22.5.0). The client library
        // asks "has my call finished" by polling this event and nothing else, while the call itself
        // runs on a worker thread it has only just started: an event that is already signalled
        // answers yes before that thread has run, and the game reads a result nobody has written.
        // Moving Out 2 lost that race on every first launch on Ryujinx and reported "An error has
        // occurred connecting to T17 services" (openpak/ryujinx de8916462, b207dac62). Work that
        // ends on a worker thread raises it through the session's signalling thread.
        completion = session->Worker().MakeHostEvent(completion_event);
    }

    ~FriendService() override {
        // The module signals the event as the session goes, which is what completes a call that
        // needed no work. A sync still running finds the event detached and leaves it alone.
        completion->Detach();
        completion_event->Signal(system.Kernel());
        service_context.CloseEvent(completion_event);
    }

private:
    /// Whether this port carries the viewer bit (friend:v, friend:m, friend:a). A viewer reads a
    /// friend's presence blob whatever group it belongs to; friend:u and friend:s get the privacy
    /// filter (contract §B.3).
    bool Viewer() const {
        return (permission & ViewerMask) != 0;
    }

    /// The viewer bit, or 2121-0090 (contract §B.1). 20xxx and 22xxx need it.
    Result RequireViewer() const {
        return Viewer() ? ResultSuccess : ResultPermissionDenied;
    }

    /// The manager bit (friend:m, friend:a), or 2121-0090. Every 30xxx needs it.
    Result RequireManager() const {
        return (permission & ManagerMask) != 0 ? ResultSuccess : ResultPermissionDenied;
    }

    /// The system bit (friend:s, friend:a), or 2121-0090. 40100/40400/49900 need it.
    Result RequireSystem() const {
        return (permission & SystemMask) != 0 ? ResultSuccess : ResultPermissionDenied;
    }

    Result GetCompletionEvent(OutCopyHandle<Kernel::KReadableEvent> out_event) {
        LOG_DEBUG(Service_Friend, "called");
        *out_event = &completion_event->GetReadableEvent();
        R_SUCCEED();
    }

    /// The completion event reports whether the work a command started has finished: cleared while
    /// that work runs, signalled when it ends.
    void Await(std::function<void()> work) {
        completion_event->Clear(system.Kernel());
        Baas::RunInBackground([work = std::move(work), done = completion] {
            work();
            done->Raise();
        });
    }

    /// Nothing to wait for: the answer is already in hand.
    void Done() {
        completion_event->Signal(system.Kernel());
    }

    Result Cancel() {
        // A sync already in flight cannot be stopped, but a guest that cancels must not be left
        // waiting on the completion event for work it has given up on.
        LOG_DEBUG(Service_Friend, "called");
        Done();
        R_SUCCEED();
    }

    // ---- 10xxx: every port ----

    Result GetFriendListIds(Out<s32> out_count,
                            OutArray<NetworkServiceAccountId, BufferAttr_HipcPointer> out_ids,
                            s32 offset, Uid user, Fr::SizedFriendFilter filter,
                            ClientProcessId pid) {
        *out_count = 0;
        R_UNLESS(!IsNull(user), ResultInvalidArgument);

        if (!AvailableFor(user)) {
            LOG_DEBUG(Service_Friend, "(not served) called, offset={}", offset);
            R_SUCCEED();
        }

        s32 count = 0;
        for (const Baas::Friend& one : Filtered(filter, offset)) {
            if (static_cast<std::size_t>(count) == out_ids.size()) {
                break;
            }
            out_ids[count++] = one.id;
        }

        *out_count = count;
        R_SUCCEED();
    }

    Result GetFriendList(Out<s32> out_count,
                         OutArray<Fr::FriendImpl, BufferAttr_HipcMapAlias> out_list,
                         s32 offset, Uid user, Fr::SizedFriendFilter filter,
                         ClientProcessId pid) {
        *out_count = 0;
        R_UNLESS(!IsNull(user), ResultInvalidArgument);

        if (!AvailableFor(user)) {
            LOG_DEBUG(Service_Friend, "(not served) called, offset={}", offset);
            R_SUCCEED();
        }

        const u64 own_group = OwnPresenceGroupId();
        s32 count = 0;
        for (const Baas::Friend& one : Filtered(filter, offset)) {
            if (static_cast<std::size_t>(count) == out_list.size()) {
                break;
            }
            out_list[count++] = Fr::ToFriendImpl(one, user, own_group, Viewer());
        }

        LOG_DEBUG(Service_Friend, "[OpenPak] Served {} friends to the guest", count);

        *out_count = count;
        R_SUCCEED();
    }

    Result UpdateFriendInfo(OutArray<Fr::FriendImpl, BufferAttr_HipcMapAlias> out_info,
                            Uid user,
                            InArray<NetworkServiceAccountId, BufferAttr_HipcPointer> friend_ids,
                            ClientProcessId pid) {
        if (!AvailableFor(user)) {
            LOG_DEBUG(Service_Friend, "(not served) called");
            R_SUCCEED();
        }

        // The guest asks about specific ids and expects them back in the order it asked, so this
        // answers per slot rather than filling the buffer with whoever matched.
        const u64 own_group = OwnPresenceGroupId();
        const auto list = Baas::Friends();

        for (std::size_t index = 0; index < friend_ids.size() && index < out_info.size();
             index++) {
            out_info[index] = {};

            for (const Baas::Friend& one : *list) {
                if (one.id == friend_ids[index]) {
                    out_info[index] = Fr::ToFriendImpl(one, user, own_group, Viewer());
                    break;
                }
            }
        }

        R_SUCCEED();
    }

    Result GetFriendProfileImage(Out<s32> out_size, Uid user, NetworkServiceAccountId friend_id,
                                 OutBuffer<BufferAttr_HipcMapAlias> out_image) {
        *out_size = 0;

        // The overlay applet's pop-ups need it (overlay-notes.md 4.1-4.2): no picture, no pop-up.
        // Answered from the picture cache only; a miss starts the fetch for the next call, and
        // the push consumer fetches the picture before it sends the notification that asks.
        const std::string url = AvailableFor(user) ? Pictures::UrlOf(friend_id) : std::string{};
        if (url.empty()) {
            LOG_DEBUG(Service_Friend, "no picture for that friend");
            R_SUCCEED();
        }
        const auto image = Pictures::Cached(url);
        if (!image) {
            Pictures::FetchInBackground(url);
            LOG_DEBUG(Service_Friend, "picture not fetched yet");
            R_SUCCEED();
        }
        if (image->size() > out_image.size()) {
            LOG_WARNING(Service_Friend, "a picture of 0x{:x} bytes does not fit 0x{:x}",
                        image->size(), out_image.size());
            R_SUCCEED();
        }
        std::memcpy(out_image.data(), image->data(), image->size());
        *out_size = static_cast<s32>(image->size());
        R_SUCCEED();
    }

    Result CheckFriendListAvailability(Out<bool> out_available, Uid user) {
        // Offline this is what friend.cpp always said: a list of nobody, available at once.
        *out_available = AvailableFor(user) ? Baas::FriendListAvailable() : true;
        R_SUCCEED();
    }

    Result EnsureFriendListAvailable(Uid user) {
        R_UNLESS(!IsNull(user), ResultInvalidArgument);

        // The console syncs inline when it has no cache. Here the sync is started and the guest
        // answered at once: blocking a game's thread on an HTTP round trip is the one thing this
        // service may never do.
        if (AvailableFor(user) && !Baas::FriendListAvailable()) {
            Await([] { Baas::SyncFriendList(true); });
        } else {
            Done();
        }

        R_SUCCEED();
    }

    Result SendFriendRequestForApplication(
        Uid user, NetworkServiceAccountId friend_id,
        InLargeData<Fr::InAppScreenName, BufferAttr_HipcPointer> target_name,
        InLargeData<Fr::InAppScreenName, BufferAttr_HipcPointer> own_name,
        ClientProcessId pid) {
        if (!AvailableFor(user)) {
            LOG_DEBUG(Service_Friend, "(not served) called");
            R_SUCCEED();
        }

        // The application info comes from the caller's process, and the channel is always
        // IN_APP: this is the in-game "send friend request" button.
        const u64 title_id = system.GetApplicationProcessProgramID();

        Baas::FriendRequestSend send;
        send.target_id = friend_id;
        send.channel = "IN_APP";
        send.application_id = title_id;
        send.presence_group_id = title_id;
        std::tie(send.target_name, send.target_language) = Fr::ScreenName(*target_name);
        std::tie(send.own_name, send.own_language) = Fr::ScreenName(*own_name);

        SendFriendRequestInBackground(std::move(send));
        R_SUCCEED();
    }

    Result AddFacedFriendRequestForApplication(Uid user) {
        LOG_WARNING(Service_Friend, "(STUBBED) called");
        R_SUCCEED();
    }

    Result GetBlockedUserListIds(Out<s32> out_count,
                                 OutArray<NetworkServiceAccountId, BufferAttr_HipcPointer> out_ids,
                                 s32 offset, Uid user) {
        *out_count = 0;

        // Offline this is safe to stub, as there should be no adverse consequences from
        // reporting no blocked users.
        if (!AvailableFor(user)) {
            R_SUCCEED();
        }

        const auto blocks = Baas::Blocks();
        s32 count = 0;
        for (std::size_t index = static_cast<std::size_t>((std::max)(offset, 0));
             index < blocks->size() && static_cast<std::size_t>(count) < out_ids.size();
             index++) {
            out_ids[count++] = (*blocks)[index].id;
        }

        *out_count = count;
        R_SUCCEED();
    }

    Result CheckBlockedUserListAvailability(Out<bool> out_available, Uid user) {
        *out_available = AvailableFor(user) ? Baas::BlockListAvailable() : true;
        R_SUCCEED();
    }

    Result EnsureBlockedUserListAvailable(Uid user) {
        if (AvailableFor(user) && !Baas::BlockListAvailable()) {
            Await([] { Baas::SyncBlockList(); });
        }
        R_SUCCEED();
    }

    Result GetProfileList(OutArray<Fr::ProfileImpl, BufferAttr_HipcMapAlias> out_list,
                          Uid user,
                          InArray<NetworkServiceAccountId, BufferAttr_HipcPointer> friend_ids) {
        if (!AvailableFor(user)) {
            LOG_DEBUG(Service_Friend, "(not served) called");
            R_SUCCEED();
        }

        // Answered per slot, in the order asked, as the module matches its results by id. A user
        // nobody has looked up yet is left invalid and fetched for the next call.
        std::size_t resolved = 0;
        std::size_t asked = 0;
        for (std::size_t index = 0; index < friend_ids.size() && index < out_list.size();
             index++) {
            const auto found = User(friend_ids[index]);
            out_list[index] = found ? Fr::ToProfileImpl(*found) : Fr::ProfileImpl{};
            resolved += found ? 1 : 0;
            ++asked;
        }
        // An unresolved profile is invisible from outside, and a game that cannot build its friend
        // list just gives up, so a miss is worth a line -- counts only, never the ids.
        if (resolved != asked) {
            LOG_WARNING(Service_Friend, "[OpenPak] GetProfileList: {}/{} resolved", resolved, asked);
        }

        Warm(friend_ids);
        R_SUCCEED();
    }

    // friends 23.0.1 answers 10503 with 2121-0092 and nothing else (social FINDINGS); unnamed on
    // switchbrew. 10502 is 10500's code with a busy check this module never trips.
    Result Unknown10503() {
        R_THROW(FromDescription(92));
    }

    Result DeclareOpenOnlinePlaySession(Uid user) {
        LOG_DEBUG(Service_Friend, "called");
        R_UNLESS(!IsNull(user), ResultInvalidArgument);

        // Other profiles have no account to publish for, and are not kept.
        if (AvailableFor(user)) {
            PresenceState::Instance().DeclareOpenOnlinePlaySession(KeyOf(user));
        }
        R_SUCCEED();
    }

    Result DeclareCloseOnlinePlaySession(Uid user) {
        LOG_DEBUG(Service_Friend, "called");
        R_UNLESS(!IsNull(user), ResultInvalidArgument);

        if (AvailableFor(user)) {
            PresenceState::Instance().DeclareCloseOnlinePlaySession(KeyOf(user));
        }
        R_SUCCEED();
    }

    Result UpdateUserPresence(Uid user,
                              InLargeData<Fr::UserPresenceImpl, BufferAttr_HipcPointer> presence,
                              ClientProcessId pid) {
        R_UNLESS(!IsNull(user), ResultInvalidArgument);

        // Commit: the declaration byte and the whole blob, published as PLAYING/ONLINE and an
        // appField object by the session, and only when one of them changed.
        // The blob is the title's own data (room ids, names): only the declaration is logged.
        LOG_DEBUG(Service_Friend, "called, declaration {}", presence->online_play_declaration);

        if (AvailableFor(user)) {
            PresenceState::Instance().UpdateUserPresence(KeyOf(user), *presence);
        }
        R_SUCCEED();
    }

    Result GetPlayHistoryRegistrationKey(
        OutLargeData<Fr::PlayHistoryRegistrationKey, BufferAttr_HipcPointer> out_key,
        bool arg2, Uid user) {
        R_UNLESS(!IsNull(user), ResultInvalidArgument);

        // The console keeps a per-user Uuid in its save data and signs the key with one of eight
        // HMAC keys. Play history is not served, so a random Uuid and a blank hash do.
        const Common::UUID random = Common::UUID::MakeRandom();

        Fr::PlayHistoryRegistrationKey key{};
        key.type = 0x101;
        key.key_index = static_cast<u8>(random.uuid[0] & 7);
        key.user_id_bool = 0;
        key.unknown_bool = arg2 ? 1 : 0;
        std::memcpy(&key.uuid, random.uuid.data(), sizeof(key.uuid));

        *out_key = key;
        R_SUCCEED();
    }

    Result GetPlayHistoryRegistrationKeyWithNetworkServiceAccountId(
        OutLargeData<Fr::PlayHistoryRegistrationKey, BufferAttr_HipcPointer> out_key) {
        LOG_WARNING(Service_Friend, "(STUBBED) called");
        *out_key = {};
        R_SUCCEED();
    }

    Result AddPlayHistory(Uid user) {
        LOG_WARNING(Service_Friend, "(STUBBED) called");
        R_SUCCEED();
    }

    Result GetProfileImageUrl(Out<Fr::Url> out_url, Fr::Url url, s32 size) {
        // The game hands in the thumbnail URL from a friend's record and asks for the URL to fetch.
        // An empty answer is a placeholder avatar where a console shows the real one: nx-baas puts
        // cdn-image-<...>.baas.nintendo.com/1/<userId> in the friend payload, that host is
        // redirected to OpenPak, and it serves the image. Pass it through (openpak/ryujinx
        // 201c5c451).
        // ponytail: unchanged, not resized. `size` selects a variant on hardware and nx-baas serves
        // /1/ and /2/; revisit if a title asks for a size we do not have.
        LOG_DEBUG(Service_Friend, "called, size={}", size);
        *out_url = url;
        R_SUCCEED();
    }

    // ---- 20xxx: viewer ports ----

    Result GetFriendCount(Out<s32> out_count, Uid user, Fr::SizedFriendFilter filter,
                          ClientProcessId pid) {
        *out_count = 0;
        R_TRY(RequireViewer());

        *out_count = AvailableFor(user) ? static_cast<s32>(Filtered(filter, 0).size()) : 0;
        R_SUCCEED();
    }

    Result GetNewlyFriendCount(Out<s32> out_count, Uid user) {
        *out_count = 0;
        R_TRY(RequireViewer());

        if (!AvailableFor(user)) {
            R_SUCCEED();
        }

        // "Newly" is extras.self.isConfirmed absent or false (§A.1): a friendship the person has
        // not yet looked at, which is what the badge on the friend list counts.
        const auto list = Baas::Friends();
        *out_count = static_cast<s32>(std::count_if(
            list->begin(), list->end(), [](const Baas::Friend& one) { return one.is_newly; }));
        R_SUCCEED();
    }

    Result GetFriendDetailedInfo(
        OutLargeData<Fr::FriendDetailedInfoImpl, BufferAttr_HipcPointer> out_info, Uid user,
        NetworkServiceAccountId friend_id) {
        *out_info = {};
        R_TRY(RequireViewer());

        const auto found = AvailableFor(user) ? Baas::FindFriend(friend_id) : std::nullopt;
        R_UNLESS(found.has_value(), FromDescription(Baas::FriendNotFound));

        *out_info = Fr::ToDetailedInfoImpl(*found, user);
        R_SUCCEED();
    }

    Result SyncFriendList(Uid user) {
        R_TRY(RequireViewer());

        // The console clears the cooldown and syncs inline; here the sync is started and the
        // guest answered at once. The list on screen refreshes through the notification event.
        if (AvailableFor(user)) {
            Await([] { Baas::SyncFriendList(true); });
        } else {
            Done();
        }
        R_SUCCEED();
    }

    Result GetFriendListForViewer(
        Out<s32> out_count, OutArray<Fr::FriendForViewerImpl, BufferAttr_HipcMapAlias> out_list,
        s32 offset, Uid user, Fr::SizedFriendFilter filter) {
        *out_count = 0;
        R_TRY(RequireViewer());
        R_UNLESS(!IsNull(user), ResultInvalidArgument);

        if (!AvailableFor(user)) {
            R_SUCCEED();
        }

        const u64 own_group = OwnPresenceGroupId();
        s32 count = 0;
        for (const Baas::Friend& one : Filtered(filter, offset)) {
            if (static_cast<std::size_t>(count) == out_list.size()) {
                break;
            }
            out_list[count++] = Fr::ToFriendForViewerImpl(one, user, own_group);
        }

        *out_count = count;
        R_SUCCEED();
    }

    Result UpdateFriendInfoForViewer(
        OutArray<Fr::FriendForViewerImpl, BufferAttr_HipcMapAlias> out_info, Uid user,
        InArray<NetworkServiceAccountId, BufferAttr_HipcPointer> friend_ids) {
        R_TRY(RequireViewer());

        const bool available = AvailableFor(user);
        const u64 own_group = OwnPresenceGroupId();

        for (std::size_t index = 0; index < friend_ids.size() && index < out_info.size();
             index++) {
            const auto found = available ? Baas::FindFriend(friend_ids[index]) : std::nullopt;
            out_info[index] = found ? Fr::ToFriendForViewerImpl(*found, user, own_group)
                                    : Fr::FriendForViewerImpl{};
        }
        R_SUCCEED();
    }

    // 20108 / 20109 are the 0x220 viewer shape. Its first part is the same, but the audit does
    // not pin where the valid flag sits in it, and a friend written without one is a friend the
    // caller drops -- so the commands exist (a missing id is a CMIF error the caller cannot tell
    // from a real failure) and answer an empty list.
    Result GetFriendListForViewerV2(Out<s32> out_count) {
        *out_count = 0;
        LOG_WARNING(Service_Friend, "unknown contract: friend 20108 GetFriendListForViewerV2 "
                                    "(0x220 record layout not pinned), answered empty");
        R_RETURN(RequireViewer());
    }

    Result UpdateFriendInfoForViewerV2() {
        // overlayDisp reads the "is now online" nickname from this record (overlay-notes.md 4.1).
        LOG_WARNING(Service_Friend, "unknown contract: friend 20109 UpdateFriendInfoForViewerV2 "
                                    "(0x220 record layout not pinned), answered empty");
        R_RETURN(RequireViewer());
    }

    Result LoadFriendSetting(
        OutLargeData<Fr::FriendSettingImpl, BufferAttr_HipcPointer> out_setting, Uid user,
        NetworkServiceAccountId friend_id) {
        *out_setting = {};
        R_TRY(RequireViewer());

        const auto found = AvailableFor(user) ? Baas::FindFriend(friend_id) : std::nullopt;
        R_UNLESS(found.has_value(), FromDescription(Baas::FriendNotFound));

        *out_setting = Fr::ToFriendSettingImpl(*found, user);
        R_SUCCEED();
    }

    Result LoadFriendSettingV2(
        OutLargeData<Fr::FriendSettingImplV2, BufferAttr_HipcPointer> out_setting, Uid user,
        NetworkServiceAccountId friend_id) {
        *out_setting = {};
        R_TRY(RequireViewer());

        const auto found = AvailableFor(user) ? Baas::FindFriend(friend_id) : std::nullopt;
        R_UNLESS(found.has_value(), FromDescription(Baas::FriendNotFound));

        *out_setting = Fr::ToFriendSettingImplV2(*found, user);
        R_SUCCEED();
    }

    Result GetReceivedFriendRequestCount(Out<s32> out_unread, Out<s32> out_read, Uid user) {
        *out_unread = 0;
        *out_read = 0;
        R_TRY(RequireViewer());

        if (!AvailableFor(user)) {
            LOG_DEBUG(Service_Friend, "(not served) called");
            R_SUCCEED();
        }

        // The badge (contract §A.4.3): unread is the inbox items without extras.receiver.read
        // true, read the ones with it.
        for (const Baas::Request& request : *Baas::InboxRequests()) {
            if (request.read) {
                ++*out_read;
            } else {
                ++*out_unread;
            }
        }
        R_SUCCEED();
    }

    /// Type 1 is the outbox (sent), 2 the inbox (received). Type 0 is in-person requests, which
    /// live in faced.v2.bin and never touch REST.
    static std::shared_ptr<const std::vector<Baas::Request>> Box(s32 list_type) {
        switch (list_type) {
        case 1:
            return Baas::OutboxRequests();
        case 2:
            return Baas::InboxRequests();
        default:
            return std::make_shared<const std::vector<Baas::Request>>();
        }
    }

    Result GetFriendRequestList(Out<s32> out_count,
                                OutArray<Fr::FriendRequestImpl, BufferAttr_HipcMapAlias> out_list,
                                s32 offset, s32 list_type, Uid user) {
        *out_count = 0;
        R_TRY(RequireViewer());

        if (!AvailableFor(user)) {
            LOG_DEBUG(Service_Friend, "(not served) called, offset={}, list_type={}", offset, list_type);
            R_SUCCEED();
        }

        const auto box = Box(list_type);
        s32 count = 0;
        for (std::size_t index = static_cast<std::size_t>((std::max)(offset, 0));
             index < box->size() && static_cast<std::size_t>(count) < out_list.size(); index++) {
            out_list[count++] = Fr::ToRequestImpl((*box)[index], user, list_type);
        }

        *out_count = count;
        R_SUCCEED();
    }

    Result GetFriendRequestListV2(
        Out<s32> out_count, OutArray<Fr::FriendRequestImplV2, BufferAttr_HipcMapAlias> out_list,
        s32 offset, s32 list_type, Uid user) {
        *out_count = 0;
        R_TRY(RequireViewer());

        if (!AvailableFor(user)) {
            LOG_DEBUG(Service_Friend, "(not served) called, offset={}, list_type={}", offset, list_type);
            R_SUCCEED();
        }

        const auto box = Box(list_type);
        s32 count = 0;
        for (std::size_t index = static_cast<std::size_t>((std::max)(offset, 0));
             index < box->size() && static_cast<std::size_t>(count) < out_list.size(); index++) {
            out_list[count++] = Fr::ToRequestImplV2((*box)[index], user, list_type);
        }

        *out_count = count;
        R_SUCCEED();
    }

    // 20203 counts the `friend_request_received` pushes this console has seen. When the module
    // resets that counter is not established, so it stays 0, which is what a console that has
    // been pushed nothing reports.
    Result GetReceivedFriendRequestPushCount(Out<s32> out_count) {
        *out_count = 0;
        R_RETURN(RequireViewer());
    }

    Result GetFriendCandidateList(Out<s32> out_count) {
        LOG_WARNING(Service_Friend, "(STUBBED) called");
        *out_count = 0;
        R_SUCCEED();
    }

    Result GetNintendoNetworkIdInfo(
        OutLargeData<Fr::NintendoNetworkIdUserInfo, BufferAttr_HipcPointer> out_info,
        Out<s32> out_count) {
        LOG_WARNING(Service_Friend, "(STUBBED) called");
        *out_info = {};
        *out_count = 0;
        R_SUCCEED();
    }

    Result GetSnsAccountLinkage(Out<Fr::SnsAccountLinkage> out_linkage) {
        LOG_WARNING(Service_Friend, "(STUBBED) called");
        *out_linkage = {};
        R_SUCCEED();
    }

    Result GetSnsAccountProfile(
        OutLargeData<Fr::SnsAccountProfile, BufferAttr_HipcPointer> out_profile) {
        LOG_WARNING(Service_Friend, "(STUBBED) called");
        *out_profile = {};
        R_SUCCEED();
    }

    Result GetSnsAccountFriendList(Out<s32> out_count) {
        LOG_WARNING(Service_Friend, "(STUBBED) called");
        *out_count = 0;
        R_SUCCEED();
    }

    Result GetBlockedUserList(Out<s32> out_count,
                              OutArray<Fr::BlockedUserImpl, BufferAttr_HipcMapAlias> out_list,
                              s32 offset, Uid user) {
        *out_count = 0;
        R_TRY(RequireViewer());

        if (!AvailableFor(user)) {
            R_SUCCEED();
        }

        const auto blocks = Baas::Blocks();
        s32 count = 0;
        for (std::size_t index = static_cast<std::size_t>((std::max)(offset, 0));
             index < blocks->size() && static_cast<std::size_t>(count) < out_list.size();
             index++) {
            out_list[count++] = Fr::ToBlockedUserImpl((*blocks)[index], user);
        }

        *out_count = count;
        R_SUCCEED();
    }

    Result GetBlockedUserListV2(Out<s32> out_count,
                                OutArray<Fr::BlockedUserImplV2, BufferAttr_HipcMapAlias> out_list,
                                s32 offset, Uid user) {
        *out_count = 0;
        R_TRY(RequireViewer());

        if (!AvailableFor(user)) {
            R_SUCCEED();
        }

        const auto blocks = Baas::Blocks();
        s32 count = 0;
        for (std::size_t index = static_cast<std::size_t>((std::max)(offset, 0));
             index < blocks->size() && static_cast<std::size_t>(count) < out_list.size();
             index++) {
            out_list[count++] = Fr::ToBlockedUserImplV2((*blocks)[index], user);
        }

        *out_count = count;
        R_SUCCEED();
    }

    Result SyncBlockedUserList(Uid user) {
        R_TRY(RequireViewer());

        if (AvailableFor(user)) {
            Await([] { Baas::SyncBlockList(); });
        }
        R_SUCCEED();
    }

    Result GetProfileExtraList(OutArray<Fr::ProfileExtraImpl, BufferAttr_HipcMapAlias> out_list,
                               Uid user,
                               InArray<NetworkServiceAccountId, BufferAttr_HipcPointer> friend_ids) {
        R_TRY(RequireViewer());

        for (std::size_t index = 0; index < friend_ids.size() && index < out_list.size();
             index++) {
            const auto found = User(friend_ids[index]);
            out_list[index] = found ? Fr::ToProfileExtraImpl(*found)
                                    : Fr::ProfileExtraImpl{};
        }

        Warm(friend_ids);
        R_SUCCEED();
    }

    Result GetProfileExtraListV2(
        OutArray<Fr::ProfileExtraImplV2, BufferAttr_HipcMapAlias> out_list, Uid user,
        InArray<NetworkServiceAccountId, BufferAttr_HipcPointer> friend_ids) {
        R_TRY(RequireViewer());

        for (std::size_t index = 0; index < friend_ids.size() && index < out_list.size();
             index++) {
            const auto found = User(friend_ids[index]);
            out_list[index] = found ? Fr::ToProfileExtraImplV2(*found)
                                    : Fr::ProfileExtraImplV2{};
        }

        Warm(friend_ids);
        R_SUCCEED();
    }

    Result GetRelationship(Out<Fr::Relationship> out_relationship, Uid user,
                           NetworkServiceAccountId friend_id) {
        *out_relationship = {};
        R_TRY(RequireViewer());

        if (!AvailableFor(user)) {
            R_SUCCEED();
        }

        // Answered from the caches the server already filled rather than with a round trip: the
        // friend list, the block list, and the sent-request box (§A.7).
        const auto blocks = Baas::Blocks();
        const auto outbox = Baas::OutboxRequests();

        out_relationship->is_friend = Baas::FindFriend(friend_id).has_value() ? 1 : 0;
        out_relationship->is_blocking =
            std::any_of(blocks->begin(), blocks->end(),
                        [friend_id](const Baas::Block& block) { return block.id == friend_id; })
                ? 1
                : 0;
        out_relationship->is_request_sent =
            std::any_of(outbox->begin(), outbox->end(),
                        [friend_id](const Baas::Request& request) {
                            return request.other_id == friend_id;
                        })
                ? 1
                : 0;
        R_SUCCEED();
    }

    Result GetUserPresenceView(
        OutLargeData<Fr::UserPresenceViewImpl, BufferAttr_HipcPointer> out_view, Uid user) {
        *out_view = {};
        R_TRY(RequireViewer());

        if (AvailableFor(user)) {
            *out_view = PresenceState::Instance().OwnPresenceView(KeyOf(user));
        }
        R_SUCCEED();
    }

    Result GetPlayHistoryList(Out<s32> out_count) {
        LOG_WARNING(Service_Friend, "(STUBBED) called");
        *out_count = 0;
        R_SUCCEED();
    }

    Result GetPlayHistoryStatistics(Out<Fr::PlayHistoryStatistics> out_statistics) {
        LOG_WARNING(Service_Friend, "(STUBBED) called");
        *out_statistics = {};
        R_SUCCEED();
    }

    Result LoadUserSetting(
        OutLargeData<Fr::UserSettingImpl, BufferAttr_HipcPointer> out_setting, Uid user) {
        *out_setting = {};
        R_TRY(RequireViewer());

        if (!AvailableFor(user)) {
            *out_setting = OfflineUserSetting(user);
            R_SUCCEED();
        }

        if (const auto setting = Baas::OwnUserSetting()) {
            *out_setting = Fr::ToUserSettingImpl(*setting, user);
        }
        R_SUCCEED();
    }

    Result LoadUserSettingV2(
        OutLargeData<Fr::UserSettingImplV2, BufferAttr_HipcPointer> out_setting, Uid user) {
        *out_setting = {};
        R_TRY(RequireViewer());

        if (!AvailableFor(user)) {
            // The V2 shape is the same up to the play log, so the offline answer is too.
            const Fr::UserSettingImpl offline = OfflineUserSetting(user);
            static_assert(sizeof(offline) == sizeof(Fr::UserSettingImplV2));
            std::memcpy(out_setting.Get(), &offline, sizeof(offline));
            R_SUCCEED();
        }

        if (const auto setting = Baas::OwnUserSetting()) {
            *out_setting = Fr::ToUserSettingImplV2(*setting, user);
        }
        R_SUCCEED();
    }

    Result SyncUserSetting(Uid user) {
        R_TRY(RequireViewer());

        if (AvailableFor(user)) {
            Await([] { Baas::SyncUserSetting(); });
        }
        R_SUCCEED();
    }

    // qlaunch calls it on resume: the 7001 summary overlayDisp shows once per user.
    Result RequestListSummaryOverlayNotification() {
        LOG_DEBUG(Service_Friend, "called");
        session->RequestListSummary();
        R_SUCCEED();
    }

    Result GetExternalApplicationCatalog(
        OutLargeData<Fr::ExternalApplicationCatalog, BufferAttr_HipcPointer> out_catalog) {
        LOG_WARNING(Service_Friend, "(STUBBED) called");
        *out_catalog = {};
        R_SUCCEED();
    }

    // ---- 22xxx: invitations ----

    Result GetReceivedFriendInvitationList(
        Out<s32> out_count,
        OutArray<Fr::FriendInvitationForViewerImpl, BufferAttr_HipcMapAlias> out_list,
        Uid user) {
        *out_count = 0;
        R_TRY(RequireViewer());

        if (!AvailableFor(user)) {
            R_SUCCEED();
        }

        // Read and unread both: the inbox request the module makes carries no read filter, and
        // each item says which it is.
        s32 count = 0;
        for (const Baas::Invitation& invitation : *Baas::Invitations()) {
            if (static_cast<std::size_t>(count) == out_list.size()) {
                break;
            }
            out_list[count++] = Fr::ToInvitationImpl(invitation);
        }

        *out_count = count;
        R_SUCCEED();
    }

    Result GetReceivedFriendInvitationListV2(
        Out<s32> out_count,
        OutArray<Fr::FriendInvitationForViewerImplV2, BufferAttr_HipcMapAlias> out_list,
        Uid user) {
        *out_count = 0;
        R_TRY(RequireViewer());

        if (!AvailableFor(user)) {
            R_SUCCEED();
        }

        s32 count = 0;
        for (const Baas::Invitation& invitation : *Baas::Invitations()) {
            if (static_cast<std::size_t>(count) == out_list.size()) {
                break;
            }
            out_list[count++] = Fr::ToInvitationImplV2(invitation);
        }

        *out_count = count;
        R_SUCCEED();
    }

    Result GetReceivedFriendInvitationDetailedInfo(
        OutLargeData<Fr::FriendInvitationGroupImpl, BufferAttr_HipcMapAlias> out_group,
        Uid user, u64 group_id) {
        *out_group = {};
        R_TRY(RequireViewer());

        const auto group = AvailableFor(user) ? Baas::CachedInvitationGroup(group_id)
                                              : std::nullopt;
        R_UNLESS(group.has_value(), FromDescription(Baas::InvalidArgument));

        *out_group = Fr::ToInvitationGroupImpl(*group);
        R_SUCCEED();
    }

    Result GetReceivedFriendInvitationDetailedInfoV2(
        OutLargeData<Fr::FriendInvitationGroupImplV2, BufferAttr_HipcMapAlias> out_group,
        Uid user, u64 group_id) {
        *out_group = {};
        R_TRY(RequireViewer());

        const auto group = AvailableFor(user) ? Baas::CachedInvitationGroup(group_id)
                                              : std::nullopt;
        R_UNLESS(group.has_value(), FromDescription(Baas::InvalidArgument));

        *out_group = Fr::ToInvitationGroupImplV2(*group);
        R_SUCCEED();
    }

    Result GetReceivedFriendInvitationCountCache(Out<s32> out_count, Uid user) {
        *out_count = 0;
        R_TRY(RequireViewer());

        // What the last invitation push said, else what the native inbox held at the last poll,
        // dismissals removed: the count a title's "you have been invited" badge is answered with.
        *out_count = AvailableFor(user) ? FriendsSession::UnreadInvitations() : 0;
        R_SUCCEED();
    }

    // ---- 30xxx: manager ports ----

    Result DropFriendNewlyFlags(Uid user) {
        R_TRY(RequireManager());

        if (AvailableFor(user)) {
            for (const Baas::Friend& one : *Baas::Friends()) {
                if (one.is_newly) {
                    DropNewly(one.id);
                }
            }
        }
        R_SUCCEED();
    }

    Result DeleteFriend(Uid user, NetworkServiceAccountId friend_id) {
        R_TRY(RequireManager());

        // Fire and forget: the DELETE is a network call and this is the game's thread. The
        // friendship disappears from the cache because the sync that follows it no longer
        // carries it, which is how the console loses it too (§A.3).
        if (AvailableFor(user)) {
            Baas::RunInBackground([friend_id] { Baas::DeleteFriend(friend_id); });
        }
        R_SUCCEED();
    }

    Result DropFriendNewlyFlag(Uid user, NetworkServiceAccountId friend_id) {
        R_TRY(RequireManager());

        if (AvailableFor(user)) {
            DropNewly(friend_id);
        }
        R_SUCCEED();
    }

    Result ChangeFriendFavoriteFlag(bool favorite, Uid user, NetworkServiceAccountId friend_id) {
        R_TRY(RequireManager());

        if (AvailableFor(user)) {
            Baas::Update(friend_id, [favorite](Baas::Friend& one) { one.is_favorite = favorite; });
            Baas::RunInBackground([friend_id, favorite] {
                Baas::PatchFriend(friend_id, "replace", "/isFavorite", favorite);
            });
        }
        R_SUCCEED();
    }

    Result ChangeFriendOnlineNotificationFlag(bool notify, Uid user,
                                              NetworkServiceAccountId friend_id) {
        R_TRY(RequireManager());

        if (AvailableFor(user)) {
            Baas::Update(friend_id,
                         [notify](Baas::Friend& one) { one.is_online_notification = notify; });
            Baas::RunInBackground([friend_id, notify] {
                Baas::PatchFriend(friend_id, "add", "/extras/self/isOnlineNotification", notify);
            });
        }
        R_SUCCEED();
    }

    Result ChangeFriendNote(Fr::FriendNote note, Uid user, NetworkServiceAccountId friend_id) {
        R_TRY(RequireManager());

        if (AvailableFor(user)) {
            std::string text = Fr::Text(std::span<const char>{note.note});
            Baas::Update(friend_id, [text](Baas::Friend& one) { one.note = text; });
            Baas::RunInBackground([friend_id, text = std::move(text)] {
                Baas::PatchFriend(friend_id, "replace", "/friendNote", text);
            });
        }
        R_SUCCEED();
    }

    // Nothing here keeps a queue of pending friend changes: every 301xx write goes out the moment
    // it is made. So a flush has nothing to flush, and says so by succeeding. 30131 is the same
    // command without the port check.
    Result SendPendingFriendChange() {
        R_RETURN(RequireManager());
    }

    Result SendPendingFriendChangeUnchecked() {
        R_SUCCEED();
    }

    Result SendFriendRequest(s32 channel, Uid user, NetworkServiceAccountId friend_id) {
        R_TRY(RequireManager());

        if (!AvailableFor(user)) {
            LOG_DEBUG(Service_Friend, "(not served) called, channel={}", channel);
            R_SUCCEED();
        }

        Baas::FriendRequestSend send;
        send.target_id = friend_id;
        send.channel = RequestChannel(channel);
        SendFriendRequestInBackground(std::move(send));
        R_SUCCEED();
    }

    Result SendFriendRequestWithApplicationInfo(
        s32 channel, Uid user, NetworkServiceAccountId friend_id,
        Fr::ApplicationInfo application_info,
        InLargeData<Fr::InAppScreenName, BufferAttr_HipcPointer> target_name,
        InLargeData<Fr::InAppScreenName, BufferAttr_HipcPointer> own_name) {
        R_TRY(RequireManager());

        if (!AvailableFor(user)) {
            LOG_DEBUG(Service_Friend, "(not served) called, channel={}", channel);
            R_SUCCEED();
        }

        // Screen name #1 is the target's name as the sender saw it, #2 the sender's own in-app
        // name -- the buffers arrive in that order.
        Baas::FriendRequestSend send;
        send.target_id = friend_id;
        send.channel = RequestChannel(channel);
        send.application_id = application_info.application_id;
        send.presence_group_id = application_info.presence_group_id;
        std::tie(send.target_name, send.target_language) = Fr::ScreenName(*target_name);
        std::tie(send.own_name, send.own_language) = Fr::ScreenName(*own_name);

        SendFriendRequestInBackground(std::move(send));
        R_SUCCEED();
    }

    /// Answer or withdraw a request: fire and forget, the PATCH is a network call and this is the
    /// game's thread.
    Result AnswerFriendRequest(Uid user, u64 request_id, const char* state) {
        R_TRY(RequireManager());

        if (!AvailableFor(user)) {
            LOG_DEBUG(Service_Friend, "(not served) called");
            R_SUCCEED();
        }

        Baas::RunInBackground([request_id, answer = std::string{state}] {
            Baas::AnswerFriendRequest(request_id, answer);
        });
        R_SUCCEED();
    }

    Result CancelFriendRequest(Uid user, u64 request_id) {
        R_RETURN(AnswerFriendRequest(user, request_id, "CANCELED"));
    }

    Result AcceptFriendRequest(Uid user, u64 request_id) {
        R_RETURN(AnswerFriendRequest(user, request_id, "AUTHORIZED"));
    }

    Result RejectFriendRequest(Uid user, u64 request_id) {
        R_RETURN(AnswerFriendRequest(user, request_id, "REJECTED"));
    }

    Result ReadFriendRequest(Uid user, u64 request_id) {
        R_TRY(RequireManager());

        if (!AvailableFor(user)) {
            LOG_DEBUG(Service_Friend, "(not served) called");
            R_SUCCEED();
        }

        Baas::RunInBackground([request_id] { Baas::ReadFriendRequest(request_id); });
        R_SUCCEED();
    }

    Result GetFacedFriendRequestRegistrationKey(
        Out<Fr::FacedFriendRequestRegistrationKey> out_key) {
        LOG_WARNING(Service_Friend, "(STUBBED) called");
        *out_key = {};
        R_SUCCEED();
    }

    Result AddFacedFriendRequest() {
        LOG_WARNING(Service_Friend, "(STUBBED) called");
        R_SUCCEED();
    }

    Result CancelFacedFriendRequest(Uid user, NetworkServiceAccountId friend_id) {
        LOG_WARNING(Service_Friend, "(STUBBED) called");
        R_SUCCEED();
    }

    Result GetFacedFriendRequestProfileImage(Out<s32> out_size) {
        LOG_WARNING(Service_Friend, "(STUBBED) called");
        *out_size = 0;
        R_SUCCEED();
    }

    Result GetFacedFriendRequestProfileImageFromPath(Out<s32> out_size) {
        LOG_WARNING(Service_Friend, "(STUBBED) called");
        *out_size = 0;
        R_SUCCEED();
    }

    Result SendFriendRequestWithExternalApplicationCatalogId(
        s32 channel, Uid user, NetworkServiceAccountId friend_id,
        Fr::ExternalApplicationCatalogId catalog_id,
        InLargeData<Fr::InAppScreenName, BufferAttr_HipcPointer> target_name,
        InLargeData<Fr::InAppScreenName, BufferAttr_HipcPointer> own_name) {
        R_TRY(RequireManager());

        if (!AvailableFor(user)) {
            LOG_DEBUG(Service_Friend, "(not served) called, channel={}", channel);
            R_SUCCEED();
        }

        Baas::FriendRequestSend send;
        send.target_id = friend_id;
        send.channel = RequestChannel(channel);
        send.catalog_id = fmt::format("{:016x}{:016x}", catalog_id.high, catalog_id.low);
        std::tie(send.target_name, send.target_language) = Fr::ScreenName(*target_name);
        std::tie(send.own_name, send.own_language) = Fr::ScreenName(*own_name);

        SendFriendRequestInBackground(std::move(send));
        R_SUCCEED();
    }

    Result SendFriendRequestWithApplicationInfoV2(
        s32 channel, Uid user, NetworkServiceAccountId friend_id,
        Fr::ApplicationInfoV2 application_info,
        InLargeData<Fr::InAppScreenName, BufferAttr_HipcPointer> target_name,
        InLargeData<Fr::InAppScreenName, BufferAttr_HipcPointer> own_name) {
        R_TRY(RequireManager());

        if (!AvailableFor(user)) {
            LOG_DEBUG(Service_Friend, "(not served) called, channel={}", channel);
            R_SUCCEED();
        }

        // The V2 send is the app route with the acd index the caller named (§A.4.1).
        Baas::FriendRequestSend send;
        send.target_id = friend_id;
        send.channel = RequestChannel(channel);
        send.application_id = application_info.application_id;
        send.acd_index = application_info.acd_index;
        send.presence_group_id = application_info.presence_group_id;
        std::tie(send.target_name, send.target_language) = Fr::ScreenName(*target_name);
        std::tie(send.own_name, send.own_language) = Fr::ScreenName(*own_name);

        SendFriendRequestInBackground(std::move(send));
        R_SUCCEED();
    }

    Result ResendFacedFriendRequest(Uid user, NetworkServiceAccountId friend_id) {
        LOG_WARNING(Service_Friend, "(STUBBED) called");
        R_SUCCEED();
    }

    Result SendFriendRequestWithNintendoNetworkIdInfo(
        Fr::MiiName own_mii_name, Fr::MiiImageUrlParam own_mii_image_url_param,
        Fr::MiiName other_mii_name, Fr::MiiImageUrlParam other_mii_image_url_param,
        s32 channel, Uid user, NetworkServiceAccountId friend_id) {
        R_TRY(RequireManager());

        if (!AvailableFor(user)) {
            LOG_DEBUG(Service_Friend, "(not served) called, channel={}", channel);
            R_SUCCEED();
        }

        // The NNID route carries the sender's own Mii name and image parameter -- block A of the
        // pair -- in both extras halves (§A.4.1).
        Baas::FriendRequestSend send;
        send.target_id = friend_id;
        send.channel = RequestChannel(channel);
        send.mii_name = Fr::Text(std::span<const char>{own_mii_name.name});
        send.mii_image_url_param = Fr::Text(std::span<const char>{own_mii_image_url_param.param});

        SendFriendRequestInBackground(std::move(send));
        R_SUCCEED();
    }

    Result GetSnsAccountLinkPageUrl(
        OutLargeData<Fr::WebPageUrl, BufferAttr_HipcMapAlias> out_url) {
        LOG_WARNING(Service_Friend, "(STUBBED) called");
        *out_url = {};
        R_SUCCEED();
    }

    Result UnlinkSnsAccount() {
        LOG_WARNING(Service_Friend, "(STUBBED) called");
        R_SUCCEED();
    }

    Result BlockUser(s32 reason, Uid user, NetworkServiceAccountId friend_id) {
        R_TRY(RequireManager());

        if (!AvailableFor(user)) {
            LOG_DEBUG(Service_Friend, "(not served) called, reason={}", reason);
            R_SUCCEED();
        }

        // BlockUser names the reason itself: 1 a bad friend request, 2 a bad friend (§B.4).
        R_UNLESS(reason == 1 || reason == 2, ResultInvalidArgument);
        R_RETURN(Block(friend_id, reason, std::nullopt));
    }

    Result BlockUserWithApplicationInfo(
        s32 reason, Uid user, NetworkServiceAccountId friend_id,
        Fr::ApplicationInfo application_info,
        InLargeData<Fr::InAppScreenName, BufferAttr_HipcPointer> name) {
        R_TRY(RequireManager());

        if (!AvailableFor(user)) {
            LOG_DEBUG(Service_Friend, "(not served) called, reason={}", reason);
            R_SUCCEED();
        }

        // A block from inside a title is IN_APP (3) whatever the caller passed, and carries the
        // title and the in-app name the blocked person went by there (§A.8).
        Baas::Route route;
        route.application_id = application_info.application_id;
        route.presence_group_id = application_info.presence_group_id;
        std::tie(route.name, route.language) = Fr::ScreenName(*name);
        R_RETURN(Block(friend_id, 3, std::move(route)));
    }

    Result BlockUserWithApplicationInfoV2(
        s32 reason, Uid user, NetworkServiceAccountId friend_id,
        Fr::ApplicationInfoV2 application_info,
        InLargeData<Fr::InAppScreenName, BufferAttr_HipcPointer> name) {
        R_TRY(RequireManager());

        if (!AvailableFor(user)) {
            LOG_DEBUG(Service_Friend, "(not served) called, reason={}", reason);
            R_SUCCEED();
        }

        Baas::Route route;
        route.application_id = application_info.application_id;
        route.acd_index = application_info.acd_index;
        route.presence_group_id = application_info.presence_group_id;
        std::tie(route.name, route.language) = Fr::ScreenName(*name);
        R_RETURN(Block(friend_id, 3, std::move(route)));
    }

    /// Fire and forget, as the other writes are: the POST is a network call and this is the
    /// game's thread. The library caches the block before the request leaves, so a title reading
    /// the list or the relationship straight after sees it, and re-syncs blocks, friends and the
    /// request boxes after the write.
    Result Block(NetworkServiceAccountId friend_id, s32 reason, std::optional<Baas::Route> route) {
        R_UNLESS(friend_id != 0, ResultInvalidArgument);

        Baas::RunInBackground([friend_id, reason, route = std::move(route)] {
            if (const int error = Baas::BlockUser(friend_id, reason, route); error != Baas::Ok) {
                LOG_WARNING(Service_Friend, "[OpenPak] Blocking failed: 2121-{:04}", error);
            }
        });
        R_SUCCEED();
    }

    Result UnblockUser(Uid user, NetworkServiceAccountId friend_id) {
        R_TRY(RequireManager());

        if (!AvailableFor(user)) {
            LOG_DEBUG(Service_Friend, "(not served) called");
            R_SUCCEED();
        }

        Baas::RunInBackground([friend_id] {
            if (const int error = Baas::UnblockUser(friend_id); error != Baas::Ok) {
                LOG_WARNING(Service_Friend, "[OpenPak] Unblocking failed: 2121-{:04}", error);
            }
        });
        R_SUCCEED();
    }

    Result GetProfileExtraFromFriendCode(
        OutLargeData<Fr::ProfileExtraImpl, BufferAttr_HipcPointer> out_profile) {
        LOG_WARNING(Service_Friend, "(STUBBED) called");
        *out_profile = {};
        R_SUCCEED();
    }

    Result DeletePlayHistory(Uid user) {
        LOG_WARNING(Service_Friend, "(STUBBED) called");
        R_SUCCEED();
    }

    Result ChangePresencePermission(s32 permission_value, Uid user) {
        R_TRY(RequireManager());

        const char* value = nullptr;
        switch (permission_value) {
        case 0:
            value = "SELF";
            break;
        case 1:
            value = "FAVORITE_FRIENDS";
            break;
        case 2:
            value = "FRIENDS";
            break;
        default:
            R_THROW(ResultInvalidArgument);
        }

        if (AvailableFor(user)) {
            Baas::RunInBackground([body = Baas::UserPatchBody("/permissions/presence", value)] {
                Baas::PatchUser(body);
            });
        }
        R_SUCCEED();
    }

    Result ChangeFriendRequestReception(bool reception, Uid user) {
        R_TRY(RequireManager());

        if (AvailableFor(user)) {
            Baas::RunInBackground(
                [body = Baas::UserPatchBody("/permissions/friendRequestReception", reception)] {
                    Baas::PatchUser(body);
                });
        }
        R_SUCCEED();
    }

    Result ChangePlayLogPermission(s32 permission_value, Uid user) {
        R_TRY(RequireManager());
        R_UNLESS(permission_value == 1 || permission_value == 2 || permission_value == 3 ||
                     permission_value == 5,
                 ResultInvalidArgument);

        if (AvailableFor(user)) {
            // The chosen group keeps the log the server already holds; the other three are
            // emptied, which is what makes the group the permission (§A.6).
            const auto setting = Baas::OwnUserSetting();
            const std::string play_log =
                setting && !setting->play_log_text.empty() ? setting->play_log_text : "[]";
            Baas::RunInBackground(
                [body = Baas::PlayLogPermissionBody(permission_value, play_log)] {
                    Baas::PatchUser(body);
                });
        }
        R_SUCCEED();
    }

    Result IssueFriendCode(Uid user) {
        R_TRY(RequireManager());

        if (AvailableFor(user)) {
            Baas::RunInBackground([] { Baas::IssueFriendCode(); });
        }
        R_SUCCEED();
    }

    Result ClearPlayLog(Uid user) {
        R_TRY(RequireManager());

        if (AvailableFor(user)) {
            // The same four ops with an empty log in the group that carries the permission.
            const auto setting = Baas::OwnUserSetting();
            Baas::RunInBackground(
                [body = Baas::PlayLogPermissionBody(setting ? setting->play_log_permission : 0,
                                                    "[]")] { Baas::PatchUser(body); });
        }
        R_SUCCEED();
    }

    // 30900 / 30901 are written by hand: their in-buffers are one pointer (X) and two map-alias
    // (A) descriptors, and the generated serializer counts in-buffers across both kinds, which
    // would read the description from the wrong descriptor.
    //
    // Raw data, sorted by alignment: the bool at +0, the Uid at +8, the ApplicationInfo at +0x18.

    void SendFriendInvitation(HLERequestContext& ctx) {
        const u8* raw = RawData(ctx);

        Uid user{};
        Fr::ApplicationInfo info{};
        std::memcpy(&user, raw + 0x08, sizeof(user));
        std::memcpy(&info, raw + 0x18, sizeof(info));

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(SendInvitation(ctx, user, info.application_id, 0, info.presence_group_id,
                               raw[0] != 0));
    }

    void SendFriendInvitationV2(HLERequestContext& ctx) {
        const u8* raw = RawData(ctx);

        Uid user{};
        Fr::ApplicationInfoV2 info{};
        std::memcpy(&user, raw + 0x08, sizeof(user));
        std::memcpy(&info, raw + 0x18, sizeof(info));

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(SendInvitation(ctx, user, info.application_id, info.acd_index,
                               info.presence_group_id, raw[0] != 0));
    }

    static const u8* RawData(HLERequestContext& ctx) {
        return reinterpret_cast<const u8*>(ctx.CommandBuffer() + ctx.GetDataPayloadOffset() + 2);
    }

    /// POST /v2/invitation_groups (invitations doc §3c). The ids are the friends' own BAAS user
    /// ids, which is what the guest's friend list serves, so they go out as they arrive.
    Result SendInvitation(HLERequestContext& ctx, const Uid& user, u64 application_id,
                          u8 acd_index, u64 presence_group_id, bool application_id_match) {
        R_TRY(RequireManager());

        std::vector<u64> friend_ids;
        if (!ctx.BufferDescriptorX().empty()) {
            const auto ids = ctx.ReadBufferX(0);
            friend_ids.resize(ids.size() / sizeof(u64));
            std::memcpy(friend_ids.data(), ids.data(), friend_ids.size() * sizeof(u64));
        }

        Fr::FriendInvitationGameModeDescription description{};
        std::span<const u8> description_bytes;
        std::span<const u8> application_data;
        if (ctx.BufferDescriptorA().size() >= 2) {
            description_bytes = ctx.ReadBufferA(0);
            application_data = ctx.ReadBufferA(1);
        }
        std::memcpy(&description, description_bytes.data(),
                    (std::min)(description_bytes.size(), sizeof(description)));

        // The module refuses anything outside 1..16 receivers or 0x400 bytes of data.
        R_UNLESS(!friend_ids.empty() && friend_ids.size() <= Fr::kMaxReceivers &&
                     application_data.size() <= Fr::kApplicationDataSize,
                 ResultInvalidArgument);

        if (!AvailableFor(user)) {
            R_SUCCEED();
        }

        std::vector<std::string> receivers;
        receivers.reserve(friend_ids.size());
        for (const u64 id : friend_ids) {
            receivers.push_back(fmt::format("{:016x}", id));
        }

        // Fire and forget: the POST is a network call and this is the game's thread.
        Baas::RunInBackground([receivers = std::move(receivers), application_id, acd_index,
                               presence_group_id,
                               data = std::vector<u8>(application_data.begin(),
                                                      application_data.end()),
                               messages = Fr::InvitationMessages(description),
                               application_id_match] {
            Baas::SendInvitation(receivers, application_id, acd_index, presence_group_id, data,
                                 messages, application_id_match);
        });
        R_SUCCEED();
    }

    Result ReadFriendInvitation(Uid user,
                                InArray<u64, BufferAttr_HipcPointer> invitation_ids) {
        R_TRY(RequireManager());

        // Fire and forget: the mark-read is a network call and this is the game's thread.
        std::vector<u64> ids(invitation_ids.begin(), invitation_ids.end());
        Baas::RunInBackground([ids = std::move(ids)] { ReadNativeInvitations(ids); });
        R_SUCCEED();
    }

    Result ReadAllFriendInvitations(Uid user) {
        R_TRY(RequireManager());

        Baas::RunInBackground([] { ReadNativeInvitations({}); });
        R_SUCCEED();
    }

    /// The guest read some invitations (or all of them, for an empty list): the same dismissal
    /// the native inbox makes, after which the pushed unread count is stale.
    static void ReadNativeInvitations(const std::vector<u64>& ids) {
        FriendsSession::ForgetPushedUnread();
        for (const auto& invitation : Session::Invitations()) {
            const u64 id = std::strtoull(invitation.id.c_str(), nullptr, 10);
            if (ids.empty() || std::find(ids.begin(), ids.end(), id) != ids.end()) {
                Session::DismissInvitation(invitation.id);
            }
        }
    }

    // ---- 4xxxx: system ports ----

    Result DeleteFriendListCache(Uid user) {
        LOG_WARNING(Service_Friend, "(STUBBED) called");
        R_RETURN(RequireSystem());
    }

    Result DeleteBlockedUserListCache(Uid user) {
        LOG_WARNING(Service_Friend, "(STUBBED) called");
        R_RETURN(RequireSystem());
    }

    Result DeleteNetworkServiceAccountCache(Uid user) {
        LOG_WARNING(Service_Friend, "(STUBBED) called");
        R_RETURN(RequireSystem());
    }

    const u32 permission;
    KernelHelpers::ServiceContext service_context;
    std::shared_ptr<FriendsSession> session;
    Kernel::KEvent* completion_event;

    /// The event as work on the friends worker sees it: that work can outlive this service, so it
    /// raises the event through the session's signalling thread, and finds it detached once the
    /// service has closed it.
    std::shared_ptr<::OpenPak::HostEvent> completion;
};

} // namespace

std::shared_ptr<ServiceFrameworkBase> CreateFriendService(Core::System& system,
                                                         std::shared_ptr<FriendsSession> session,
                                                         std::string_view port_name) {
    return std::make_shared<FriendService>(system, std::move(session), PermissionOf(port_name));
}

std::shared_ptr<ServiceFrameworkBase> CreateNotificationService(
    Core::System& system, std::shared_ptr<FriendsSession> session, std::string_view port_name,
    const Common::UUID& user) {
    return std::make_shared<NotificationService>(system, std::move(session), user,
                                                 PermissionOf(port_name));
}

void OnApplicationStarted(Core::System& system, u64 program_id) {
    // The group a friend's game compares against is the NACP's; a title that declares none is its
    // own group, which presence falls back to when this is 0.
    u64 presence_group_id = 0;
    const FileSys::PatchManager patches{program_id, system.GetFileSystemController(),
                                        system.GetContentProvider()};
    if (const auto [nacp, icon] = patches.GetControlMetadata(); nacp) {
        const std::vector<u8> raw = nacp->GetRawBytes();
        constexpr std::size_t offset = offsetof(FileSys::RawNACP, presence_group_id);
        if (raw.size() >= offset + sizeof(presence_group_id)) {
            std::memcpy(&presence_group_id, raw.data() + offset, sizeof(presence_group_id));
        }
    }
    LOG_INFO(Service_Friend, "friends: application {:016X} started (presence group {:016X})",
             program_id, presence_group_id);
    Session::SetRunningApplication(program_id, presence_group_id);
}

void OnApplicationExited() {
    LOG_INFO(Service_Friend, "friends: the application exited");
    Session::SetRunningApplication(0, 0);
}

} // namespace Service::Friend

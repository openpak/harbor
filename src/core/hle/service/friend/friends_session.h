// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// The friends sysmodule's state for one emulation session: the signalling thread every guest
// event of the module is raised on, the INotificationService listeners, the Penne push consumer
// and the overlay notifier (the ovln messages overlayDisp turns into pop-ups).
//
// OpenPak raises its hooks on its own long-lived threads (the push connection, the friends
// worker). Those never touch a kernel object: they post to this session's AsyncWorker, whose one
// signalling thread belongs to this session's kernel. A session that has ended is simply gone and
// the post is dropped.

#pragma once

#include <array>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "common/common_types.h"
#include "common/uuid.h"
#include "core/hle/service/friend/friends_overlay_payloads.h"
#include "core/hle/service/psc/ovln/ovln_agent.h"
#include "core/openpak/async_worker.h"

namespace Core {
class System;
}

namespace Service::Friend {

/// One INotificationService, as the session's registry sees it.
class NotificationListener {
public:
    virtual ~NotificationListener() = default;
    /// On the signalling thread; `user` is the uid (RawString) the change is for.
    virtual void OnFriendListUpdate(const std::string& user) = 0;
    virtual void OnNewFriendRequest(const std::string& user) = 0;
};

class FriendsSession {
public:
    explicit FriendsSession(Core::System& system);
    ~FriendsSession();

    FriendsSession(const FriendsSession&) = delete;
    FriendsSession& operator=(const FriendsSession&) = delete;

    /// Makes `session` the one OpenPak's hooks reach (LoopProcess, once per emulation session),
    /// and installs the hooks the first time.
    static void Begin(const std::shared_ptr<FriendsSession>& session);

    /// The running session, or null.
    static std::shared_ptr<FriendsSession> Current();

    Core::System& System() {
        return system;
    }

    OpenPak::AsyncWorker& Worker() {
        return worker;
    }

    /// The registry holds at most 12 listeners, as the module does; one more is not notified.
    void Register(NotificationListener* listener);
    void Unregister(NotificationListener* listener);

    /// From any thread: queue the change for every listener of the active user.
    void PostFriendListUpdate();
    void PostNewFriendRequest();

    /// friends 20900 RequestListSummaryOverlayNotification: the 7001 summary of the active user's
    /// friends. From the IPC thread; the send happens on the signalling thread.
    void RequestListSummary();

    /// The 22010 count: the unread count the last invitation push carried, else what the native
    /// inbox holds.
    static int UnreadInvitations();
    /// The guest read invitations, or the inbox changed: the pushed count is stale.
    static void ForgetPushedUnread();

private:
    friend struct FriendsPush;

    /// On the signalling thread: one overlay notification on the module's sender.
    void SendOverlay(u32 tag, std::span<const u8> payload, std::string_view what);

    /// On the signalling thread: whether `user` is open in acc, and whether anybody is.
    bool IsUserOpen(const Common::UUID& user);
    bool IsAnyUserOpen();

    /// On the signalling thread: set:sys AccountNotificationSettings +0x14 for `user` (0 when
    /// the user has no record).
    u8 FriendPresenceOverlayPermission(const Common::UUID& user);

    Core::System& system;

    std::mutex registry_mutex;
    std::array<NotificationListener*, 12> registry{};

    /// Touched only on the signalling thread.
    std::shared_ptr<PSC::OvlnAgent::SenderState> ovln_sender;

    /// Last, so it is destroyed first: its signalling thread is joined before anything a posted
    /// task reads goes away.
    OpenPak::AsyncWorker worker;
};

/// The uid OpenPak speaks for (acc's open user, as the push registration's), or invalid.
Common::UUID PushUser();

/// The 16 guest bytes of a uid.
Overlay::UidBytes UidBytesOf(const Common::UUID& uid);

} // namespace Service::Friend

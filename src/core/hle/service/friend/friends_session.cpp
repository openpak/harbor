// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base): FriendsOverlayNotifier.cs and
// NotificationEventHandler.cs, with the gates of friends 22.5.0 (invite-re ovln-notes.md §2).

#include <algorithm>
#include <chrono>
#include <cstdlib>

#include <nlohmann/json.hpp>

#include "common/logging.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/service/acc/profile_manager.h"
#include "core/hle/service/friend/friend_pictures.h"
#include "core/hle/service/friend/friends_session.h"
#include "core/hle/service/psc/ovln/ovln_types.h"
#include "core/hle/service/set/system_settings_server.h"
#include "core/hle/service/sm/sm.h"
#include "core/openpak/baas.h"
#include "core/openpak/platform.h"
#include "core/openpak/push.h"
#include "core/openpak/session.h"

namespace Service::Friend {

namespace {

namespace Baas = OpenPak::Baas;

std::mutex current_mutex;
std::weak_ptr<FriendsSession> current;

std::mutex unread_mutex;
std::optional<int> pushed_unread;

std::int64_t Now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

const char* StatusName(PSC::OvlnStatus status) {
    switch (status) {
    case PSC::OvlnStatus::Success:
        return "ok";
    case PSC::OvlnStatus::QueueFullRejected:
        return "2025-0002";
    case PSC::OvlnStatus::InvalidArgument:
        return "2025-0003";
    case PSC::OvlnStatus::QueueFull:
        return "2025-0010 (queue full)";
    case PSC::OvlnStatus::SourceAlreadyAttached:
        return "2025-0011";
    case PSC::OvlnStatus::NoMessage:
        return "2025-0012";
    }
    return "?";
}

/// A hex id field as the module reads one; false when it is missing or not a hex string.
bool Hex(const Baas::Json& object, const char* key, u64& out) {
    const auto it = object.find(key);
    return it != object.end() && Baas::TryHex(*it, out);
}

} // namespace

Common::UUID PushUser() {
    const std::string id = OpenPak::Platform::ProfileId();
    if (id.empty()) {
        return {};
    }
    return Common::UUID{id};
}

Overlay::UidBytes UidBytesOf(const Common::UUID& uid) {
    Overlay::UidBytes bytes{};
    std::copy(uid.uuid.begin(), uid.uuid.end(), bytes.begin());
    return bytes;
}

// ------------------------------------------------------------------------------------------------
// The Penne push consumer (friends 22.5.0 dispatcher 0xA15C0). It runs on the push thread: it only
// parses and reads caches there, fetches pictures on the friends worker, and posts every send to
// the session's signalling thread.
// ------------------------------------------------------------------------------------------------

struct FriendsPush {
    static void OnPush(const std::string& kind, const std::string& body) {
        if (kind != "friend_invitation_received" && kind != "presence_updated" &&
            kind != "friend_request_received") {
            return;
        }
        const Baas::Json root = Baas::Json::parse(body, nullptr, false);
        if (!root.is_object()) {
            LOG_DEBUG(Service_Friend, "friends: {} push dropped (not JSON)", kind);
            return;
        }
        if (kind == "friend_invitation_received") {
            OnInvitation(root);
        } else if (kind == "presence_updated") {
            OnPresence(root);
        } else {
            OnFriendRequest(root);
        }
    }

    /// 7004 (handler 0x1a19a0): the count cache first, then the pop-up when the user is open or
    /// nobody is.
    static void OnInvitation(const Baas::Json& root) {
        const Common::UUID user = PushUser();
        if (user.IsInvalid()) {
            return;
        }
        const auto invitation = root.find("invitation");
        if (invitation == root.end() || !invitation->is_object()) {
            LOG_DEBUG(Service_Friend, "friends: invitation push dropped (no invitation)");
            return;
        }
        // id, invitation_group_id and created_at are JSON integers; sender_id and application_id
        // hex strings. Anything else drops the event, as the module's parser does.
        const auto id = invitation->find("id");
        const auto group = invitation->find("invitation_group_id");
        const auto created = root.find("created_at");
        u64 sender_id = 0;
        u64 application_id = 0;
        if (id == invitation->end() || !id->is_number_unsigned() || group == invitation->end() ||
            !group->is_number_unsigned() || created == root.end() || !created->is_number_integer() ||
            !Hex(*invitation, "sender_id", sender_id) ||
            !Hex(*invitation, "application_id", application_id)) {
            LOG_DEBUG(Service_Friend, "friends: invitation push dropped (a field is missing)");
            return;
        }
        const u64 invitation_id = id->get<u64>();
        const u64 group_id = group->get<u64>();
        const std::int64_t created_at = created->get<std::int64_t>();

        if (const auto unread = root.find("unread_count");
            unread != root.end() && unread->is_number_integer()) {
            std::scoped_lock lock{unread_mutex};
            pushed_unread = unread->get<int>();
        }

        // overlayDisp drops the invitation pop-up unless created_at is within 60 s of its network
        // clock, which on this host is the host's clock.
        if (const std::int64_t skew = Now() - created_at; skew >= 60 || skew < 0) {
            LOG_WARNING(Service_Friend,
                        "friends: invitation created {} s from this host's clock; overlayDisp "
                        "will drop its pop-up",
                        skew);
        }

        const auto payload = Overlay::InvitationPayload(UidBytesOf(user), invitation_id, group_id,
                                                        sender_id, application_id, created_at);
        // overlayDisp drops the pop-up when friend:v 10110 has no picture of the sender: have it
        // before the message goes.
        Baas::RunInBackground([user, sender_id, payload] {
            PrefetchPicture(sender_id);
            PostToSession([user, payload](FriendsSession& session) {
                if (!session.IsUserOpen(user) && session.IsAnyUserOpen()) {
                    LOG_INFO(Service_Friend, "friends: invitation received; no overlay "
                                             "notification (another user is open)");
                    return;
                }
                session.SendOverlay(Overlay::TagFriendInvitation, payload, "invitation");
            });
        });
    }

    /// 7002 (handler 0x1a1790): an open user's cached friend went from offline to online or
    /// playing, and the user's friend presence overlay permission allows it.
    static void OnPresence(const Baas::Json& root) {
        const Common::UUID user = PushUser();
        if (user.IsInvalid()) {
            return;
        }
        const auto value = root.find("value");
        u64 friend_id = 0;
        if (value == root.end() || !value->is_object() || !Hex(*value, "userId", friend_id)) {
            LOG_DEBUG(Service_Friend, "friends: presence push dropped (no value.userId)");
            return;
        }
        const auto state_field = value->find("state");
        const std::string state = state_field != value->end() && state_field->is_string()
                                      ? state_field->get<std::string>()
                                      : std::string{};
        const bool online = state == "ONLINE" || state == "PLAYING";

        // The cache as it was before this event: the refresh it causes runs after this hook.
        const auto cached = Baas::FindFriend(friend_id);
        if (!cached || cached->state != 0 || !online) {
            return;
        }
        const bool favourite = cached->is_favorite;
        const std::int64_t offline = Now() - cached->updated_at;
        const auto payload = Overlay::FriendOnlinePayload(UidBytesOf(user), friend_id, offline);

        Baas::RunInBackground([user, friend_id, favourite, offline, payload] {
            PrefetchPicture(friend_id);
            PostToSession([user, favourite, offline, payload](FriendsSession& session) {
                if (!session.IsUserOpen(user)) {
                    LOG_DEBUG(Service_Friend, "friends: a friend came online; no overlay "
                                              "notification (the user is not open)");
                    return;
                }
                const u8 permission = session.FriendPresenceOverlayPermission(user);
                if (!Overlay::FriendOnlineAllowed(permission, favourite)) {
                    LOG_DEBUG(Service_Friend,
                              "friends: a friend came online; overlay permission {} holds it back",
                              permission);
                    return;
                }
                // overlayDisp itself shows nothing for a friend offline 1799 s or less.
                session.SendOverlay(Overlay::TagFriendOnline, payload,
                                    fmt::format("friend online after {} s offline", offline));
            });
        });
    }

    /// 7003 (handler 0x1a1ad0): an open user received a request that was not made in person.
    /// overlayDisp checks the user's friend-request notification flag itself.
    static void OnFriendRequest(const Baas::Json& root) {
        const Common::UUID user = PushUser();
        if (user.IsInvalid()) {
            return;
        }
        // The received parser fails the whole event without $.value.channels[0].
        const auto value = root.find("value");
        if (value == root.end() || !value->is_object()) {
            LOG_DEBUG(Service_Friend, "friends: friend request push dropped (no value)");
            return;
        }
        const auto channels = value->find("channels");
        if (channels == value->end() || !channels->is_array() || channels->empty() ||
            !(*channels)[0].is_string()) {
            LOG_DEBUG(Service_Friend, "friends: friend request push dropped (no channels[0])");
            return;
        }
        if ((*channels)[0].get<std::string>() == Baas::Channels[0]) { // NX_FACED
            return;
        }
        const auto payload = Overlay::FriendRequestPayload(UidBytesOf(user));
        PostToSession([user, payload](FriendsSession& session) {
            if (!session.IsUserOpen(user)) {
                return;
            }
            session.SendOverlay(Overlay::TagFriendRequest, payload, "friend request");
        });
    }

    /// Blocking, on the friends worker: the picture 10110 will be asked for, looked up first when
    /// nobody has the user cached.
    static void PrefetchPicture(u64 id) {
        std::string url = Pictures::UrlOf(id);
        if (url.empty()) {
            Baas::WarmUsers({id});
            url = Pictures::UrlOf(id);
        }
        if (url.empty() || !Pictures::Fetch(url)) {
            // Then overlayDisp drops the pop-up, as a console's does without the picture.
            LOG_DEBUG(Service_Friend, "friends: no picture for an overlay notification");
        }
    }

    template <typename F>
    static void PostToSession(F&& fn) {
        const auto session = FriendsSession::Current();
        if (!session) {
            return;
        }
        session->Worker().Post([weak = std::weak_ptr{session}, fn = std::forward<F>(fn)] {
            if (const auto alive = weak.lock()) {
                fn(*alive);
            }
        });
    }
};

// ------------------------------------------------------------------------------------------------
// FriendsSession
// ------------------------------------------------------------------------------------------------

FriendsSession::FriendsSession(Core::System& system_)
    : system{system_}, worker{system_, "friend:OpenPak"} {}

FriendsSession::~FriendsSession() = default;

void FriendsSession::Begin(const std::shared_ptr<FriendsSession>& session) {
    {
        std::scoped_lock lock{current_mutex};
        current = session;
    }

    static std::once_flag hooked;
    std::call_once(hooked, [] {
        // The module fills the notification queue from its push handlers and from every list
        // sync that finds a change: a changed friend list is a list update, a request box that
        // grew is a new friend request.
        Baas::SetFriendListChangedHook([] {
            if (const auto s = Current()) {
                s->PostFriendListUpdate();
            }
        });
        Baas::SetFriendRequestArrivedHook([] {
            if (const auto s = Current()) {
                s->PostNewFriendRequest();
            }
        });
        // An invitation has no event type of its own in this queue. The list update is what
        // sends a system screen back to the service, where 22010 has the new count.
        OpenPak::Session::SetInvitationsChangedHook([] {
            ForgetPushedUnread();
            if (const auto s = Current()) {
                s->PostFriendListUpdate();
            }
        });
        OpenPak::Push::SetHook("friends", [](const std::string& kind, const std::string& body) {
            try {
                FriendsPush::OnPush(kind, body);
            } catch (const std::exception& e) {
                // The module drops an event that does not parse; so does this.
                LOG_DEBUG(Service_Friend, "friends: {} push dropped ({})", kind, e.what());
            }
        });
        // Test runs feed pushes from files (OPENPAK_SIMULATE_PUSH_DIR) with no connection.
        OpenPak::Push::StartSimulated();
    });
}

std::shared_ptr<FriendsSession> FriendsSession::Current() {
    std::scoped_lock lock{current_mutex};
    return current.lock();
}

void FriendsSession::Register(NotificationListener* listener) {
    std::scoped_lock lock{registry_mutex};
    // With no room left the module reports nothing either.
    const auto free = std::find(registry.begin(), registry.end(), nullptr);
    if (free != registry.end()) {
        *free = listener;
    }
}

void FriendsSession::Unregister(NotificationListener* listener) {
    std::scoped_lock lock{registry_mutex};
    const auto found = std::find(registry.begin(), registry.end(), listener);
    if (found != registry.end()) {
        *found = nullptr;
    }
}

void FriendsSession::PostFriendListUpdate() {
    worker.Post([this, user = OpenPak::Platform::ProfileId()] {
        if (user.empty()) {
            return;
        }
        std::scoped_lock lock{registry_mutex};
        for (NotificationListener* listener : registry) {
            if (listener != nullptr) {
                listener->OnFriendListUpdate(user);
            }
        }
    });
}

void FriendsSession::PostNewFriendRequest() {
    worker.Post([this, user = OpenPak::Platform::ProfileId()] {
        if (user.empty()) {
            return;
        }
        std::scoped_lock lock{registry_mutex};
        for (NotificationListener* listener : registry) {
            if (listener != nullptr) {
                listener->OnNewFriendRequest(user);
            }
        }
    });
}

void FriendsSession::RequestListSummary() {
    // ponytail: the module sends 7001 at the end of a list and count sync it runs for this
    // request (the trigger is inferred from the command's name); the counts here come from the
    // caches the heartbeat keeps warm instead.
    const Common::UUID user = PushUser();
    if (user.IsInvalid() || !Baas::Ready() || !OpenPak::Session::Linked({})) {
        LOG_DEBUG(Service_Friend, "friends: list summary requested; no overlay notification "
                                  "(no open user signed in)");
        return;
    }

    const auto friends = Baas::Friends();
    const auto online = static_cast<s32>(std::count_if(
        friends->begin(), friends->end(), [](const Baas::Friend& one) { return one.state != 0; }));
    s32 requests_unread = 0;
    s32 requests_read = 0;
    for (const Baas::Request& request : *Baas::InboxRequests()) {
        ++(request.read ? requests_read : requests_unread);
    }
    const s32 invitations = UnreadInvitations();

    const auto payload = Overlay::ListSummaryPayload(UidBytesOf(user), online, requests_unread,
                                                     requests_read, invitations);
    FriendsPush::PostToSession([user, payload, online, requests_unread, requests_read,
                                invitations](FriendsSession& session) {
        if (!session.IsUserOpen(user)) {
            return;
        }
        session.SendOverlay(Overlay::TagListSummary, payload,
                            fmt::format("summary: {} online, {}+{} requests, {} invitations",
                                        online, requests_unread, requests_read, invitations));
    });
}

int FriendsSession::UnreadInvitations() {
    {
        std::scoped_lock lock{unread_mutex};
        if (pushed_unread) {
            return *pushed_unread;
        }
    }
    return static_cast<int>(OpenPak::Session::Invitations().size());
}

void FriendsSession::ForgetPushedUnread() {
    std::scoped_lock lock{unread_mutex};
    pushed_unread.reset();
}

void FriendsSession::SendOverlay(u32 tag, std::span<const u8> payload, std::string_view what) {
    auto& agent = PSC::GetOvlnAgent();
    const PSC::OverlayNotification message = PSC::MakeNotification(tag, payload);
    const u64 tick = system.CoreTiming().GetClockTicks();

    PSC::OvlnStatus status = PSC::OvlnStatus::InvalidArgument;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!ovln_sender) {
            PSC::SourceName name{};
            constexpr std::string_view source = "overlay";
            std::copy(source.begin(), source.end(), name.name.begin());
            if (agent.OpenSender(name, Overlay::QueueLength, ovln_sender) !=
                PSC::OvlnStatus::Success) {
                ovln_sender.reset();
                break;
            }
        }
        status = agent.Send(*ovln_sender, Overlay::SendOption, message, tick);
        if (status != PSC::OvlnStatus::InvalidArgument) {
            break;
        }
        // A new ovln session closed this sender (OvlnAgent::Reset): open another, once.
        ovln_sender.reset();
    }

    LOG_INFO(Service_Friend, "friends: overlay notification {} sent ({}) -> {}", tag, what,
             StatusName(status));
}

bool FriendsSession::IsUserOpen(const Common::UUID& user) {
    const auto open = system.GetProfileManager().GetOpenUsers();
    return std::find(open.begin(), open.end(), user) != open.end();
}

bool FriendsSession::IsAnyUserOpen() {
    return system.GetProfileManager().GetOpenUserCount() > 0;
}

u8 FriendsSession::FriendPresenceOverlayPermission(const Common::UUID& user) {
    const auto set_sys =
        system.ServiceManager().GetService<Set::ISystemSettingsServer>("set:sys", true);
    if (!set_sys) {
        return 0;
    }
    std::array<Set::AccountNotificationSettings, 8> settings{};
    s32 count = 0;
    if (set_sys->GetAccountNotificationSettings(
            &count, std::span<Set::AccountNotificationSettings>{settings}).IsError()) {
        return 0;
    }
    for (s32 i = 0; i < count && i < static_cast<s32>(settings.size()); ++i) {
        if (settings[i].uid == user) {
            return static_cast<u8>(settings[i].friend_presence_permission);
        }
    }
    return 0;
}

} // namespace Service::Friend

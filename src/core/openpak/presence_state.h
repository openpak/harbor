// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later
// Derived from openpak-client 3fd9ba7 (presence_state.h).

// What the running game said about itself through friend:u, per profile: whether an online-play
// session is declared open (10600 / 10601, or the +0x18 byte of UpdateUserPresence 10610), and
// its app-field pairs as the JSON object the friends module publishes. Ported from the Ryujinx
// reference's Ryujinx.OpenPak/OpenPakPresence.cs, with the presence-body rules of its session
// (presence doc §2.3) alongside.
//
// The emulator's friend service writes it and the session that PATCHes presence reads it; neither
// can see the other, so it meets here. It knows nothing of any emulator: the host tells it which
// title runs and that title's NACP presence group, and names profiles by whatever string it
// already uses for them (the signed-in profile's Uid as hex, in the reference).

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "core/openpak/friend_structs.h"

namespace OpenPak::Friends {

/// One profile's declaration, for the title that made it.
struct DeclaredPresence {
    std::uint64_t title_id = 0;
    bool session_open = false;
    std::string app_field = "{}";
    /// When the state or the blob last changed (Unix seconds), as the module restamps its slot.
    std::int64_t updated_at = 0;
};

/// What presence publishes right now: the state string (INACTIVE / ONLINE / PLAYING) and the
/// `friends` extras.
struct PresencePublication {
    std::string state = "INACTIVE";
    std::uint64_t application_id = 0;
    std::uint64_t presence_group_id = 0;
    std::string app_field = "{}";
};

/// The online-play declaration byte of UpdateUserPresence (+0x18).
enum : std::uint8_t {
    DeclarationNone = 0,
    DeclarationOpen = 1,
    DeclarationClose = 2,
};

class PresenceState {
public:
    /// Unix seconds now; replaceable so tests can pin the stamp.
    using Clock = std::function<std::int64_t()>;

    explicit PresenceState(Clock clock = {});

    /// The process-wide state the friend service and the publishing session share.
    static PresenceState& Instance();

    /// The running application's id and its NACP presence group, as the module resolves them
    /// from the calling process. The host sets it on launch and sets (0, 0) when nothing runs.
    void SetRunningApplication(std::uint64_t application_id, std::uint64_t presence_group_id);

    /// What presence publishes as `appInfo:appId` and `appInfo:presenceGroupId`. The group is
    /// the NACP's, which is what a friend's game compares against; a title that declares none
    /// is its own group. (0, 0) when nothing runs.
    std::pair<std::uint64_t, std::uint64_t> Application() const;

    /// The caller's own presence group, which the module compares a friend's against for the
    /// same-app filters and the blob privacy rule. 0 when nothing runs.
    std::uint64_t OwnPresenceGroupId() const { return Application().second; }

    /// The declaration for this profile while `title_id` runs (the running title when omitted).
    /// A declaration left by another title does not carry over: a launch is a fresh ONLINE with
    /// "{}", as on a console.
    DeclaredPresence For(const std::string& profile_id, std::uint64_t title_id) const;
    DeclaredPresence For(const std::string& profile_id) const;

    /// One UpdateUserPresence for `title_id`. `declaration` is the struct's +0x18 byte: 1 opens
    /// the session, 2 closes it, anything else leaves it as it was -- the module keeps it sticky
    /// across commits that only change the description. An absent `app_field` keeps the pairs
    /// already declared, which is what the standalone Declare commands (10600/10601) do.
    ///
    /// The module publishes on a change of state or blob and on nothing else, and restamps its
    /// slot's time when one of them changes; so does this. Returns whether anything changed, and
    /// raises the changed callback (outside the lock) when it did, so it is published now rather
    /// than at the next beat.
    bool Update(const std::string& profile_id, std::uint64_t title_id, std::uint8_t declaration,
                std::optional<std::string> app_field);

    /// 10600 / 10601 for the running title: the session flag alone, the pairs kept.
    bool DeclareOpenOnlinePlaySession(const std::string& profile_id);
    bool DeclareCloseOnlinePlaySession(const std::string& profile_id);

    /// 10610 for the running title: the declaration byte and the whole blob, the blob turned into
    /// the appField object (BlobToAppField).
    bool UpdateUserPresence(const std::string& profile_id, const UserPresenceImpl& presence);

    /// What this console would publish right now for the profile: PLAYING while the running
    /// title has declared an online-play session, ONLINE while one runs without, and INACTIVE
    /// (no title, "{}") when none does.
    PresencePublication Current(const std::string& profile_id) const;

    /// The profile's own presence slot as 20600 returns it.
    UserPresenceViewImpl OwnPresenceView(const std::string& profile_id) const;

    /// Called after every change, outside the lock; empty to stop.
    void SetChangedCallback(std::function<void()> callback);

    /// Drops every declaration (sign-out, tests). The running application is kept.
    void Clear();

private:
    std::int64_t Now() const;
    DeclaredPresence ForLocked(const std::string& profile_id, std::uint64_t title_id) const;

    Clock clock_;
    mutable std::mutex mutex_;
    std::uint64_t application_id_ = 0;
    std::uint64_t presence_group_id_ = 0;
    std::map<std::string, DeclaredPresence> states_;

    std::mutex callback_mutex_;
    std::function<void()> changed_;
};

/// The JSON-Patch body the module publishes (presence doc §2.3): the state replaced first, then
/// the four `friends` extras added in its order. appField is always there, "{}" when the game
/// set nothing, as a JSON object inside a JSON string; acdIndex is an unquoted number because the
/// console writes it as one; the ids are 16 lowercase hex digits.
std::string PresencePatch(const PresencePublication& publication);

} // namespace OpenPak::Friends

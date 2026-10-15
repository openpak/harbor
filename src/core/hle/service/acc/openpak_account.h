// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// acc's side of the OpenPak session (core/openpak/session.h): who the active user is, the network
// half of the Nintendo Account procedures, and the profile a linked user takes from its account.
//
// The blocking calls make network round trips and run on an OpenPak::AsyncWorker thread, never
// on a guest IPC thread. Nothing here logs an id, a token, a name or a URL past its host.

#pragma once

#include <array>
#include <chrono>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"
#include "common/uuid.h"
#include "core/hle/service/acc/nintendo_account_oauth.h"
#include "core/hle/result.h"

namespace Core {
class System;
}

namespace Service::Account {

class ProfileManager;

namespace OpenPakAccount {

/// The uid as the OpenPak session names users: 32 hex digits.
std::string UidString(const Common::UUID& uid);

/// A nickname as a profile's 32-byte username, never cutting a UTF-8 sequence in half.
std::array<u8, 32> ToUsername(std::string_view nickname);

/// Whether an OpenPak console server is configured for this process.
bool Enabled();

/// Points the session's "active user" at the user acc has open. Called once at acc start.
void InstallProfileSource(ProfileManager& profile_manager);

/// Binds the next sign-in to the title asking acc for an application manager (aauth), as a
/// console's login names the running game.
void BindRunningApplication(Core::System& system);

/// The user's profile picture file in the system save, where IProfile reads it from.
std::filesystem::path ProfileImagePath(const Common::UUID& uid);

/// What a procedure keeps once its response is applied.
struct NintendoAccountLink {
    u64 nintendo_account_id = 0;
    u64 network_service_account_id = 0;
    std::string nickname;
    /// 256x256 baseline JPEG, or empty when the account has no picture to fetch.
    std::vector<u8> image;
    /// The user was linked to another network service account before (linkage 101).
    bool replaced = false;
};

/// The code from the Login applet's callback, applied: dauth device token, code exchange,
/// users/me, then federation of `uid`'s device account to that account. `uid` may name a user
/// that does not exist yet (registration creates it after). `others` are the other users on this
/// console, to refuse an account one of them already holds. Blocking.
Result ApplyCode(const std::stop_token& stop, const std::string& code,
                 const std::string& verifier, const std::string& uid,
                 const std::vector<std::string>& others, NintendoAccountLink& out);

/// After a link made outside the Login applet (the setup's account, OnFirmwareUserRegistered):
/// users/me read with that link's NA id_token, its names kept for 136 as ApplyCode keeps them.
/// Blocking.
void KeepNamesOfLink(const std::string& uid, const std::string& id_token);

/// IAdministrator 250's rule, which 120/136 and ns:ec 9/11 follow too: the user is the active
/// one and its sign-in is linked to a Nintendo Account.
bool IsActiveLinked(const Common::UUID& uid);

/// ns:ec 9/11: the token HOME and systemWeb hand the Lhub and shop pages, under the 250 rule.
/// The module's answers otherwise: 2124-0200 (no network service account) or 2124-0220 (linked
/// with Nintendo Account id 0). The token is the user's BAAS id_token: OpenPak's pages accept the
/// BAAS token of a linked user.
Result LinkedIdToken(const Common::UUID& uid, std::string& token);

/// The Nintendo Account behind a linked user, as its last sign-in in this process reported it.
struct NintendoAccountInfo {
    u64 id = 0;
    std::string nickname;
    std::string birthday;
    std::string country;
    /// users/me's screenName, email and loginId as of the user's firmware link; empty for a user
    /// linked another way (Harbor has no users/me for it).
    NintendoAccountOAuth::NasUserNames names;
};
/// Nothing when the user is not signed in and linked here.
std::optional<NintendoAccountInfo> NintendoAccount(const std::string& uid);

/// A picture from OpenPak, re-encoded as the 256x256 baseline JPEG qlaunch can draw (a
/// progressive one shows as a white circle). Empty on failure. Blocking.
std::vector<u8> FetchProfileImage(const std::string& url);

/// Sign `uid` in (blocking) and, when it is linked, queue the account's nickname and picture for
/// its local profile. Used by the acc commands that ensure a token.
bool EnsureAndAdopt(const std::string& uid);

/// EnsureAndAdopt on a thread of its own, at most once at a time per user. Never blocks.
void EnsureAndAdoptInBackground(const std::string& uid);

/// For a user Harbor created from the OpenPak account (not the firmware's setup, where the person
/// picks a picture): once linked, the account's nickname and picture are queued for it, or a plain
/// generated avatar when the account has no picture, so it never shows the placeholder. Returns at
/// once.
void AdoptNewUserInBackground(const std::string& uid);

/// Whether the user is signed in on the console server, starting the background sign-in and
/// waiting for it up to `max` when it is not yet. False when it is not done by then or failed.
bool SignedInWithin(const std::string& uid, std::chrono::milliseconds max);

/// Queue a profile change for the IPC thread: the worker never touches ProfileManager itself.
void QueueAdoption(const Common::UUID& uid, std::string nickname, std::vector<u8> image);

/// Apply what was queued. Called from acc's IPC handlers before they read a profile.
void ApplyPendingAdoptions(ProfileManager& profile_manager);

/// Whether `uid`'s profile has been adopted from its account in this process.
bool Adopted(const std::string& uid);

} // namespace OpenPakAccount

} // namespace Service::Account

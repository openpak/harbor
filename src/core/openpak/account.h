// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// The OpenPak account Harbor's setup signs in to (an owner-approved exception to "the firmware
// does the account UI", for phone usability), and the automatic link of the first firmware user
// to it.
//
// Everything goes through the website's account API (NetworkProfile::Site() + /api/v1), with the
// public roots:
//   POST /api/v1/register                {email, password, display_name, country, birthday}
//   POST /api/v1/token                   {email, password} -> {token, expires_at} (30 days)
//   GET  /api/v1/me                      email_verified, display_name
//   POST /api/v1/me/verify/resend
//   POST /api/v1/me/switch/link          {client_id} -> {id_token} (an NA id_token, 15 minutes)
//   DELETE /api/v1/token
// Only the bearer is kept, in the secret store (never a file, never logged); the password is never
// stored. Blocking calls: run them off any UI or guest IPC thread.

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace OpenPak::Account {

enum class Status {
    Ok,
    InvalidCredentials, ///< wrong email or password
    TwoFactorRequired,  ///< the account has two-step verification: sign in on the firmware's page
    RateLimited,
    Suspended,
    RegistrationsClosed,
    SignedOut, ///< no bearer, or the server no longer accepts it (it was forgotten)
    Offline,   ///< the website could not be reached
    Failed,
};

/// Signs in and keeps the bearer. Ok also for an address not verified yet (see Me).
Status SignIn(std::string_view email, std::string_view password);

/// Asks for an account. The server answers the same whether or not the address can be used (a
/// verification mail is sent when it can), so Ok means "asked", and SignIn tells the rest.
Status Register(std::string_view email, std::string_view password, std::string_view nickname,
                std::string_view country, std::string_view birthday);

struct Profile {
    bool email_verified = false;
    std::string display_name;
};
/// The signed-in account, or nothing (with `status` saying why).
std::optional<Profile> Me(Status* status = nullptr);

/// The signed-in account's nickname: the one Me() last kept, else one read of Me(). Empty when
/// signed out or the server is unreachable.
std::string KnownDisplayName();

/// Sends the verification mail again (the server sends at most one per 10 minutes).
Status ResendVerification();

/// Whether a bearer is kept.
bool HasAccount();

/// Forgets the bearer (and tells the server, best effort).
void SignOut();

/// An NA id_token for the signed-in account, for Session::Link. Empty on failure.
std::string LinkToken(Status* status = nullptr);

} // namespace OpenPak::Account

namespace OpenPak {

/// The firmware registered a new user (acc's CompleteUserRegistration). When the setup signed in
/// to an OpenPak account (with a verified address) and no firmware user is linked to it yet, this
/// user is linked to it in the background: a device account, then federation with the account's
/// id_token. acc's answers (Session::Linked) then show it linked, with OpenPak's nickname and
/// picture. Returns at once; never blocks on the network. `uid` is the 32 hex digits of the UserId.
/// `on_linked` runs on the link's thread once the user is linked, with the link's NA id_token
/// (acc reads users/me with it, as the firmware's own link does). Never logged.
void OnFirmwareUserRegistered(const std::string& uid,
                              std::function<void(const std::string& id_token)> on_linked = {});

/// While OnFirmwareUserRegistered's link of `uid` runs, waits for it (at most a few seconds), so
/// acc answers the firmware with the linked state rather than the moment before it. Returns at once
/// for any other user.
void WaitForFirmwareUserLink(const std::string& uid);

} // namespace OpenPak

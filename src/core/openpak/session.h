// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later
// Derived from openpak-client 3fd9ba7 (session.h).

// The online chain a Switch walks for a signed-in user, walked here for each firmware user:
//
//   POST dauth /v8/challenge               a challenge to mix into the device MAC
//   POST dauth /v8/device_auth_tokens      a device token
//   POST aauth /v5/application_auth_token  a per-title token, when a title is bound below
//   POST baas  /1.0.0/application/token    an application token, the Bearer for the rest
//   POST baas  /1.0.0/users                a device account -- once, then kept in the secret store
//   POST baas  /1.0.0/login | /federation  the id_token a title asks acc for
//
// All under Nintendo's own hostnames, which the console server answers to and routes by SNI.
//
// Identity is per firmware user: every call that concerns an identity names the user by `uid`,
// the 32 hex digits acc prints a UserId as, and each uid has its own device account, tokens and
// sign-in. An empty uid means the active user (Platform::ProfileId(): the one acc has open).
// The friends caches, presence, the invitation inbox and the Penne push speak for the active user
// only, as the Ryujinx Harbor's single session does.
//
// Threading: the blocking calls (Ensure, Link, RefreshInvitations, SendAs) make network round
// trips and belong on a worker thread, never on a guest IPC thread. The *Async forms return at
// once. The accessors (IdToken, UserId...) only take a lock for a moment and never wait on a
// sign-in in progress.

#pragma once

#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace OpenPak::Session {

/// One invitation waiting in the native inbox (the active user's).
struct Invitation {
    std::string id;          ///< Decimal, as the friends module parses it (%llu).
    std::string sender_id;   ///< The sender's BAAS user id.
    std::string sender_name; ///< Their nickname, or the id when it cannot be looked up.
    std::string title_id;    ///< 16 hex digits: the title to launch.
    /// The title's own join payload, relayed to the guest byte for byte.
    std::vector<std::uint8_t> app_param;
    std::int64_t created_at = 0;
    std::int64_t expires_at = 0;
    /// What the sender's game wrote, by language tag. Empty slots are not kept.
    std::map<std::string, std::string> messages;
};

/// Where the console server answers, and the CA that proves it. Set by BootConnection::Run;
/// changing the server starts different device accounts, as it must.
void Configure(std::string server_host, int port, std::string ca_path);

/// Where this console's own TLS client identity comes from (DER certificate and DER private key;
/// Harbor's calibration). Used for a server this install has no certificate for yet; one that has
/// keeps its own, so existing device accounts carry on. Without a source a self-signed
/// certificate is made, as before.
void SetClientIdentitySource(
    std::function<bool(std::vector<std::uint8_t>& certificate, std::vector<std::uint8_t>& key)>
        source);

/// Whether a server has been configured.
bool Enabled();

/// The pinned OpenPak CA in DER form (the first certificate of ca.pem), or empty. Never fetches.
std::vector<std::uint8_t> CaCertificateDer();

/// Make sure `uid` has a fresh id_token, running the chain if not. Blocking; never throws.
bool Ensure(const std::string& uid);
/// Ensure on a thread of its own; the future (or `done`, on that thread) carries the answer.
std::future<bool> EnsureAsync(std::string uid);
void EnsureAsync(std::string uid, std::function<void(bool)> done);

/// Bind `uid`'s device account to the Nintendo Account that `account_id_token` belongs to
/// (federation). Empty on success, else why not (for the log, never shown as host UI). `uid` need
/// not be a firmware user yet (registration federates before acc creates the user): its device
/// account is created and kept in the secret store, and ForgetProfile(uid) undoes that.
std::string Link(const std::string& uid, const std::string& account_id_token);
std::future<std::string> LinkAsync(std::string uid, std::string account_id_token);

/// The title about to ask for a token, as 16 hex digits and its version string: the login is
/// bound to it (aauth). Empty leaves the chain unbound. A changed binding re-runs the chain on the
/// next Ensure.
void SetApplication(std::string application_id, std::string application_version);

/// What the last sign-in of `uid` produced; empty / 0 when there is none.
std::string IdToken(const std::string& uid);
std::uint64_t NetworkServiceAccountId(const std::string& uid);
std::string UserId(const std::string& uid); ///< The BAAS user id as 16 hex digits.
std::string Nickname(const std::string& uid);
std::string FriendCode(const std::string& uid);
/// Signed in is not linked: an unlinked device account gets a valid token no title server can
/// attach to a person.
bool Linked(const std::string& uid);
/// From the login/federation reply's user object, as sent; never logged.
std::uint64_t NintendoAccountId(const std::string& uid); ///< links.nintendoAccount.id, or 0
std::string Birthday(const std::string& uid);            ///< "YYYY-MM-DD" (may be 0000-00-00)
std::string Country(const std::string& uid);
std::string ThumbnailUrl(const std::string& uid);

/// A dauth token for another client with this install's device identity: the device auth token
/// (empty `vendor`, /v8/device_auth_tokens) or an edge token (/v8/edge_tokens with vendor_id,
/// e.g. "akamai" for nim:shp). Empty on failure or with no console server. Blocking: run it from
/// an AsyncWorker task only.
std::string DauthToken(const std::string& client_id, const std::string& vendor);

/// Whether `uid` has a device account at the configured server, from the secret store alone: no
/// network, no sign-in. False with no server configured.
bool HasDeviceAccount(const std::string& uid);

/// A firmware user was deleted: its device account is forgotten (secret store) and its identity
/// dropped from this process.
void ForgetProfile(const std::string& uid);

/// ForgetProfile for every user this process has signed in: the console became another one.
void ForgetAllProfiles();

/// The beat for the active user: presence renewals, the invitation inbox and the friends caches,
/// and a sign-in when the active user changes. Starts the Penne push. Idempotent.
void StartHeartbeat(std::function<std::string()> current_title_id);

/// The application that runs now and its NACP presence group id, or (0, 0) when nothing runs.
/// Published off the caller's thread.
void SetRunningApplication(std::uint64_t application_id, std::uint64_t presence_group_id);

/// Whether the heartbeat runs and has a signed-in active user.
bool Beating();

/// The JSON-Patch body presence would publish right now for the active user.
std::string PresenceBody();

/// Stop the beat and the push, and say the active user is going (INACTIVE).
void GoOffline();

/// The active user's native inbox as of the last poll, dismissals removed.
std::vector<Invitation> Invitations();
/// Ask the inbox now (blocking). False when nobody is signed in.
bool RefreshInvitations();
/// Take one off the list and mark it read on the server (blocking).
void DismissInvitation(const std::string& invitation_id);
void SetInvitationHook(std::function<void(const Invitation&)> on_arrival);
void SetInvitationsChangedHook(std::function<void()> on_change);

/// One request as the active user: pinned client, user-scoped bearer, sign-in refreshed first.
/// Status 0 is a request that could not be made. Blocking; never throws.
struct Response {
    int status = 0;
    std::string body;
    std::string etag;         ///< The ETag header, when the server sent one.
    std::string content_type; ///< The Content-Type header, when the server sent one.
};
Response SendAs(const std::string& method, const std::string& host,
                const std::string& path_and_query, const std::string& content_type,
                const std::string& body);

/// SendAs for the firmware user `uid` (empty: the active user), for a service that acts for
/// whichever user the guest names (olsc). Signs `uid` in first. Blocking; never throws.
Response SendAsUser(const std::string& uid, const std::string& method, const std::string& host,
                    const std::string& path_and_query, const std::string& content_type,
                    const std::string& body);

/// One request to a Nintendo hostname through the pinned client (redirected to the console
/// server, never the real host), with no user bearer and no sign-in first: the NA token exchange,
/// users/me with the NA access token, a thumbnail. `headers` go out as given; an empty
/// `content_type` sends no body, any other is sent as is. Blocking; never throws.
using Headers = std::vector<std::pair<std::string, std::string>>;
Response Send(const std::string& method, const std::string& host,
              const std::string& path_and_query, const Headers& headers,
              const std::string& content_type, const std::string& body);

} // namespace OpenPak::Session

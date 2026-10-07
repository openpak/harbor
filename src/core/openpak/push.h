// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later
// Derived from openpak-client 3fd9ba7 (push.h).

// The push connection a console holds (Penne/NPNS, nx-baas docs/penne-protocol.md), as the
// Ryujinx Harbor's OpenPakSession.Push.cs and PenneFrames.cs hold it: register a Penne id, take a login ticket,
// and keep a POST to the frontline open, reading the length-prefixed FlatBuffers frames the
// server streams. A delivered friend request, acceptance, removal, invitation or presence change
// re-reads the cache it concerns at once, instead of at the heartbeat's next poll, which stays
// as the fallback.
//
// Downlink only, as the console's own frontline body is empty; presence keeps going out on the
// REST PATCH the heartbeat already makes. OPENPAK_NO_PUSH=1 turns it off.

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>

namespace OpenPak::Push {

inline constexpr std::uint8_t TypePutRecord = 0x04;
inline constexpr std::uint8_t TypeHandoverResult = 0x0c;
inline constexpr std::uint8_t TypeReset = 0x0d;

/// The largest frame accepted.
inline constexpr std::uint32_t MaxFrame = 64 * 1024;

/// The envelope's type byte, or nothing when the frame is not a readable envelope.
std::optional<std::uint8_t> EnvelopeType(std::span<const std::uint8_t> frame);

/// A notification: the kind-1 message a PutRecord carries.
struct Message {
    std::string name;
    std::string body;
};

/// The message in a PutRecord frame, or nothing for any other frame. Never reads outside it.
std::optional<Message> ReadMessage(std::span<const std::uint8_t> frame);

/// The event kind a message names: the body's "type", else the message name.
std::string Kind(const Message& message);

/// Splits a byte stream into frames (four-byte little-endian length, then the frame).
class FrameReader {
public:
    /// Take more bytes. False when the stream is broken (a zero or oversize length).
    bool Feed(std::span<const std::uint8_t> bytes);

    /// The next whole frame, if one has arrived.
    std::optional<std::string> Next();

private:
    std::string buffer;
};

/// Hold the connection on a thread of its own, for whoever is signed in; idempotent. The
/// session's heartbeat calls it, so a host never has to.
void Start();

/// End the connection and the thread. The session calls it on the way out.
void Stop();

/// The account changed: the connection speaking for the old one ends now.
void Drop();

/// Whether a frontline connection is open right now.
bool Connected();

/// A deleted user's registration (the secret store's copy), on the configured server.
void ForgetProfile(const std::string& uid);

/// Called for every delivered message after the caches it concerns were queued for a refresh,
/// with the event kind (the body's "type", else the message name) and the raw body, on the push
/// thread with no lock held. The kinds the guest cares about: friend_invitation_received and
/// presence_updated (FriendsOverlayNotifier), DID_CHANGE_PARENTAL_CONTROL_SETTING and
/// DID_UNLINK_NX (pctl). A consumer that signals a guest event posts it to its own session's
/// AsyncWorker; it never signals from this thread. Must not throw.
using Hook = std::function<void(const std::string& kind, const std::string& body)>;
void SetHook(Hook hook);

/// The active user's Penne registration id (pctl's notificationToken), or empty when it has none
/// yet.
std::string PenneId();

} // namespace OpenPak::Push

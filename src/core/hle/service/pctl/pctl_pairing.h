// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// What pctl keeps about pairing with the parent app, and the moon calls that change it, against
// OpenPak's nx-pctl. pctl holds two things (its network manager): the record a pairing code found,
// with up to five owners, which the account getters read (+0x4d090); and the device this console
// is paired with plus an active flag (+0x3e0, +0x3c9), which IsPairingActive reads. pctl keeps the
// second, the PIN and the setting in its system savedata; Harbor keeps them in the host secret
// store under "pctl-<server>" (never a plain file, never logged), per OpenPak server because the
// device id is the dauth device's.
//
// A paired console syncs the setting 5 s after it starts and then every 10 minutes, and at once on
// the DID_CHANGE_PARENTAL_CONTROL_SETTING push. Not done, though a console does it after pairing:
// PUT players/<id> per user and POST events.
//
// UNPROVEN: the Ryujinx Harbor's pairing was never tested on Harbor; this follows its code and the
// firmware study, and every network step here is unexercised against nx-pctl from Eden.
//
// The blocking calls make network round trips: run them on an OpenPak::AsyncWorker task or the
// sync thread, never on a guest IPC thread. Behaviour follows the Ryujinx Harbor (MIT-licensed
// Ryujinx base).

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/hle/service/pctl/moon.h"
#include "core/hle/service/pctl/pctl_setting.h"

namespace OpenPak {
class AsyncWorker;
}

namespace Service::PCTL {

class Pairing {
public:
    static constexpr std::uint32_t StateFound = 1;
    static constexpr std::uint32_t StateActive = 2;

    /// The pairing for the configured OpenPak server, loaded once per server.
    static Pairing& Current();

    /// The worker whose signalling thread raises guest events and sends overlay notifications for
    /// the background sync and the push (the pctl module's, per emulation session).
    static void SetWorker(std::weak_ptr<OpenPak::AsyncWorker> worker);

    /// Called (on the worker's signalling thread) whenever the setting in force changes: a sync
    /// brought a new one, the PIN changed, or the pairing went. Returns an id for RemoveListener.
    static int AddListener(std::function<void()> listener);
    static void RemoveListener(int id);

    /// What the restriction commands read: one consistent copy.
    struct Restrictions {
        std::string pin;
        bool temporarily_unlocked = false;
        int safety_level = 0;
        Rules::Restriction custom;
        int rating_organization = -1;
        bool stereo_vision_restricted = false;
        std::vector<std::pair<std::uint64_t, bool>> free_communication_applications;
        std::set<std::uint64_t> exempt_applications;
        PlayTimerBlock play_timer{};

        /// IsRestrictionEnabled: a PIN is set (pctl tests its first byte, .text 0x2cf58).
        bool Enabled() const {
            return !pin.empty();
        }
    };
    Restrictions InForce() const;

    bool IsPairingActive() const;
    std::uint64_t PairedDevice() const;
    /// As a posix time, or 0 when it never was (GetSettingsLastUpdated: 2142-0140).
    std::int64_t SettingsLastUpdated() const;
    std::uint64_t FoundDevice() const;
    std::vector<Moon::Owner> Owners() const;
    std::optional<Moon::Owner> FirstOwner() const;
    std::pair<std::vector<std::uint8_t>, std::string> Mii() const;

    void SetPin(std::string pin);
    void SetTemporarilyUnlocked(bool unlocked);
    /// pctl keeps it only while a PIN is set (.text 0x41a10).
    void SetStereoVisionRestricted(bool restricted);

    /// What a console says about itself at activate, read on the guest's thread.
    struct DeviceFacts {
        std::string language;
        std::string region;
        std::string displayed_version;
        std::uint32_t internal_version = 0;
    };

    // Blocking network work. Each answers a pctl result description (0 success).
    Moon::Description RequestPairing(const std::string& code);
    Moon::Description Authorize(std::uint64_t device_id, const DeviceFacts& facts);
    Moon::Description Retrieve();
    Moon::Description Unlink(bool force);
    Moon::Description Synchronize(bool* changed = nullptr);
    Moon::Description FetchMii();

    /// What pctl does on an unlink, or when a retrieve finds no owner left (.text 0x30004).
    void ForgetPairing();

    /// The parent changed the setting (the server's push): sync now instead of at the next tick.
    void SyncSoon();

    /// The DID_UNLINK_NX push: dropped unless its deviceId (hex) is the paired device, as pctl
    /// drops it; otherwise the pairing is forgotten and 4002 (source 0) sent.
    void OnUnlinkPush(const std::string& body);

    /// The state behind it (pctl_pairing.cpp only).
    struct Impl;

private:
    explicit Pairing(std::string server_key);
    std::shared_ptr<Impl> impl;
};

} // namespace Service::PCTL

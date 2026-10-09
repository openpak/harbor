// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// This install's calibration: what a Switch keeps in its PRODINFO partition (serial number, Wi-Fi
// MAC, Bluetooth address, device id, device certificates and keys), made once for each new NAND
// and kept in it, at <nand>/calibration/prodinfo.bin, so it travels with the NAND and with a NAND
// backup. Two NANDs never share one.
//
// The device key (what a Switch keeps in its SoC, and the only thing that opens the private keys
// in the binary) is kept in the host's password store, never on disk. A NAND moved to a host that
// does not have its device key keeps its identity but gets new key pairs, as a console's NAND
// would be useless in another console.
//
// docs/harbor/calibration.md has the layout, the choices and what consumes each field.

#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "core/harbor_calibration/cal0.h"

namespace Harbor {

class Calibration {
public:
    /// The current NAND's calibration, made on first use for a NAND that has none. Thread-safe;
    /// follows the NAND directory if it changes.
    static const Calibration& Get();

    /// Read again on the next Get() (the binary or the device key changed on disk).
    static void Reload();

    /// Whether this console's private keys can be used here. Keys are never replaced on their own:
    /// only a restore from a backup or "Make this a new console" changes them.
    enum class KeyState {
        Ready,            ///< the device key is in this host's password store
        StoreUnavailable, ///< no store installed yet, or it could not be read: looked up again later
        NotOnThisHost,    ///< confirmed absent (or not this console's): restore or make a new console
        BackupPending,    ///< a restored NAND whose sealed key waits for its passphrase
    };
    KeyState Keys() const {
        return key_state;
    }

    /// The whole binary (0x8000 bytes, CAL0), as set:cal and a BIS CalibrationBinary read see it.
    std::span<const u8> Binary() const {
        return binary;
    }

    /// One block's value (without its check bytes); zeros when no binary could be made.
    std::span<const u8> BlockData(const Cal0::Block& block) const;

    std::string SerialNumber() const;
    u64 DeviceId() const;
    std::array<u8, 6> WlanMacAddress() const;
    std::array<u8, 6> BluetoothAddress() const;

    /// The TLS client certificate (DER) and its private key (DER), for presenting this console.
    std::vector<u8> SslCertificate() const;
    std::optional<std::vector<u8>> SslPrivateKey() const;

    /// A 16-byte key derived from the device key for one purpose (say "fs-save-transfer"): the
    /// device-bound input a console derives from its own hardware key. Same label, same install,
    /// same key; another install never gets it.
    Cal0::DeviceKey DeriveDeviceKey(std::string_view label) const;

private:
    friend bool WriteDeviceKeyBackup(std::string_view passphrase);
    friend bool DeviceKeyRestorePending();

    std::vector<u8> binary;
    Cal0::DeviceKey device_key{};
    KeyState key_state = KeyState::Ready;
};

// Console backups. A NAND backup carries the device key sealed with a passphrase the user chooses
// (scrypt, AES-256-GCM), at <nand>/calibration/device-key.hbk; the passphrase is never kept.
// Restoring it puts the key back in this host's password store, so the restored console is the
// same console.

/// Seal this console's device key into the NAND for a backup about to be taken.
bool WriteDeviceKeyBackup(std::string_view passphrase);

/// The sealed key out of the live NAND once the backup is taken (or restored).
void RemoveDeviceKeyBackup();

/// A restored NAND whose device key is not in this host's store and that carries a sealed one.
bool DeviceKeyRestorePending();

enum class RestoreResult { Restored, WrongPassphrase, NoBackup, Failed };

/// Open the NAND's sealed device key with `passphrase` and keep it in this host's store.
RestoreResult RestoreDeviceKey(std::string_view passphrase);

/// Make this NAND a new console: a fresh calibration and device key, the old console's
/// credentials dropped (password store and this process). Firmware users and saves stay; the
/// users link their Nintendo Accounts again. The emulated console must be stopped.
bool MakeNewConsole();

/// Keep OpenPak's secrets (credentials, tokens, registrations) under this NAND's own namespace in
/// the host password store, so consoles on one host never share them. Call before any of them is
/// read: the frontend's secret store installation and core's boot both do.
void InstallSecretNamespace();

/// Make OpenPak servers this install meets from now on see the console's own TLS certificate.
/// Called before the services start, so no session can make a self-signed one first.
void InstallClientIdentity();

} // namespace Harbor

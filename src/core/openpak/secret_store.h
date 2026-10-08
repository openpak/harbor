// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// The host's password store, for what OpenPak keeps that is a credential: the BaaS device
// account of each firmware user (its id and password are the account), the Penne registration,
// and later the pctl pairing state. Never plain JSON on disk.
//
// The frontends implement it (Android Keystore through JNI, libsecret on desktop) and install it
// before BootConnection::Run. Without one, secrets live in this process's memory only: a device
// account made then is lost at exit, and a warning says so once.

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace OpenPak {

class SecretStore {
public:
    virtual ~SecretStore() = default;

    /// The secret under `key`, or nothing when there is none (or the store could not be read).
    virtual std::optional<std::string> Load(std::string_view key) = 0;

    /// Keep `secret` under `key`, replacing any. False when it could not be kept.
    virtual bool Store(std::string_view key, std::string_view secret) = 0;

    /// Forget `key`; nothing when there is none.
    virtual void Erase(std::string_view key) = 0;
};

/// Install the host's store (null removes it). Thread-safe; set it once at start-up.
void SetSecretStore(std::shared_ptr<SecretStore> store);

/// Whether a host store is installed.
bool HasSecretStore();

/// Through the host store, or the process-memory fallback when none is installed. Keys are short
/// ASCII names such as "device-<server>-<uid>"; values are opaque (JSON today).
std::optional<std::string> LoadSecret(std::string_view key);
bool StoreSecret(std::string_view key, std::string_view secret);
void EraseSecret(std::string_view key);

/// The console the secrets above belong to. Every key is kept as "<namespace>-<key>", so two
/// consoles (NANDs) on one host, even with the same firmware user, never share credentials. The
/// function is asked at each use, so it follows the current NAND; an empty answer means no
/// namespace. An entry kept before namespacing is moved under the namespace the first time it is
/// read (read the old key, write the new one, erase the old one), so nobody is signed out.
void SetSecretNamespace(std::function<std::string()> resolve);

/// Host-wide entries outside any console's namespace: the console's own device key, which names
/// the namespace and so cannot live inside it.
std::optional<std::string> LoadHostSecret(std::string_view key);
bool StoreHostSecret(std::string_view key, std::string_view secret);
void EraseHostSecret(std::string_view key);

} // namespace OpenPak

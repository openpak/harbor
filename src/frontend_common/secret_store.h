// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// OpenPak's credentials (core/openpak/secret_store.h) in the operating system's own password store,
// one backend per platform, chosen in CMake:
//   Linux/BSD  secret_store_libsecret.cpp  the Secret Service (GNOME Keyring, KWallet) via libsecret
//   Android    secret_store_android.cpp    the Android Keystore (AES-256-GCM), through HarborSecrets.kt
//   macOS/iOS  secret_store_apple.cpp      the keychain (Security.framework)
//   Windows    secret_store_windows.cpp    Credential Manager, DPAPI files for oversized secrets
// There is no file fallback: without an OS store OpenPak keeps its secrets in this process's memory
// only, and says so.

#pragma once

#include <memory>

namespace OpenPak {
class SecretStore;
}

namespace FrontendCommon {

/// This platform's store, or null when it cannot be reached here. Not yet checked.
/// Android: call from a JNI method on a Java thread (the Kotlin class is found through the app's
/// class loader); the store itself can then be used from any thread.
std::shared_ptr<OpenPak::SecretStore> CreatePlatformSecretStore();

/// Creates the platform store, checks it with a store/load/erase round trip of a dummy key, and
/// installs it with OpenPak::SetSecretStore. Call before OpenPak::BootConnection::Run. Later calls
/// return the first answer. Returns whether an OS store is installed.
bool InstallSecretStore();

} // namespace FrontendCommon

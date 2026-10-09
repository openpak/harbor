// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <mutex>
#include <optional>

#include "common/logging.h"
#include "core/harbor_calibration/calibration.h"
#include "core/openpak/secret_store.h"
#include "frontend_common/secret_store.h"

namespace FrontendCommon {

namespace {

// Some stores answer but cannot keep anything (a keyring nobody unlocks, a vendor keymaster whose
// keys then fail): find out at boot, not at the first sign-in.
bool RoundTrip(OpenPak::SecretStore& store) {
    constexpr std::string_view key = "harbor-secret-probe";
    // JSON with escaped quotes: the shape GNOME Keyring was seen to lose.
    constexpr std::string_view probe = R"({"probe":"a \"quoted\" value","n":1234})";
    const bool works = store.Store(key, probe) && store.Load(key) == probe;
    store.Erase(key);
    return works && !store.Load(key).has_value();
}

} // namespace

bool InstallSecretStore() {
    // Before anything reads a secret: each console (NAND) keeps its own.
    Harbor::InstallSecretNamespace();

    static std::mutex mutex;
    static std::optional<bool> installed;
    std::scoped_lock lock{mutex};
    if (installed) {
        return *installed;
    }
    auto store = CreatePlatformSecretStore();
    if (!store) {
        LOG_WARNING(Frontend, "harbor: no password store on this system; OpenPak keeps its "
                              "credentials in memory only");
        installed = false;
        OpenPak::SetSecretStoreDecided();
        return false;
    }
    if (!RoundTrip(*store)) {
        LOG_WARNING(Frontend, "harbor: the password store failed a store/load/erase check; OpenPak "
                              "keeps its credentials in memory only");
        installed = false;
        OpenPak::SetSecretStoreDecided();
        return false;
    }
    OpenPak::SetSecretStore(std::move(store));
    LOG_INFO(Frontend, "harbor: OpenPak credentials in the system password store (round trip ok)");
    installed = true;
    return true;
}

} // namespace FrontendCommon

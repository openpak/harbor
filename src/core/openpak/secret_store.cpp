// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <map>
#include <mutex>

#include "common/logging.h"
#include "core/openpak/secret_store.h"

namespace OpenPak {

namespace {

struct Secrets {
    std::mutex mutex;
    std::shared_ptr<SecretStore> store;
    std::map<std::string, std::string, std::less<>> memory;
    bool warned = false;
};

Secrets& Get() {
    static Secrets secrets;
    return secrets;
}

std::shared_ptr<SecretStore> Store() {
    Secrets& s = Get();
    std::lock_guard lock{s.mutex};
    return s.store;
}

} // namespace

void SetSecretStore(std::shared_ptr<SecretStore> store) {
    Secrets& s = Get();
    std::lock_guard lock{s.mutex};
    s.store = std::move(store);
}

bool HasSecretStore() {
    return Store() != nullptr;
}

std::optional<std::string> LoadSecret(std::string_view key) {
    if (const auto store = Store()) {
        return store->Load(key);
    }
    Secrets& s = Get();
    std::lock_guard lock{s.mutex};
    const auto it = s.memory.find(key);
    return it != s.memory.end() ? std::optional{it->second} : std::nullopt;
}

bool StoreSecret(std::string_view key, std::string_view secret) {
    if (const auto store = Store()) {
        return store->Store(key, secret);
    }
    Secrets& s = Get();
    std::lock_guard lock{s.mutex};
    if (!s.warned) {
        s.warned = true;
        LOG_WARNING(WebService, "[OpenPak] No secret store installed: OpenPak credentials are kept "
                                "in memory only and are lost when Harbor exits");
    }
    s.memory.insert_or_assign(std::string{key}, std::string{secret});
    return true;
}

void EraseSecret(std::string_view key) {
    if (const auto store = Store()) {
        store->Erase(key);
        return;
    }
    Secrets& s = Get();
    std::lock_guard lock{s.mutex};
    if (const auto it = s.memory.find(key); it != s.memory.end()) {
        s.memory.erase(it);
    }
}

} // namespace OpenPak

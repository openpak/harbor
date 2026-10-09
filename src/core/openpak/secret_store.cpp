// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <algorithm>
#include <map>
#include <vector>
#include <mutex>

#include "common/logging.h"
#include "core/openpak/secret_store.h"

namespace OpenPak {

namespace {

struct Secrets {
    std::mutex mutex;
    std::shared_ptr<SecretStore> store;
    std::function<std::string()> resolve_namespace;
    bool migration_logged = false;
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

std::optional<std::string> LoadHostSecret(std::string_view key, bool* unreadable) {
    if (const auto store = Store()) {
        return store->Load(key, unreadable);
    }
    Secrets& s = Get();
    std::lock_guard lock{s.mutex};
    const auto it = s.memory.find(key);
    return it != s.memory.end() ? std::optional{it->second} : std::nullopt;
}

bool StoreHostSecret(std::string_view key, std::string_view secret) {
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

void EraseHostSecret(std::string_view key) {
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

namespace {

std::string Namespace() {
    std::function<std::string()> resolve;
    {
        Secrets& s = Get();
        std::lock_guard lock{s.mutex};
        resolve = s.resolve_namespace;
    }
    return resolve ? resolve() : std::string{};
}

std::string Namespaced(const std::string& ns, std::string_view key) {
    return ns.empty() ? std::string{key} : ns + "-" + std::string{key};
}

// Each namespace keeps the names of its entries (one per line) under "<namespace>-index", so a
// console's credentials can be dropped together: the stores have no listing of their own.
std::mutex& IndexMutex() {
    static std::mutex mutex;
    return mutex;
}

std::vector<std::string> ReadIndex(const std::string& ns) {
    std::vector<std::string> keys;
    const auto text = LoadHostSecret(Namespaced(ns, "index"));
    if (!text) {
        return keys;
    }
    std::size_t start = 0;
    while (start < text->size()) {
        const std::size_t end = std::min(text->find('\n', start), text->size());
        if (end > start) {
            keys.emplace_back(text->substr(start, end - start));
        }
        start = end + 1;
    }
    return keys;
}

void WriteIndex(const std::string& ns, const std::vector<std::string>& keys) {
    if (keys.empty()) {
        EraseHostSecret(Namespaced(ns, "index"));
        return;
    }
    std::string text;
    for (const auto& key : keys) {
        text += key;
        text += '\n';
    }
    StoreHostSecret(Namespaced(ns, "index"), text);
}

void IndexAdd(const std::string& ns, std::string_view key) {
    if (ns.empty()) {
        return;
    }
    std::lock_guard lock{IndexMutex()};
    auto keys = ReadIndex(ns);
    if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
        keys.emplace_back(key);
        WriteIndex(ns, keys);
    }
}

void IndexRemove(const std::string& ns, std::string_view key) {
    if (ns.empty()) {
        return;
    }
    std::lock_guard lock{IndexMutex()};
    auto keys = ReadIndex(ns);
    if (const auto it = std::find(keys.begin(), keys.end(), key); it != keys.end()) {
        keys.erase(it);
        WriteIndex(ns, keys);
    }
}

} // namespace

void SetSecretNamespace(std::function<std::string()> resolve) {
    Secrets& s = Get();
    std::lock_guard lock{s.mutex};
    s.resolve_namespace = std::move(resolve);
}

std::optional<std::string> LoadSecret(std::string_view key, bool* unreadable) {
    const std::string ns = Namespace();
    bool failed = false;
    if (auto secret = LoadHostSecret(Namespaced(ns, key), &failed); secret || failed || ns.empty()) {
        if (failed && unreadable) {
            *unreadable = true;
        }
        return secret;
    }
    // An entry from before namespacing: it becomes this console's, once.
    auto legacy = LoadHostSecret(key, unreadable);
    if (legacy && StoreHostSecret(Namespaced(ns, key), *legacy)) {
        IndexAdd(ns, key);
        EraseHostSecret(key);
        Secrets& s = Get();
        std::lock_guard lock{s.mutex};
        if (!s.migration_logged) {
            s.migration_logged = true;
            LOG_INFO(WebService, "[OpenPak] Credentials kept before consoles had their own "
                                 "namespace moved to this console's");
        }
    }
    return legacy;
}

bool StoreSecret(std::string_view key, std::string_view secret) {
    const std::string ns = Namespace();
    if (!StoreHostSecret(Namespaced(ns, key), secret)) {
        return false;
    }
    IndexAdd(ns, key);
    return true;
}

void EraseSecret(std::string_view key) {
    const std::string ns = Namespace();
    EraseHostSecret(Namespaced(ns, key));
    if (!ns.empty()) {
        IndexRemove(ns, key);
        EraseHostSecret(key); // and any entry from before namespacing
    }
}

std::string CurrentSecretNamespace() {
    return Namespace();
}

void EraseSecretNamespace(const std::string& ns) {
    if (ns.empty()) {
        return;
    }
    std::lock_guard lock{IndexMutex()};
    for (const auto& key : ReadIndex(ns)) {
        EraseHostSecret(Namespaced(ns, key));
    }
    EraseHostSecret(Namespaced(ns, "index"));
}

} // namespace OpenPak

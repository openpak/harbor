// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// macOS and iOS: the keychain through Security.framework. Generic passwords, service
// "org.openpak.harbor", account = <key>, readable after the first unlock on this device only and
// never synchronised to other devices.
// Untested: written against the Security.framework documentation, not yet built or run on Apple.

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <TargetConditionals.h>

#include "common/logging.h"
#include "core/openpak/secret_store.h"
#include "frontend_common/secret_store.h"

namespace FrontendCommon {

namespace {

constexpr const char* Service = "org.openpak.harbor";

/// Owns one Core Foundation reference.
template <typename T>
class CF {
public:
    explicit CF(T ref_ = nullptr) : ref{ref_} {}
    ~CF() {
        if (ref) {
            CFRelease(ref);
        }
    }
    CF(const CF&) = delete;
    CF& operator=(const CF&) = delete;
    T get() const {
        return ref;
    }

private:
    T ref;
};

CFStringRef String(std::string_view text) {
    return CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(text.data()),
                                   static_cast<CFIndex>(text.size()), kCFStringEncodingUTF8, false);
}

class KeychainSecretStore final : public OpenPak::SecretStore {
public:
    explicit KeychainSecretStore(bool data_protection_) : data_protection{data_protection_} {}

    std::optional<std::string> Load(std::string_view key) override {
        CF<CFMutableDictionaryRef> query{Query(key)};
        CFDictionarySetValue(query.get(), kSecReturnData, kCFBooleanTrue);
        CFDictionarySetValue(query.get(), kSecMatchLimit, kSecMatchLimitOne);
        CFTypeRef result = nullptr;
        const OSStatus status = SecItemCopyMatching(query.get(), &result);
        CF<CFTypeRef> owned{result};
        if (status == errSecItemNotFound) {
            return std::nullopt;
        }
        if (status != errSecSuccess || !result || CFGetTypeID(result) != CFDataGetTypeID()) {
            LOG_WARNING(Frontend, "harbor: the keychain could not be read (OSStatus {})", status);
            return std::nullopt;
        }
        const auto data = static_cast<CFDataRef>(result);
        return std::string(reinterpret_cast<const char*>(CFDataGetBytePtr(data)),
                           static_cast<size_t>(CFDataGetLength(data)));
    }

    bool Store(std::string_view key, std::string_view secret) override {
        CF<CFDataRef> data{CFDataCreate(kCFAllocatorDefault,
                                        reinterpret_cast<const UInt8*>(secret.data()),
                                        static_cast<CFIndex>(secret.size()))};
        CF<CFMutableDictionaryRef> query{Query(key)};
        CF<CFMutableDictionaryRef> update{CFDictionaryCreateMutable(
            kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks)};
        CFDictionarySetValue(update.get(), kSecValueData, data.get());
        CFDictionarySetValue(update.get(), kSecAttrAccessible,
                             kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly);
        OSStatus status = SecItemUpdate(query.get(), update.get());
        if (status == errSecItemNotFound) {
            CFDictionarySetValue(query.get(), kSecValueData, data.get());
            CFDictionarySetValue(query.get(), kSecAttrAccessible,
                                 kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly);
            status = SecItemAdd(query.get(), nullptr);
        }
        if (status != errSecSuccess) {
            LOG_WARNING(Frontend, "harbor: the keychain could not be written (OSStatus {})", status);
            return false;
        }
        return true;
    }

    void Erase(std::string_view key) override {
        CF<CFMutableDictionaryRef> query{Query(key)};
        const OSStatus status = SecItemDelete(query.get());
        if (status != errSecSuccess && status != errSecItemNotFound) {
            LOG_WARNING(Frontend, "harbor: the keychain could not forget a secret (OSStatus {})",
                        status);
        }
    }

private:
    /// The item's identity: class, service, account, this device only.
    CFMutableDictionaryRef Query(std::string_view key) const {
        CFMutableDictionaryRef query = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CF<CFStringRef> service{String(Service)};
        CF<CFStringRef> account{String(key)};
        CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
        CFDictionarySetValue(query, kSecAttrService, service.get());
        CFDictionarySetValue(query, kSecAttrAccount, account.get());
        CFDictionarySetValue(query, kSecAttrSynchronizable, kCFBooleanFalse);
#if TARGET_OS_OSX
        // macOS: the iOS-style keychain, which honours kSecAttrAccessible. A build without the
        // keychain entitlement cannot use it and falls back to the login keychain.
        if (data_protection) {
            CFDictionarySetValue(query, kSecUseDataProtectionKeychain, kCFBooleanTrue);
        }
#endif
        return query;
    }

    bool data_protection;
};

} // namespace

std::shared_ptr<OpenPak::SecretStore> CreatePlatformSecretStore() {
#if TARGET_OS_OSX
    // Without the keychain-access-groups entitlement (an unsigned or ad-hoc build) the data
    // protection keychain answers errSecMissingEntitlement; the login keychain then keeps them.
    auto store = std::make_shared<KeychainSecretStore>(true);
    constexpr std::string_view probe = "harbor-keychain-probe";
    const bool works = store->Store(probe, "probe") && store->Load(probe) == "probe";
    store->Erase(probe);
    if (works) {
        return store;
    }
    LOG_WARNING(Frontend, "harbor: no data protection keychain here; using the login keychain");
    return std::make_shared<KeychainSecretStore>(false);
#else
    return std::make_shared<KeychainSecretStore>(true);
#endif
}

} // namespace FrontendCommon

// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Linux and the BSDs: the Secret Service (GNOME Keyring, KWallet, KeePassXC) through libsecret.
// Items: schema "org.openpak.harbor", attributes service = "org.openpak.harbor", account = <key>.

#include "common/logging.h"
#include "core/openpak/secret_store.h"
#include "frontend_common/secret_store.h"

#ifdef HARBOR_HAS_LIBSECRET
#include <libsecret/secret.h>
#endif

namespace FrontendCommon {

#ifdef HARBOR_HAS_LIBSECRET
namespace {

constexpr const char* Service = "org.openpak.harbor";

const SecretSchema* Schema() {
    static const SecretSchema schema = {
        "org.openpak.harbor",
        SECRET_SCHEMA_NONE,
        {
            {"service", SECRET_SCHEMA_ATTRIBUTE_STRING},
            {"account", SECRET_SCHEMA_ATTRIBUTE_STRING},
            {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING},
        },
        0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    };
    return &schema;
}

/// Logs a libsecret error (never a value) and frees it. True when there was one.
bool Failed(GError* error, const char* what) {
    if (!error) {
        return false;
    }
    LOG_WARNING(Frontend, "harbor: the password store could not {}: {}", what, error->message);
    g_error_free(error);
    return true;
}

class LibsecretStore final : public OpenPak::SecretStore {
public:
    std::optional<std::string> Load(std::string_view key) override {
        const std::string account{key};
        GError* error = nullptr;
        gchar* secret = secret_password_lookup_sync(Schema(), nullptr, &error, "service", Service,
                                                    "account", account.c_str(), nullptr);
        if (Failed(error, "be read") || !secret) {
            return std::nullopt;
        }
        std::string value{secret};
        secret_password_free(secret);
        return value;
    }

    bool Store(std::string_view key, std::string_view secret) override {
        const std::string account{key};
        const std::string value{secret};
        const std::string label = "Harbor (OpenPak): " + account;
        GError* error = nullptr;
        const gboolean ok = secret_password_store_sync(
            Schema(), SECRET_COLLECTION_DEFAULT, label.c_str(), value.c_str(), nullptr, &error,
            "service", Service, "account", account.c_str(), nullptr);
        return !Failed(error, "be written") && ok;
    }

    void Erase(std::string_view key) override {
        const std::string account{key};
        GError* error = nullptr;
        secret_password_clear_sync(Schema(), nullptr, &error, "service", Service, "account",
                                   account.c_str(), nullptr);
        Failed(error, "forget a secret");
    }
};

} // namespace
#endif

std::shared_ptr<OpenPak::SecretStore> CreatePlatformSecretStore() {
#ifdef HARBOR_HAS_LIBSECRET
    GError* error = nullptr;
    SecretService* service = secret_service_get_sync(SECRET_SERVICE_NONE, nullptr, &error);
    if (!service) {
        LOG_WARNING(Frontend, "harbor: no Secret Service ({})", error ? error->message : "unknown");
        if (error) {
            g_error_free(error);
        }
        return nullptr;
    }
    g_object_unref(service);
    return std::make_shared<LibsecretStore>();
#else
    LOG_WARNING(Frontend, "harbor: this build has no libsecret");
    return nullptr;
#endif
}

} // namespace FrontendCommon

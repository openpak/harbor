// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Linux and the BSDs: the Secret Service (GNOME Keyring, KWallet, KeePassXC) through libsecret.
// Items: schema "org.openpak.harbor", attributes service = "org.openpak.harbor", account = <key>.

#include <optional>
#include <string>
#include <string_view>

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

// GNOME Keyring through libsecret has been seen to hand back an empty secret for a value it had
// accepted (a JSON value with \" escapes; a PIN lost in 3 runs of 3). Values are therefore kept
// base64-encoded behind a prefix, every write is read back, and both sides retry. Values written
// before the encoding (no prefix) still load as they are.
constexpr std::string_view EncodedPrefix = "b64:";
constexpr int Attempts = 3;

std::string Encode(std::string_view value) {
    gchar* encoded = g_base64_encode(reinterpret_cast<const guchar*>(value.data()), value.size());
    std::string out = std::string{EncodedPrefix} + encoded;
    g_free(encoded);
    return out;
}

std::optional<std::string> Decode(const std::string& stored) {
    if (!stored.starts_with(EncodedPrefix)) {
        return stored; // written before the encoding
    }
    const std::string body = stored.substr(EncodedPrefix.size());
    gsize length = 0;
    guchar* decoded = g_base64_decode(body.c_str(), &length);
    std::string out{reinterpret_cast<const char*>(decoded), length};
    g_free(decoded);
    return out;
}

class LibsecretStore final : public OpenPak::SecretStore {
public:
    std::optional<std::string> Load(std::string_view key) override {
        const std::string account{key};
        for (int attempt = 1; attempt <= Attempts; attempt++) {
            bool missing = false;
            const auto stored = LookUp(account, &missing);
            if (missing) {
                return std::nullopt;
            }
            // An empty answer is never what was stored (an encoded value has at least the prefix).
            if (stored && !stored->empty()) {
                return Decode(*stored);
            }
            LOG_WARNING(Frontend, "harbor: the password store answered an empty or failed read "
                                  "(attempt {} of {})",
                        attempt, Attempts);
        }
        return std::nullopt;
    }

    bool Store(std::string_view key, std::string_view secret) override {
        const std::string account{key};
        const std::string value = Encode(secret);
        const std::string label = "Harbor (OpenPak): " + account;
        for (int attempt = 1; attempt <= Attempts; attempt++) {
            GError* error = nullptr;
            const gboolean ok = secret_password_store_sync(
                Schema(), SECRET_COLLECTION_DEFAULT, label.c_str(), value.c_str(), nullptr, &error,
                "service", Service, "account", account.c_str(), nullptr);
            if (!Failed(error, "be written") && ok) {
                bool missing = false;
                const auto back = LookUp(account, &missing);
                if (back && *back == value) {
                    return true;
                }
                LOG_WARNING(Frontend, "harbor: the password store kept {} of {} bytes (attempt {} of {})",
                            back ? back->size() : 0, value.size(), attempt, Attempts);
            }
        }
        return false;
    }

    void Erase(std::string_view key) override {
        const std::string account{key};
        GError* error = nullptr;
        secret_password_clear_sync(Schema(), nullptr, &error, "service", Service, "account",
                                   account.c_str(), nullptr);
        Failed(error, "forget a secret");
    }

private:
    /// The stored string as libsecret returns it. `missing` is set when there is no such item.
    static std::optional<std::string> LookUp(const std::string& account, bool* missing) {
        GError* error = nullptr;
        gchar* secret = secret_password_lookup_sync(Schema(), nullptr, &error, "service", Service,
                                                    "account", account.c_str(), nullptr);
        if (Failed(error, "be read")) {
            return std::nullopt;
        }
        if (!secret) {
            *missing = true;
            return std::nullopt;
        }
        std::string value{secret};
        secret_password_free(secret);
        return value;
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

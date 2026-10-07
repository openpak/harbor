// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later
// Derived from openpak-client 3fd9ba7 (session.cpp).
// Per-user identities and the secret store follow the Ryujinx Harbor (MIT-licensed Ryujinx base).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#if defined(LIBRESSL_VERSION_NUMBER) || OPENSSL_VERSION_NUMBER < 0x30000000L
#include <openssl/bn.h>
#include <openssl/rsa.h>
#endif

#include "common/httplib.h"
#include "common/logging.h"
#include "core/openpak/baas.h"
#include "core/openpak/platform.h"
#include "core/openpak/presence_state.h"
#include "core/openpak/push.h"
#include "core/openpak/secret_store.h"
#include "core/openpak/session.h"
#include "core/openpak/session_detail.h"

namespace OpenPak::Session {

namespace {

// A 2048-bit RSA key. EVP_RSA_gen is OpenSSL 3; LibreSSL and older OpenSSL build it from
// RSA_generate_key_ex.
EVP_PKEY* GenerateRsaKey() {
#if defined(LIBRESSL_VERSION_NUMBER) || OPENSSL_VERSION_NUMBER < 0x30000000L
    EVP_PKEY* key = EVP_PKEY_new();
    RSA* rsa = RSA_new();
    BIGNUM* exponent = BN_new();
    const bool ok = key != nullptr && rsa != nullptr && exponent != nullptr &&
                    BN_set_word(exponent, RSA_F4) == 1 &&
                    RSA_generate_key_ex(rsa, 2048, exponent, nullptr) == 1 &&
                    EVP_PKEY_assign_RSA(key, rsa) == 1;
    BN_free(exponent);
    if (!ok) {
        RSA_free(rsa);
        EVP_PKEY_free(key);
        return nullptr;
    }
    return key;
#else
    return EVP_RSA_gen(2048);
#endif
}

// Nintendo's own names, on purpose: OpenPak answers to them and routes by SNI, so one address
// serves the whole chain and a title sees the names it was built to see.
constexpr const char* DauthHost = "dauth-lp1.ndas.srv.nintendo.net";
constexpr const char* AauthHost = "aauth-lp1.ndas.srv.nintendo.net";
constexpr const char* BaasHost = "e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com";

// Where an invitation sent by a console lands.
constexpr const char* FiveHost = "app.lp1.five.nintendo.net";

// Echoed back by the server, which knows the console from its client certificate rather than
// from anything in the request body.
constexpr const char* BaasClientId = "8f849b5d34778d8e";

constexpr int TimeoutSeconds = 20;

// Three renewals inside the lease the server hands out, so one lost request is not a person
// blinking offline.
constexpr int HeartbeatSeconds = 10;

// The inbox is store-and-forward and changes rarely; every third beat is often enough.
constexpr int InvitationEveryNthBeat = 3;

// The wire carries no expiry: the server keeps an invitation for a day and prunes it.
constexpr std::int64_t InvitationLifetimeSeconds = 24 * 60 * 60;

/// One firmware user's OpenPak identity. Every field but `login` is guarded by State::mutex;
/// `login` keeps two chains for the same user from crossing, and is never held by an accessor.
struct Identity {
    std::mutex login;

    bool loaded = false; ///< The device account was looked up in the secret store.
    std::string device_id;
    std::string device_password;

    std::string id_token;
    std::chrono::system_clock::time_point id_token_expiry;
    std::string id_token_application; ///< The title the cached id_token was minted for.
    std::string application_token;    ///< User-scoped after login: what presence speaks with.
    std::string user_id;
    std::uint64_t nsa_id = 0;
    std::string nickname;
    std::string thumbnail_url;
    std::string friend_code;
    bool linked = false;
    std::uint64_t na_id = 0;
    std::string birthday;
    std::string country;
    /// The friends surface (inbox, presence) answered 401 since the last sign-in: the server
    /// does not hold this user as linked, so nothing social is asked again until the next one.
    bool social_refused = false;
};

struct State {
    std::mutex mutex;

    std::string server_host;
    int port = 443;
    std::string ca_path;

    /// Keyed by uid (32 hex digits).
    std::map<std::string, std::shared_ptr<Identity>> identities;

    /// The user the friends caches, presence, the inbox and the push speak for.
    std::string active;

    std::string application_id;      ///< The bound title, 16 hex digits, or empty.
    std::string application_version; ///< The bound title's own version string.

    std::vector<std::uint8_t> ca_der; ///< The CA as the guest's certificate store wants it.
    std::vector<Invitation> invitations;
    std::map<std::string, std::string> sender_names;
    std::vector<std::string> dismissed;

    std::function<void(const Invitation&)> on_arrival;
    std::function<void()> on_invitations_changed;
    std::function<std::string()> current_title_id;

    std::thread heartbeat;
    std::condition_variable heartbeat_wake;
    std::mutex heartbeat_mutex;
    std::atomic<bool> heartbeat_running{false};

    /// Work the heartbeat does between beats, guarded by heartbeat_mutex. Whoever raises them may
    /// be a guest's IPC thread, which must never wait on the network itself.
    bool presence_dirty = false;
    bool signed_in_pending = false;

    /// The presence body the server last accepted. Guarded by presence_mutex, which also keeps
    /// two publishes from crossing.
    std::mutex presence_mutex;
    std::string published_presence;

    /// Whether the host names the running title itself (SetRunningApplication).
    std::atomic<bool> host_sets_application{false};

    /// Stop and join, never detach: a joinable std::thread destroyed at exit is an abort, and a
    /// detached one would read this object after it is gone.
    ~State() {
        if (heartbeat_running.exchange(false)) {
            heartbeat_wake.notify_all();
        }
        if (heartbeat.joinable()) {
            heartbeat.join();
        }
    }

    State() = default;
    State(const State&) = delete;
    State& operator=(const State&) = delete;
};

State& Get() {
    static State state;
    return state;
}

std::string Base64(const unsigned char* data, std::size_t size) {
    static constexpr char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    std::size_t i = 0;
    while (i + 2 < size) {
        const std::uint32_t n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out.push_back(table[(n >> 18) & 63]);
        out.push_back(table[(n >> 12) & 63]);
        out.push_back(table[(n >> 6) & 63]);
        out.push_back(table[n & 63]);
        i += 3;
    }
    if (i < size) {
        std::uint32_t n = static_cast<std::uint32_t>(data[i]) << 16;
        if (i + 1 < size) {
            n |= static_cast<std::uint32_t>(data[i + 1]) << 8;
        }
        out.push_back(table[(n >> 18) & 63]);
        out.push_back(table[(n >> 12) & 63]);
        out.push_back(i + 1 < size ? table[(n >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

std::vector<std::uint8_t> Base64Decode(std::string_view text) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };

    std::vector<std::uint8_t> out;
    int bits = 0;
    int accumulator = 0;

    for (const char c : text) {
        const int digit = value(c);
        if (digit < 0) {
            continue; // padding and whitespace carry nothing
        }
        accumulator = (accumulator << 6) | digit;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((accumulator >> bits) & 0xFF));
        }
    }

    return out;
}

/// A key for names: one device account and one certificate per server, because pointing the
/// console at a different OpenPak means a different device and reusing one would send the wrong
/// password to a stranger.
std::string ServerKeyOf(const std::string& host, int port) {
    std::string key = fmt::format("{}-{}", host, port);
    for (char& c : key) {
        if (c == ':' || c == '/' || c == '\\') {
            c = '-';
        }
    }
    return key;
}

/// Where to reach OpenPak, copied out of the state rather than read through it.
struct Endpoint {
    std::string host;
    int port = 443;
    std::string ca_path;
    std::string key;
};

Endpoint EndpointOfLocked(const State& state) {
    return {state.server_host, state.port, state.ca_path,
            ServerKeyOf(state.server_host, state.port)};
}

Endpoint EndpointSnapshot(State& state) {
    std::lock_guard lock{state.mutex};
    return EndpointOfLocked(state);
}

/// The per-server client certificate: the console, and a console has one.
std::filesystem::path DevicePath(const Endpoint& endpoint, const char* suffix) {
    return Platform::DataDir() / fmt::format("device-{}.{}", endpoint.key, suffix);
}

/// A self-signed certificate, kept per server, presented on every OpenPak host. A real console
/// proves itself with the certificate in its NAND; this is a stable identifier for the install,
/// and asserts nothing about hardware.
bool EnsureClientCertificate(const Endpoint& endpoint) {
    const std::filesystem::path cert_path = DevicePath(endpoint, "pem");
    const std::filesystem::path key_path = DevicePath(endpoint, "key");

    // The private key is readable by its owner only (installs from before this kept it 0644).
    constexpr auto owner_only = std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;
    if (std::filesystem::exists(cert_path) && std::filesystem::exists(key_path)) {
        std::error_code ec;
        std::filesystem::permissions(key_path, owner_only, ec);
        return true;
    }

    EVP_PKEY* key = GenerateRsaKey();
    if (key == nullptr) {
        LOG_ERROR(WebService, "[OpenPak] Could not generate a device key");
        return false;
    }

    X509* cert = X509_new();
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
    X509_set_version(cert, 2);

    // A day of backdating, because a clock that is behind rejects a not-yet-valid certificate.
    X509_gmtime_adj(X509_getm_notBefore(cert), -86400);
    X509_gmtime_adj(X509_getm_notAfter(cert), 10L * 365 * 86400);
    X509_set_pubkey(cert, key);

    // Built rather than borrowed from the certificate: OpenSSL 3 hands out the subject name
    // mutable and OpenSSL 4 hands out the same call as const, and Android builds against the
    // newer one. Setting a name we own is the spelling both accept.
    X509_NAME* name = X509_NAME_new();
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>("OpenPak Harbor device"), -1,
                               -1, 0);

    bool ok = X509_set_subject_name(cert, name) != 0 && X509_set_issuer_name(cert, name) != 0;

    X509_NAME_free(name);

    ok = ok && X509_sign(cert, key, EVP_sha256()) != 0;

    if (ok) {
        Platform::CreateParentDirs(cert_path);

        FILE* cert_file = std::fopen(cert_path.string().c_str(), "wb");
        ok = cert_file != nullptr && PEM_write_X509(cert_file, cert) != 0;
        if (cert_file != nullptr) {
            std::fclose(cert_file);
        }

        FILE* key_file = std::fopen(key_path.string().c_str(), "wb");
        if (key_file != nullptr) {
            // Restricted before the key is written, so it is never readable by others.
            std::error_code ec;
            std::filesystem::permissions(key_path, owner_only, ec);
        }
        ok = ok && key_file != nullptr &&
             PEM_write_PrivateKey(key_file, key, nullptr, nullptr, 0, nullptr, nullptr) != 0;
        if (key_file != nullptr) {
            std::fclose(key_file);
        }
    }

    X509_free(cert);
    EVP_PKEY_free(key);

    if (!ok) {
        LOG_ERROR(WebService, "[OpenPak] Could not write the device certificate");
    }

    return ok;
}

/// An HTTPS client for one Nintendo hostname that reaches the console server instead. The url
/// keeps the Nintendo name so SNI and the Host header carry it, while the socket goes to the
/// console server. Always verified against the pinned OpenPak CA: without one nothing connects.
std::unique_ptr<httplib::SSLClient> MakeClient(const Endpoint& endpoint, const char* host) {
    auto client = std::make_unique<httplib::SSLClient>(host, endpoint.port,
                                                       DevicePath(endpoint, "pem").string(),
                                                       DevicePath(endpoint, "key").string());

    client->set_hostname_addr_map({{host, endpoint.host}});
    client->set_ca_cert_path(endpoint.ca_path);
    client->enable_server_certificate_verification(true);

    client->set_connection_timeout(TimeoutSeconds, 0);
    client->set_read_timeout(TimeoutSeconds, 0);
    client->set_write_timeout(TimeoutSeconds, 0);
    client->set_follow_location(false);

    if (std::string header = Platform::ClientHeader(); !header.empty()) {
        client->set_default_headers({{"X-OpenPak-Client", std::move(header)}});
    }

    return client;
}

httplib::Headers Bearer(const std::string& token) {
    if (token.empty()) {
        return {};
    }
    return {{"Authorization", "Bearer " + token}};
}

/// A response body parsed, or nothing. Failures log the status and the body's size only: the
/// body of this chain carries tokens and ids.
std::optional<nlohmann::json> Parsed(const httplib::Result& result, std::string_view what) {
    if (!result) {
        LOG_WARNING(WebService, "[OpenPak] {} did not reach the server: {}", what,
                    httplib::to_string(result.error()));
        return std::nullopt;
    }
    if (result->status < 200 || result->status >= 300) {
        LOG_WARNING(WebService, "[OpenPak] {} returned {} ({} bytes)", what, result->status,
                    result->body.size());
        return std::nullopt;
    }

    nlohmann::json parsed = nlohmann::json::parse(result->body, nullptr, false);
    if (parsed.is_discarded()) {
        LOG_WARNING(WebService, "[OpenPak] {} answered with something that is not JSON", what);
        return std::nullopt;
    }

    return parsed;
}

std::string Text(const nlohmann::json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

std::int64_t Now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// Empty names the active user: the one acc has open.
std::string Resolve(const std::string& uid) {
    return uid.empty() ? Platform::ProfileId() : uid;
}

std::shared_ptr<Identity> IdentityOf(State& state, const std::string& uid) {
    std::lock_guard lock{state.mutex};
    auto& slot = state.identities[uid];
    if (!slot) {
        slot = std::make_shared<Identity>();
    }
    return slot;
}

std::shared_ptr<Identity> ActiveIdentity(State& state) {
    std::lock_guard lock{state.mutex};
    if (state.active.empty()) {
        return {};
    }
    const auto it = state.identities.find(state.active);
    return it != state.identities.end() ? it->second : nullptr;
}

/// Whether the friends surface may be asked for this identity: it is linked to a Nintendo
/// Account and the server has not refused it since its sign-in. An unlinked device account has
/// a valid token but no friends, inbox or presence, and every ask is a 401.
bool SocialAllowed(State& state, const std::shared_ptr<Identity>& identity) {
    if (!identity) {
        return false;
    }
    std::lock_guard lock{state.mutex};
    return identity->linked && !identity->social_refused;
}

/// The server refused a friends call for this identity: stop asking until the next sign-in.
void RefuseSocial(State& state, const std::shared_ptr<Identity>& identity) {
    bool first;
    {
        std::lock_guard lock{state.mutex};
        first = !identity->social_refused;
        identity->social_refused = true;
    }
    if (first) {
        LOG_INFO(WebService, "[OpenPak] The friends surface refused the active user (401): "
                             "treated as not linked until the next sign-in");
    }
}

/// The device account's name in the secret store: per server and per user.
std::string DeviceSecretKey(const Endpoint& endpoint, const std::string& uid) {
    return fmt::format("device-{}-{}", endpoint.key, uid);
}

void LoadDeviceAccount(const Endpoint& endpoint, const std::string& uid, std::string& id,
                       std::string& password) {
    const auto secret = LoadSecret(DeviceSecretKey(endpoint, uid));
    if (!secret || secret->empty()) {
        return;
    }
    const nlohmann::json parsed = nlohmann::json::parse(*secret, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        LOG_WARNING(WebService, "[OpenPak] Ignoring an unreadable device account");
        return;
    }
    id = Text(parsed, "id");
    password = Text(parsed, "password");
}

void SaveDeviceAccount(const Endpoint& endpoint, const std::string& uid, const std::string& id,
                       const std::string& password) {
    const nlohmann::json out{{"id", id}, {"password", password}};
    if (!StoreSecret(DeviceSecretKey(endpoint, uid), out.dump())) {
        LOG_WARNING(WebService, "[OpenPak] The secret store did not keep the device account");
    }
}

/// Five minutes of slack, and bound to the title it names. Called with State::mutex held.
bool FreshLocked(const State& state, const Identity& identity) {
    return !identity.id_token.empty() && identity.id_token_application == state.application_id &&
           std::chrono::system_clock::now() < identity.id_token_expiry - std::chrono::minutes(5);
}

/// What one chain produced, committed to the identity in one step.
struct Login {
    std::string device_id;
    std::string device_password;
    std::string id_token;
    std::chrono::system_clock::time_point id_token_expiry;
    std::string id_token_application;
    std::string application_token;
    std::string user_id;
    std::uint64_t nsa_id = 0;
    std::string nickname;
    std::string thumbnail_url;
    std::string friend_code;
    bool linked = false;
    std::uint64_t na_id = 0;
    std::string birthday;
    std::string country;
};

/// The login chain, with no lock held. With a Nintendo Account id_token it goes to /federation,
/// which is the same login plus the binding to a person.
bool RunChain(const Endpoint& endpoint, const std::string& uid, const std::string& application_id,
              const std::string& application_version, const std::string& account_id_token,
              Login& out, std::string& error) {
    if (!EnsureClientCertificate(endpoint)) {
        error = "could not create this install's device certificate";
        return false;
    }

    auto dauth = MakeClient(endpoint, DauthHost);

    const auto challenge_body = Parsed(dauth->Post("/v8/challenge"), "dauth challenge");
    if (!challenge_body) {
        error = "the console server did not answer";
        return false;
    }

    unsigned char mac[32];
    RAND_bytes(mac, sizeof(mac));

    // The server verifies none of this -- it has no Nintendo key to check the MAC with. Sent
    // because the shape is the console's.
    const nlohmann::json device_request{
        {"system_version", "22.2.0-0.0"},
        {"fw_revision", "0"},
        {"key_generation", 11},
        {"challenge", Text(*challenge_body, "challenge")},
        {"mac", Base64(mac, sizeof(mac))},
        {"ist", false},
        {"token_requests", nlohmann::json::array({{{"client_id", BaasClientId}}})},
    };

    const auto device_tokens =
        Parsed(dauth->Post("/v8/device_auth_tokens", device_request.dump(), "application/json"),
               "device auth tokens");
    if (!device_tokens || !device_tokens->contains("results") ||
        (*device_tokens)["results"].empty()) {
        error = "the console server refused this device";
        return false;
    }

    const std::string device_token = Text((*device_tokens)["results"][0], "device_auth_token");

    // The per-title binding: a title's own online stack checks that the id_token names it.
    std::string application_auth_token;
    if (!application_id.empty()) {
        auto aauth = MakeClient(endpoint, AauthHost);

        const httplib::Params application{
            {"application_id", application_id},
            {"application_version", application_version},
        };

        const auto answered =
            Parsed(aauth->Post("/v5/application_auth_token", Bearer(device_token), application),
                   "application auth token");
        if (answered) {
            application_auth_token = Text(*answered, "application_auth_token");
        }
    }

    auto baas = MakeClient(endpoint, BaasHost);

    const httplib::Params exchange{
        {"assertion", device_token},
        {"grant_type", "urn:ietf:params:oauth:grant-type:token-exchange"},
    };

    const auto application =
        Parsed(baas->Post("/1.0.0/application/token", exchange), "application token");
    if (!application) {
        error = "no application token";
        return false;
    }

    const std::string application_token = Text(*application, "accessToken");
    out.application_token = application_token;

    if (out.device_id.empty()) {
        const auto created = Parsed(
            baas->Post("/1.0.0/users", Bearer(application_token), "{}", "application/json"),
            "device account");
        if (!created || !created->contains("deviceAccounts") ||
            (*created)["deviceAccounts"].empty()) {
            error = "no device account";
            return false;
        }

        const nlohmann::json& account = (*created)["deviceAccounts"][0];
        out.device_id = Text(account, "id");
        out.device_password = Text(account, "password");

        SaveDeviceAccount(endpoint, uid, out.device_id, out.device_password);
    }

    httplib::Params login_params{{"id", out.device_id}, {"password", out.device_password}};
    if (!account_id_token.empty()) {
        login_params.emplace("idToken", account_id_token);
    }
    if (!application_auth_token.empty()) {
        login_params.emplace("appAuthNToken", application_auth_token);
    }

    const char* path = account_id_token.empty() ? "/1.0.0/login" : "/1.0.0/federation";

    const auto login = Parsed(baas->Post(path, Bearer(application_token), login_params), "login");
    if (!login) {
        error = account_id_token.empty() ? "the device was not signed in"
                                         : "the account was not linked";
        return false;
    }

    out.id_token = Text(*login, "idToken");
    out.id_token_application = application_id;

    const auto expires = login->find("expiresIn");
    const int seconds =
        expires != login->end() && expires->is_number() ? expires->get<int>() : 3600;
    out.id_token_expiry = std::chrono::system_clock::now() + std::chrono::seconds(seconds);

    if (login->contains("user")) {
        const nlohmann::json& user = (*login)["user"];

        out.user_id = Text(user, "id");
        out.nickname = Text(user, "nickname");
        out.thumbnail_url = Text(user, "thumbnailUrl");

        if (user.contains("links") && user["links"].contains("friendCode")) {
            out.friend_code = Text(user["links"]["friendCode"], "id");
        }
        out.birthday = Text(user, "birthday");
        out.country = Text(user, "country");
        if (user.contains("links") && user["links"].contains("nintendoAccount")) {
            out.na_id = std::strtoull(Text(user["links"]["nintendoAccount"], "id").c_str(),
                                      nullptr, 16);
        }
        // An unlinked device account carries no nickname and no NA link; a linked one both.
        out.linked = !out.nickname.empty() || out.na_id != 0;

        out.nsa_id = std::strtoull(out.user_id.c_str(), nullptr, 16);
    }
    if (!account_id_token.empty()) {
        out.linked = true;
    }

    // Presence is user-scoped: the login's own accessToken carries the BAAS user as its subject,
    // which the device-scoped exchange token does not.
    if (const std::string user_token = Text(*login, "accessToken"); !user_token.empty()) {
        out.application_token = user_token;
    }

    return true;
}

/// Sign `uid` in (or link it), committing the result. True when there is a fresh token after.
bool SignIn(State& state, const std::string& uid, const std::string& account_id_token,
            std::string& error) {
    if (uid.empty()) {
        error = "no firmware user is open";
        return false;
    }
    const std::shared_ptr<Identity> identity = IdentityOf(state, uid);
    std::lock_guard chain{identity->login};

    Endpoint endpoint;
    std::string application_id;
    std::string application_version;
    Login login;
    bool loaded;
    {
        std::lock_guard lock{state.mutex};
        if (state.server_host.empty()) {
            error = "no console server is configured";
            return false;
        }
        // Another caller may have finished the same chain while this one waited for it.
        if (account_id_token.empty() && FreshLocked(state, *identity)) {
            return true;
        }
        endpoint = EndpointOfLocked(state);
        application_id = state.application_id;
        application_version = state.application_version;
        login.device_id = identity->device_id;
        login.device_password = identity->device_password;
        loaded = identity->loaded;
    }
    if (!loaded) {
        LoadDeviceAccount(endpoint, uid, login.device_id, login.device_password);
    }

    const bool ok = RunChain(endpoint, uid, application_id, application_version,
                             account_id_token, login, error);

    bool active;
    {
        std::lock_guard lock{state.mutex};
        identity->loaded = true;
        identity->device_id = login.device_id;
        identity->device_password = login.device_password;
        if (ok) {
            identity->id_token = login.id_token;
            identity->id_token_expiry = login.id_token_expiry;
            identity->id_token_application = login.id_token_application;
            identity->application_token = login.application_token;
            identity->user_id = login.user_id;
            identity->nsa_id = login.nsa_id;
            identity->nickname = login.nickname;
            identity->thumbnail_url = login.thumbnail_url;
            identity->friend_code = login.friend_code;
            identity->linked = login.linked;
            identity->social_refused = false;
            identity->na_id = login.na_id;
            identity->birthday = login.birthday;
            identity->country = login.country;
        }
        active = uid == state.active;
    }
    if (!ok) {
        return false;
    }

    if (active && !login.user_id.empty()) {
        // The guest's friends module speaks to BAAS as the active user; with the picture, since a
        // title draws the local player from this entry.
        Baas::Attach(login.user_id);
        Baas::RememberUser({login.nsa_id, login.nickname, login.thumbnail_url, {}});

        // A console publishes its presence again on reconnect and syncs its lists when the
        // account becomes network-ready; this may be a worker the guest waits on, so the
        // heartbeat does both.
        {
            std::lock_guard wake{state.heartbeat_mutex};
            state.signed_in_pending = true;
        }
        state.heartbeat_wake.notify_all();
    }

    LOG_INFO(WebService, "[OpenPak] A firmware user signed in on the console server (linked: {})",
             login.linked);
    return true;
}

/// One presence body on the route a console's friends sysmodule uses, as the active user.
bool SendPresence(State& state, const std::string& body) {
    const auto identity = ActiveIdentity(state);
    if (!SocialAllowed(state, identity)) {
        return false;
    }
    std::string user_id;
    std::string device_id;
    std::string token;
    {
        std::lock_guard lock{state.mutex};
        user_id = identity->user_id;
        device_id = identity->device_id;
        token = identity->application_token;
    }

    // Unauthenticated presence is a 401 by design.
    if (user_id.empty() || device_id.empty() || token.empty()) {
        return false;
    }

    auto baas = MakeClient(EndpointSnapshot(state), BaasHost);

    const auto result =
        baas->Patch(fmt::format("/1.0.0/users/{}/device_accounts/{}", user_id, device_id),
                    Bearer(token), body, "application/json-patch+json");

    if (result && result->status == 401) {
        RefuseSocial(state, identity);
        return false;
    }
    if (!result || result->status < 200 || result->status >= 300) {
        LOG_DEBUG(WebService, "[OpenPak] Presence returned {}", result ? result->status : 0);
        return false;
    }

    return true;
}

/// Publish the presence, if it is not the one already published; `force` is the republish on
/// reconnect and the leased beat's.
void PublishPresence(State& state, bool force) {
    std::lock_guard gate{state.presence_mutex};

    const std::string body = PresenceBody();

    if (!force && body == state.published_presence) {
        return;
    }

    // Kept only once the server has it: a publish that failed is one to try again.
    if (SendPresence(state, body)) {
        state.published_presence = body;
    }
}

/// Say this console runs nothing (INACTIVE); the lease turns a silent client into offline.
void PublishInactive(State& state) {
    std::lock_guard gate{state.presence_mutex};

    SendPresence(state, OpenPak::Friends::PresencePatch({}));

    state.published_presence.clear();
}

/// The heartbeat's callback, for a host that has not named the running title itself.
void FollowTitle(State& state) {
    if (state.host_sets_application.load()) {
        return;
    }

    std::function<std::string()> title_of;
    {
        std::lock_guard lock{state.mutex};
        title_of = state.current_title_id;
    }

    std::uint64_t title = 0;
    if (title_of) {
        const std::string hex = title_of();
        title = hex.empty() ? 0 : std::strtoull(hex.c_str(), nullptr, 16);
    }

    auto& presence = OpenPak::Friends::PresenceState::Instance();
    if (presence.Application().first != title) {
        presence.SetRunningApplication(title, 0);
    }
}

/// Raise the invitation-count hook, outside every lock.
void InvitationsChanged(State& state) {
    std::function<void()> hook;
    {
        std::lock_guard lock{state.mutex};
        hook = state.on_invitations_changed;
    }

    if (hook) {
        hook();
    }
}

/// Who a BAAS id belongs to, by the lookup the friends module uses, kept once found.
std::string SenderName(State& state, const std::string& sender_id, const std::string& token) {
    {
        std::lock_guard lock{state.mutex};
        const auto it = state.sender_names.find(sender_id);
        if (it != state.sender_names.end()) {
            return it->second;
        }
    }

    auto baas = MakeClient(EndpointSnapshot(state), BaasHost);

    const auto found =
        Parsed(baas->Get(fmt::format("/1.0.0/users?filter.id.$in={}", sender_id), Bearer(token)),
               "user lookup");
    if (!found || !found->contains("items")) {
        return {};
    }

    for (const nlohmann::json& user : (*found)["items"]) {
        const std::string nickname = Text(user, "nickname");
        if (!nickname.empty()) {
            std::lock_guard lock{state.mutex};
            state.sender_names[sender_id] = nickname;
            return nickname;
        }
    }

    return {};
}

/// The active user follows the uid acc has open. The one leaving goes INACTIVE now, its
/// active-user caches go, and the one arriving is attached (or signed in by the caller). True
/// when there was a switch.
bool FollowProfile(State& state) {
    const std::string profile = Platform::ProfileId();

    {
        std::lock_guard lock{state.mutex};
        if (profile == state.active) {
            return false;
        }
    }

    PublishInactive(state);

    std::string user_id;
    std::uint64_t nsa_id = 0;
    std::string nickname;
    std::string thumbnail;
    {
        std::lock_guard lock{state.mutex};
        if (profile == state.active) {
            return false;
        }
        state.active = profile;
        state.invitations.clear();
        state.sender_names.clear();
        state.dismissed.clear();
        if (const auto it = state.identities.find(profile); it != state.identities.end()) {
            user_id = it->second->user_id;
            nsa_id = it->second->nsa_id;
            nickname = it->second->nickname;
            thumbnail = it->second->thumbnail_url;
        }
    }

    // Another user is another BAAS user: nothing cached for the last one may be read as this
    // one's, and its push connection is not this one's either.
    Baas::Detach();
    Push::Drop();

    if (!user_id.empty()) {
        Baas::Attach(user_id);
        Baas::RememberUser({nsa_id, nickname, thumbnail, {}});
        {
            std::lock_guard wake{state.heartbeat_mutex};
            state.signed_in_pending = true;
        }
        state.heartbeat_wake.notify_all();
    }

    LOG_INFO(WebService, "[OpenPak] The active firmware user changed");
    return true;
}

} // namespace

std::vector<std::uint8_t> CaCertificateDer() {
    State& state = Get();

    std::string path;
    {
        std::lock_guard lock{state.mutex};
        if (!state.ca_der.empty()) {
            return state.ca_der;
        }
        path = state.ca_path;
    }

    const std::string pem = path.empty() ? std::string{} : Platform::ReadFile(path);
    if (pem.empty()) {
        return {};
    }

    // PEM is the DER the store wants, base64 between the armour lines. The first certificate is
    // the CA.
    const std::size_t begin = pem.find("-----BEGIN CERTIFICATE-----");
    const std::size_t end = pem.find("-----END CERTIFICATE-----", begin);

    if (begin == std::string::npos || end == std::string::npos) {
        LOG_WARNING(WebService, "[OpenPak] ca.pem is not a PEM certificate");
        return {};
    }

    const std::size_t body = begin + std::strlen("-----BEGIN CERTIFICATE-----");
    std::vector<std::uint8_t> der = Base64Decode(std::string_view{pem}.substr(body, end - body));

    {
        std::lock_guard lock{state.mutex};
        state.ca_der = der;
    }

    return der;
}

std::unique_ptr<httplib::SSLClient> detail::MakeClientFor(const std::string& host) {
    State& state = Get();
    const Endpoint endpoint = EndpointSnapshot(state);
    EnsureClientCertificate(endpoint);
    return MakeClient(endpoint, host.c_str());
}

std::string detail::ServerKey() {
    State& state = Get();
    std::lock_guard lock{state.mutex};
    return state.server_host.empty() ? std::string{}
                                     : ServerKeyOf(state.server_host, state.port);
}

void Configure(std::string server_host, int port, std::string ca_path) {
    State& state = Get();
    std::lock_guard lock{state.mutex};

    if (state.server_host != server_host || state.port != port) {
        // Another server is another set of device accounts.
        state.identities.clear();
    }
    state.server_host = std::move(server_host);
    state.port = port;
    state.ca_path = ca_path.empty() ? Platform::CaPath().string() : std::move(ca_path);
    state.ca_der.clear();
}

void SetApplication(std::string application_id, std::string application_version) {
    State& state = Get();
    std::lock_guard lock{state.mutex};

    state.application_id = std::move(application_id);
    state.application_version = std::move(application_version);
}

bool Enabled() {
    State& state = Get();
    std::lock_guard lock{state.mutex};

    return !state.server_host.empty();
}

bool Ensure(const std::string& uid) {
    if (!Enabled()) {
        return false;
    }

    State& state = Get();
    FollowProfile(state);

    const std::string who = Resolve(uid);
    if (who.empty()) {
        return false;
    }
    {
        const auto identity = IdentityOf(state, who);
        std::lock_guard lock{state.mutex};
        if (FreshLocked(state, *identity)) {
            return true;
        }
    }

    std::string error;
    if (!SignIn(state, who, {}, error)) {
        // An empty token is the whole error path: the guest sees a console that is not online.
        LOG_WARNING(WebService, "[OpenPak] Sign-in failed: {}", error);
        return false;
    }

    return true;
}

std::future<bool> EnsureAsync(std::string uid) {
    // Resolved now: the user open when the guest asked is the one it asked for.
    return std::async(std::launch::async, [who = Resolve(uid)] { return Ensure(who); });
}

void EnsureAsync(std::string uid, std::function<void(bool)> done) {
    std::thread([who = Resolve(uid), done = std::move(done)] {
        const bool ok = Ensure(who);
        if (done) {
            done(ok);
        }
    }).detach();
}

std::string Link(const std::string& uid, const std::string& account_id_token) {
    if (!Enabled()) {
        return "no console server is configured";
    }

    State& state = Get();
    FollowProfile(state);

    std::string error;
    if (!SignIn(state, Resolve(uid), account_id_token, error)) {
        return error;
    }

    return {};
}

std::future<std::string> LinkAsync(std::string uid, std::string account_id_token) {
    return std::async(std::launch::async,
                      [who = Resolve(uid), token = std::move(account_id_token)] {
                          return Link(who, token);
                      });
}

namespace {

template <typename T, typename F>
T Read(const std::string& uid, F&& field) {
    State& state = Get();
    const std::string who = Resolve(uid);
    std::lock_guard lock{state.mutex};
    const auto it = state.identities.find(who);
    return it != state.identities.end() ? field(*it->second) : T{};
}

} // namespace

std::string IdToken(const std::string& uid) {
    return Read<std::string>(uid, [](const Identity& i) { return i.id_token; });
}

std::uint64_t NetworkServiceAccountId(const std::string& uid) {
    return Read<std::uint64_t>(uid, [](const Identity& i) { return i.nsa_id; });
}

std::string UserId(const std::string& uid) {
    return Read<std::string>(uid, [](const Identity& i) { return i.user_id; });
}

std::string Nickname(const std::string& uid) {
    return Read<std::string>(uid, [](const Identity& i) { return i.nickname; });
}

std::string FriendCode(const std::string& uid) {
    return Read<std::string>(uid, [](const Identity& i) { return i.friend_code; });
}

bool Linked(const std::string& uid) {
    return Read<bool>(uid, [](const Identity& i) { return i.linked; });
}

std::uint64_t NintendoAccountId(const std::string& uid) {
    return Read<std::uint64_t>(uid, [](const Identity& i) { return i.na_id; });
}

std::string Birthday(const std::string& uid) {
    return Read<std::string>(uid, [](const Identity& i) { return i.birthday; });
}

std::string Country(const std::string& uid) {
    return Read<std::string>(uid, [](const Identity& i) { return i.country; });
}

std::string ThumbnailUrl(const std::string& uid) {
    return Read<std::string>(uid, [](const Identity& i) { return i.thumbnail_url; });
}

std::string DauthToken(const std::string& client_id, const std::string& vendor) {
    if (!Enabled() || client_id.empty()) {
        return {};
    }
    try {
        const Endpoint endpoint = EndpointSnapshot(Get());
        if (!EnsureClientCertificate(endpoint)) {
            return {};
        }
        auto dauth = MakeClient(endpoint, DauthHost);

        const auto challenge = Parsed(dauth->Post("/v8/challenge"), "dauth challenge");
        if (!challenge) {
            return {};
        }

        unsigned char mac[32];
        RAND_bytes(mac, sizeof(mac));

        // The edge request's body is inferred from the server's reader and the module's vendor_id
        // string, as the Ryujinx Harbor sends it, not from a capture.
        nlohmann::json token_request{{"client_id", client_id}};
        if (!vendor.empty()) {
            token_request["vendor_id"] = vendor;
        }
        const nlohmann::json request{
            {"system_version", "22.2.0-0.0"},
            {"fw_revision", "0"},
            {"key_generation", 11},
            {"challenge", Text(*challenge, "challenge")},
            {"mac", Base64(mac, sizeof(mac))},
            {"ist", false},
            {"token_requests", nlohmann::json::array({token_request})},
        };

        const char* path = vendor.empty() ? "/v8/device_auth_tokens" : "/v8/edge_tokens";
        const auto tokens = Parsed(dauth->Post(path, request.dump(), "application/json"),
                                   vendor.empty() ? "device auth token" : "edge token");
        if (!tokens || !tokens->contains("results") || !(*tokens)["results"].is_array() ||
            (*tokens)["results"].empty()) {
            return {};
        }
        return Text((*tokens)["results"][0], vendor.empty() ? "device_auth_token" : "dtoken");
    } catch (const std::exception& e) {
        LOG_WARNING(WebService, "[OpenPak] dauth token: {}", e.what());
        return {};
    }
}

void SetInvitationHook(std::function<void(const Invitation&)> on_arrival) {
    State& state = Get();
    std::lock_guard lock{state.mutex};
    state.on_arrival = std::move(on_arrival);
}

void SetInvitationsChangedHook(std::function<void()> on_change) {
    State& state = Get();
    std::lock_guard lock{state.mutex};
    state.on_invitations_changed = std::move(on_change);
}

std::vector<Invitation> Invitations() {
    State& state = Get();
    std::lock_guard lock{state.mutex};
    return state.invitations;
}

bool RefreshInvitations() {
    State& state = Get();
    const auto identity = ActiveIdentity(state);
    if (!SocialAllowed(state, identity)) {
        return false;
    }

    std::string user_id;
    std::string token;
    std::vector<std::string> dismissed;
    std::vector<Invitation> previous;
    {
        std::lock_guard lock{state.mutex};
        user_id = identity->user_id;
        token = identity->application_token;
        dismissed = state.dismissed;
        previous = state.invitations;
    }

    if (user_id.empty() || token.empty()) {
        return false;
    }

    auto five = MakeClient(EndpointSnapshot(state), FiveHost);

    // Read state is not a filter: the same account signed in on a console marks these read from
    // over there, and that is no reason for this machine to have missed it.
    const auto reply =
        five->Get(fmt::format("/v2/users/{}/invitations/inbox?invitation_types=friend", user_id),
                  Bearer(token));
    if (reply && reply->status == 401) {
        RefuseSocial(state, identity);
        return false;
    }
    const auto inbox = Parsed(reply, "invitation inbox");

    if (!inbox || !inbox->contains("items")) {
        return false;
    }

    std::vector<Invitation> waiting;
    std::vector<Invitation> arrived;

    for (const nlohmann::json& item : (*inbox)["items"]) {
        // Ids are JSON numbers on the wire because the friends module parses them with %llu; a
        // string id would not survive that parser. Everything above here wants the digits.
        const auto id_field = item.find("id");
        if (id_field == item.end() || !id_field->is_number()) {
            continue;
        }

        Invitation invitation;
        invitation.id = std::to_string(id_field->get<std::uint64_t>());

        if (std::find(dismissed.begin(), dismissed.end(), invitation.id) != dismissed.end()) {
            continue;
        }

        invitation.sender_id = Text(item, "sender_id");
        invitation.title_id = Text(item, "application_id");

        // Absent when the sender carried no payload, which the wire says by omitting the field.
        invitation.app_param = Base64Decode(Text(item, "application_data"));

        const auto created = item.find("created_at");
        invitation.created_at =
            created != item.end() && created->is_number() ? created->get<std::int64_t>() : Now();
        invitation.expires_at = invitation.created_at + InvitationLifetimeSeconds;

        invitation.sender_name = SenderName(state, invitation.sender_id, token);
        if (invitation.sender_name.empty()) {
            invitation.sender_name = invitation.sender_id;
        }

        if (const auto written = item.find("messages");
            written != item.end() && written->is_object()) {
            for (const auto& [language, text] : written->items()) {
                if (text.is_string() && text.get<std::string>().find_first_not_of(" \t\r\n") !=
                                            std::string::npos) {
                    invitation.messages[language] = text.get<std::string>();
                }
            }
        }

        const bool known = std::any_of(previous.begin(), previous.end(),
                                       [&](const Invitation& seen) { return seen.id == invitation.id; });

        waiting.push_back(invitation);

        if (!known) {
            arrived.push_back(invitation);
        }
    }

    std::function<void(const Invitation&)> hook;
    bool changed;
    {
        std::lock_guard lock{state.mutex};
        changed = state.invitations.size() != waiting.size() || !arrived.empty();
        state.invitations = waiting;
        hook = state.on_arrival;
    }

    if (hook) {
        for (const Invitation& invitation : arrived) {
            hook(invitation);
        }
    }

    if (changed) {
        InvitationsChanged(state);
    }

    return true;
}

void DismissInvitation(const std::string& invitation_id) {
    State& state = Get();
    const auto identity = ActiveIdentity(state);

    std::string token;
    bool removed;
    {
        std::lock_guard lock{state.mutex};
        state.dismissed.push_back(invitation_id);
        removed = std::erase_if(state.invitations, [&](const Invitation& one) {
                      return one.id == invitation_id;
                  }) != 0;
        token = identity ? identity->application_token : std::string{};
    }

    if (removed) {
        InvitationsChanged(state);
    }

    if (token.empty()) {
        return;
    }

    auto five = MakeClient(EndpointSnapshot(state), FiveHost);

    const nlohmann::json patch = nlohmann::json::array({
        {{"op", "replace"},
         {"path", fmt::format("/{}/extras/receiver/read", invitation_id)},
         {"value", true}},
    });

    const auto result =
        five->Patch("/v1/invitations", Bearer(token), patch.dump(), "application/json");

    if (!result || result->status != 200) {
        LOG_DEBUG(WebService, "[OpenPak] Marking an invitation read did not land ({})",
                  result ? result->status : 0);
    }
}

Response SendAs(const std::string& method, const std::string& host,
                const std::string& path_and_query, const std::string& content_type,
                const std::string& body) {
    try {
        Ensure({});

        State& state = Get();
        const auto identity = ActiveIdentity(state);
        if (!identity) {
            return {};
        }
        std::string token;
        {
            std::lock_guard lock{state.mutex};
            token = identity->application_token;
        }

        if (token.empty()) {
            return {};
        }

        httplib::Request request;
        request.method = method;
        request.path = path_and_query;
        request.headers = Bearer(token);

        if (!content_type.empty()) {
            request.set_header("Content-Type", content_type + "; charset=utf-8");
            request.body = body;
        }

        auto client = MakeClient(EndpointSnapshot(state), host.c_str());
        const httplib::Result result = client->send(request);

        if (!result) {
            // The path carries user ids: the host is enough to know what failed.
            LOG_DEBUG(WebService, "[OpenPak] {} {} did not reach the server: {}", method, host,
                      httplib::to_string(result.error()));
            return {};
        }

        return {result->status, result->body};
    } catch (const std::exception& e) {
        LOG_DEBUG(WebService, "[OpenPak] {} {}: {}", method, host, e.what());
        return {};
    }
}

void StartHeartbeat(std::function<std::string()> current_title_id) {
    State& state = Get();

    {
        std::lock_guard lock{state.mutex};
        state.current_title_id = std::move(current_title_id);
    }

    // What the running title declares through friend:u is published when it changes, as the
    // module publishes it. The declaration arrives on a guest's IPC thread, so it is only noted
    // there and the heartbeat sends it.
    OpenPak::Friends::PresenceState::Instance().SetChangedCallback([&state] {
        {
            std::lock_guard wake{state.heartbeat_mutex};
            state.presence_dirty = true;
        }
        state.heartbeat_wake.notify_all();
    });

    if (state.heartbeat_running.exchange(true)) {
        return;
    }

    // Pushed changes arrive between beats; the beat below stays as the fallback.
    Push::Start();

    state.heartbeat = std::thread([&state] {
        using Clock = std::chrono::steady_clock;
        Clock::time_point next_beat = Clock::now() + std::chrono::seconds(HeartbeatSeconds);

        for (int tick = 0;;) {
            bool presence_changed = false;
            bool signed_in = false;
            {
                std::unique_lock lock{state.heartbeat_mutex};
                state.heartbeat_wake.wait_until(lock, next_beat, [&state] {
                    return !state.heartbeat_running.load() || state.presence_dirty ||
                           state.signed_in_pending;
                });
                presence_changed = std::exchange(state.presence_dirty, false);
                signed_in = std::exchange(state.signed_in_pending, false);
            }

            if (!state.heartbeat_running.load()) {
                return;
            }

            const bool beat = Clock::now() >= next_beat;
            if (beat) {
                // A beat that ran long is not made up for with a burst of them.
                next_beat = (std::max)(next_beat + std::chrono::seconds(HeartbeatSeconds),
                                     Clock::now());
            }

            // Nothing thrown here may leave the beat: on this thread it would end the emulator,
            // and a beat that stops is presence that stops (Ryujinx lost both on a friend-list
            // change, 2026-09-22).
            try {
                // A console publishes its presence again whenever it reconnects, and syncs its
                // lists when the account becomes network-ready.
                if (signed_in) {
                    FollowTitle(state);
                    PublishPresence(state, true);
                    if (SocialAllowed(state, ActiveIdentity(state))) {
                        OpenPak::Baas::BackgroundSync();
                    }
                }

                if (beat) {
                    // One beat for the life of the process: across a profile switch it speaks for
                    // whoever is signed in now, which may mean signing them in first.
                    if (FollowProfile(state)) {
                        Ensure({});
                    }

                    FollowTitle(state);

                    // The inbox is store-and-forward and changes rarely; every third beat is
                    // often enough to hear about an invitation while the person still cares. The
                    // friends module's own caches ride along -- the request boxes, the friend
                    // list at the module's 30 s cooldown, the block list, the own user and the
                    // invitation inbox -- because a console is pushed changes to them and nothing
                    // here holds a push connection to be pushed on.
                    if (tick % InvitationEveryNthBeat == 0 &&
                        SocialAllowed(state, ActiveIdentity(state))) {
                        RefreshInvitations();
                        OpenPak::Baas::BackgroundSync();
                    }

                    // Presence goes out on a change, as the module publishes it -- and once every
                    // third beat regardless, because the server's claim is leased and a person
                    // who has not changed game in a while would otherwise read as offline while
                    // sitting in front of the machine.
                    PublishPresence(state, tick % InvitationEveryNthBeat == 0);
                } else if (presence_changed) {
                    PublishPresence(state, false);
                }
            } catch (const std::exception& e) {
                LOG_WARNING(WebService, "[OpenPak] Heartbeat beat failed: {}", e.what());
            }

            if (beat) {
                tick++;
            }
        }
    });
}

void SetRunningApplication(std::uint64_t application_id, std::uint64_t presence_group_id) {
    State& state = Get();
    state.host_sets_application.store(true);

    auto& presence = OpenPak::Friends::PresenceState::Instance();
    if (presence.Application() == std::pair{application_id,
                                            presence_group_id != 0 ? presence_group_id
                                                                   : application_id}) {
        return;
    }

    presence.SetRunningApplication(application_id, presence_group_id);

    // A title starting or ending is a presence change of its own: the console publishes ONLINE
    // when an application registers and INACTIVE when none is running.
    {
        std::lock_guard wake{state.heartbeat_mutex};
        state.presence_dirty = true;
    }
    state.heartbeat_wake.notify_all();
}


bool Beating() {
    return Get().heartbeat_running.load() && !UserId({}).empty();
}

std::string PresenceBody() {
    return OpenPak::Friends::PresencePatch(
        OpenPak::Friends::PresenceState::Instance().Current(Platform::ProfileId()));
}

bool HasDeviceAccount(const std::string& uid) {
    if (uid.empty()) {
        return false;
    }
    const Endpoint endpoint = EndpointSnapshot(Get());
    if (endpoint.host.empty()) {
        return false;
    }
    std::string id;
    std::string password;
    LoadDeviceAccount(endpoint, uid, id, password);
    return !id.empty() && !password.empty();
}

void ForgetProfile(const std::string& uid) {
    if (uid.empty()) {
        return;
    }

    State& state = Get();
    const Endpoint endpoint = EndpointSnapshot(state);
    if (!endpoint.host.empty()) {
        EraseSecret(DeviceSecretKey(endpoint, uid));
    }
    {
        std::lock_guard lock{state.mutex};
        state.identities.erase(uid);
    }

    Push::ForgetProfile(uid);
}

void GoOffline() {
    State& state = Get();

    Push::Stop();

    if (state.heartbeat_running.exchange(false)) {
        state.heartbeat_wake.notify_all();

        if (state.heartbeat.joinable()) {
            state.heartbeat.join();
        }
    }

    // The one moment "I am gone" can be said; if it never arrives, the lease says it later.
    PublishInactive(state);
}

Response Send(const std::string& method, const std::string& host,
              const std::string& path_and_query, const Headers& headers,
              const std::string& content_type, const std::string& body) {
    try {
        if (!Enabled()) {
            return {};
        }
        State& state = Get();
        const Endpoint endpoint = EndpointSnapshot(state);
        EnsureClientCertificate(endpoint);

        httplib::Request request;
        request.method = method;
        request.path = path_and_query;
        for (const auto& [name, value] : headers) {
            request.headers.emplace(name, value);
        }
        if (!content_type.empty()) {
            request.set_header("Content-Type", content_type);
            request.body = body;
        }

        auto client = MakeClient(endpoint, host.c_str());
        const httplib::Result result = client->send(request);
        if (!result) {
            LOG_DEBUG(WebService, "[OpenPak] {} {} did not reach the server: {}", method, host,
                      httplib::to_string(result.error()));
            return {};
        }
        return {result->status, result->body};
    } catch (const std::exception& e) {
        LOG_DEBUG(WebService, "[OpenPak] {} {}: {}", method, host, e.what());
        return {};
    }
}

} // namespace OpenPak::Session

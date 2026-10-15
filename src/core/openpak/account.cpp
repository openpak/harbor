// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <array>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

#include <fmt/format.h>
#include "common/httplib.h"
#include <nlohmann/json.hpp>
#include <openssl/sha.h>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "core/openpak/account.h"
#include "core/openpak/network_profile.h"
#include "core/openpak/platform.h"
#include "core/openpak/secret_store.h"
#include "core/openpak/session.h"

namespace OpenPak::Account {

namespace {

// The secret store entry: {"bearer", "expires_at", "linked_uid"}. The uid is not a secret, but it
// belongs with the account it was linked to. One per install: the OS store is the person's, shared
// by every data directory on the machine, so the key names this one (a digest of its path).
std::string SecretKey() {
    const std::string dir =
        Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir).lexically_normal().string();
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256(reinterpret_cast<const unsigned char*>(dir.data()), dir.size(), digest.data());
    std::string key = "website-account-";
    for (std::size_t i = 0; i < 8; i++) {
        key += fmt::format("{:02x}", digest[i]);
    }
    return key;
}

// The id_token's audience: the console's own Nintendo Account client (nsd "NxAccount"), as the
// firmware's sign-in would ask for it. The server does not check it.
constexpr std::string_view LinkClientId = "6ffd70c434d303c8";

struct Kept {
    std::string bearer;
    std::string expires_at;
    std::string linked_uid;
    std::string display_name; // the account's nickname as Me() last read it; not a secret
};

std::mutex& StateMutex() {
    static std::mutex mutex;
    return mutex;
}

std::optional<Kept> Load() {
    const auto text = LoadSecret(SecretKey());
    if (!text) {
        return std::nullopt;
    }
    const auto doc = nlohmann::json::parse(*text, nullptr, false);
    if (!doc.is_object() || !doc.contains("bearer") || !doc.at("bearer").is_string()) {
        return std::nullopt;
    }
    Kept kept;
    kept.bearer = doc.at("bearer").get<std::string>();
    kept.expires_at = doc.value("expires_at", std::string{});
    kept.linked_uid = doc.value("linked_uid", std::string{});
    kept.display_name = doc.value("display_name", std::string{});
    return kept.bearer.empty() ? std::nullopt : std::optional{kept};
}

bool Keep(const Kept& kept) {
    const nlohmann::json doc{
        {"bearer", kept.bearer},
        {"expires_at", kept.expires_at},
        {"linked_uid", kept.linked_uid},
        {"display_name", kept.display_name}};
    return StoreSecret(SecretKey(), doc.dump());
}

std::unique_ptr<httplib::Client> Client() {
    auto client = std::make_unique<httplib::Client>(NetworkProfile::Site());
    client->set_connection_timeout(10);
    client->set_read_timeout(15);
    Platform::ApplySystemCa(*client);
    httplib::Headers headers{{"Accept", "application/json"}};
    if (std::string header = Platform::ClientHeader(); !header.empty()) {
        headers.emplace("X-OpenPak-Client", std::move(header));
    }
    client->set_default_headers(std::move(headers));
    return client;
}

httplib::Headers Bearer(const std::string& bearer) {
    return {{"Authorization", "Bearer " + bearer}};
}

/// The API's error answer {"error", "code"} as a Status. Status 0: no answer at all.
Status ToStatus(const httplib::Result& result) {
    if (!result) {
        return Status::Offline;
    }
    const int status = result->status;
    const auto doc = nlohmann::json::parse(result->body, nullptr, false);
    // {"error": English message, "code": stable id}: the code is what a client reads.
    const std::string error =
        doc.is_object() && doc.contains("code") && doc.at("code").is_string()
            ? doc.at("code").get<std::string>()
            : std::string{};
    if (status == 429 || error == "rate_limited") {
        return Status::RateLimited;
    }
    if (error == "registrations_closed") {
        return Status::RegistrationsClosed;
    }
    if (error == "account_suspended") {
        return Status::Suspended;
    }
    if (error == "invalid_credentials") {
        return Status::InvalidCredentials;
    }
    if (status == 401) {
        return Status::SignedOut;
    }
    return Status::Failed;
}

std::string Json(nlohmann::json doc) {
    return doc.dump();
}

/// Logs a failed call by path and status only: the answers carry account data.
void LogFailure(std::string_view what, const httplib::Result& result) {
    LOG_WARNING(WebService, "[OpenPak] account: {} failed ({})", what,
                result ? std::to_string(result->status) : httplib::to_string(result.error()));
}

} // namespace

Status SignIn(std::string_view email, std::string_view password) {
    const auto result =
        Client()->Post("/api/v1/token",
                       Json({{"email", std::string{email}}, {"password", std::string{password}}}),
                       "application/json");
    if (!result || (result->status != 201 && result->status != 200 && result->status != 202)) {
        LogFailure("sign-in", result);
        return ToStatus(result);
    }
    const auto doc = nlohmann::json::parse(result->body, nullptr, false);
    if (doc.is_object() && doc.value("two_factor_required", false)) {
        LOG_INFO(WebService, "[OpenPak] account: two-step verification is required");
        return Status::TwoFactorRequired;
    }
    if (!doc.is_object() || !doc.contains("token") || !doc.at("token").is_string()) {
        LOG_WARNING(WebService, "[OpenPak] account: sign-in answered without a token");
        return Status::Failed;
    }
    std::scoped_lock lock{StateMutex()};
    Kept kept;
    kept.bearer = doc.at("token").get<std::string>();
    kept.expires_at = doc.value("expires_at", std::string{});
    if (!Keep(kept)) {
        LOG_WARNING(WebService, "[OpenPak] account: the sign-in could not be kept");
        return Status::Failed;
    }
    LOG_INFO(WebService, "[OpenPak] account: signed in");
    return Status::Ok;
}

Status Register(std::string_view email, std::string_view password, std::string_view nickname,
                std::string_view country, std::string_view birthday) {
    nlohmann::json body{{"email", std::string{email}},
                        {"password", std::string{password}},
                        {"display_name", std::string{nickname}}};
    if (!country.empty()) {
        body["country"] = std::string{country};
    }
    if (!birthday.empty()) {
        body["birthday"] = std::string{birthday};
    }
    const auto result = Client()->Post("/api/v1/register", Json(body), "application/json");
    if (!result || (result->status != 202 && result->status != 200 && result->status != 201)) {
        LogFailure("registration", result);
        return ToStatus(result);
    }
    LOG_INFO(WebService, "[OpenPak] account: registration asked");
    return Status::Ok;
}

std::optional<Profile> Me(Status* status) {
    const auto kept = Load();
    if (!kept) {
        if (status) {
            *status = Status::SignedOut;
        }
        return std::nullopt;
    }
    const auto result = Client()->Get("/api/v1/me", Bearer(kept->bearer));
    if (!result || result->status != 200) {
        LogFailure("profile", result);
        const Status failure = ToStatus(result);
        if (failure == Status::SignedOut) {
            // Expired, revoked or reset: nothing to keep.
            SignOut();
        }
        if (status) {
            *status = failure;
        }
        return std::nullopt;
    }
    const auto doc = nlohmann::json::parse(result->body, nullptr, false);
    if (!doc.is_object()) {
        if (status) {
            *status = Status::Failed;
        }
        return std::nullopt;
    }
    Profile profile;
    profile.email_verified = doc.value("email_verified", false);
    profile.display_name = doc.value("display_name", std::string{});
    if (!profile.display_name.empty()) {
        // Kept for the firmware user the setup's account creates before the network is asked.
        std::scoped_lock lock{StateMutex()};
        if (auto latest = Load(); latest && latest->display_name != profile.display_name) {
            latest->display_name = profile.display_name;
            Keep(*latest);
        }
    }
    if (status) {
        *status = Status::Ok;
    }
    return profile;
}

std::string KnownDisplayName() {
    if (const auto kept = Load(); kept && !kept->display_name.empty()) {
        return kept->display_name;
    }
    // An account signed in before the name was kept: one read, which keeps it.
    if (const auto me = Me()) {
        return me->display_name;
    }
    return {};
}

Status ResendVerification() {
    const auto kept = Load();
    if (!kept) {
        return Status::SignedOut;
    }
    const auto result = Client()->Post("/api/v1/me/verify/resend", Bearer(kept->bearer), "{}",
                                       "application/json");
    if (!result || result->status / 100 != 2) {
        LogFailure("verification resend", result);
        return ToStatus(result);
    }
    return Status::Ok;
}

bool HasAccount() {
    return Load().has_value();
}

void SignOut() {
    std::optional<Kept> kept;
    {
        std::scoped_lock lock{StateMutex()};
        kept = Load();
        EraseSecret(SecretKey());
    }
    if (kept) {
        // Best effort: the bearer is forgotten here whatever the server says.
        Client()->Delete("/api/v1/token", Bearer(kept->bearer));
        LOG_INFO(WebService, "[OpenPak] account: signed out");
    }
}

std::string LinkToken(Status* status) {
    const auto kept = Load();
    if (!kept) {
        if (status) {
            *status = Status::SignedOut;
        }
        return {};
    }
    const auto result =
        Client()->Post("/api/v1/me/switch/link", Bearer(kept->bearer),
                       Json({{"client_id", std::string{LinkClientId}}}), "application/json");
    const auto doc = result && result->status / 100 == 2
                         ? nlohmann::json::parse(result->body, nullptr, false)
                         : nlohmann::json{};
    if (!doc.is_object() || !doc.contains("id_token") || !doc.at("id_token").is_string()) {
        LogFailure("link token", result);
        if (status) {
            *status = result && result->status / 100 == 2 ? Status::Failed : ToStatus(result);
        }
        return {};
    }
    if (status) {
        *status = Status::Ok;
    }
    return doc.at("id_token").get<std::string>();
}

namespace {

/// The background link for OnFirmwareUserRegistered: one at a time.
void LinkUser(const std::string& uid,
              const std::function<void(const std::string& id_token)>& on_linked) {
    static std::mutex link_mutex;
    std::scoped_lock link_lock{link_mutex};

    auto kept = Load();
    if (!kept) {
        return;
    }
    if (!kept->linked_uid.empty() && kept->linked_uid != uid &&
        Session::HasDeviceAccount(kept->linked_uid)) {
        LOG_INFO(WebService, "[OpenPak] account: another user is already linked to the setup's "
                             "account; this one links in the firmware if wanted");
        return;
    }
    // The server's link route does not ask for a verified address; Harbor does.
    Status status = Status::Failed;
    const auto profile = Me(&status);
    if (!profile) {
        LOG_WARNING(WebService, "[OpenPak] account: the new user was not linked (no profile)");
        return;
    }
    if (!profile->email_verified) {
        LOG_INFO(WebService, "[OpenPak] account: the address is not verified yet; the new user "
                             "was not linked");
        return;
    }
    const std::string id_token = LinkToken(&status);
    if (id_token.empty()) {
        LOG_WARNING(WebService, "[OpenPak] account: the new user was not linked (no link token)");
        return;
    }
    if (const std::string error = Session::Link(uid, id_token); !error.empty()) {
        LOG_WARNING(WebService, "[OpenPak] account: the new user was not linked: {}", error);
        return;
    }
    {
        std::scoped_lock lock{StateMutex()};
        if (auto latest = Load()) {
            latest->linked_uid = uid;
            Keep(*latest);
        }
    }
    LOG_INFO(WebService, "[OpenPak] account: the new user is linked to the setup's account");
    if (on_linked) {
        on_linked(id_token);
    }
}

} // namespace

} // namespace OpenPak::Account

namespace OpenPak {

namespace {

// Users whose link OnFirmwareUserRegistered is still making.
std::mutex pending_mutex;
std::condition_variable pending_changed;
std::set<std::string> pending_links;

} // namespace

void OnFirmwareUserRegistered(const std::string& uid,
                              std::function<void(const std::string& id_token)> on_linked) {
    if (uid.empty() || !Session::Enabled() || !Account::HasAccount()) {
        return;
    }
    {
        std::scoped_lock lock{pending_mutex};
        pending_links.insert(uid);
    }
    // Network round trips (the website, then dauth, the device account and federation): never on
    // the caller's thread, which answers the guest.
    std::thread([uid, on_linked = std::move(on_linked)] {
        Account::LinkUser(uid, on_linked);
        {
            std::scoped_lock lock{pending_mutex};
            pending_links.erase(uid);
        }
        pending_changed.notify_all();
    }).detach();
}

void WaitForFirmwareUserLink(const std::string& uid) {
    // A link takes ~1.5 s (four round trips); a slow network gets the honest "not linked" after this.
    constexpr auto MaxWait = std::chrono::seconds{8};
    std::unique_lock lock{pending_mutex};
    if (!pending_links.contains(uid)) {
        return;
    }
    LOG_INFO(WebService, "[OpenPak] account: waiting for the new user's link before answering");
    pending_changed.wait_for(lock, MaxWait, [&] { return !pending_links.contains(uid); });
}

} // namespace OpenPak

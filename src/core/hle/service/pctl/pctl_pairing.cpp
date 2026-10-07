// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).

#include <array>
#include <chrono>
#include <cstdio>
#include <span>
#include <stdexcept>
#include <condition_variable>
#include <map>
#include <mutex>
#include <stop_token>
#include <thread>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "common/logging.h"
#include "common/redact.h"
#include "core/hle/service/pctl/pctl_pairing.h"
#include "core/hle/service/psc/ovln/ovln_agent.h"
#include "core/hle/service/psc/ovln/ovln_types.h"
#include "core/openpak/async_worker.h"
#include "core/openpak/push.h"
#include "core/openpak/secret_store.h"
#include "core/openpak/session.h"
#include "core/openpak/session_detail.h"

namespace Service::PCTL {

namespace {

using json = nlohmann::json;

constexpr Moon::Description Unreachable = 2003;
constexpr std::uint32_t TagSettingsChanged = 4000;
constexpr std::uint32_t TagUnlinked = 4002;
/// A server key always ends in "-<port>", so this never names a server.
constexpr const char* LocalKey = "local";

struct Reply {
    int status = 0;
    std::string body;
    std::string etag;
    std::string content_type;
};

std::int64_t Now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string DecodeRecord(const std::string& stored);

struct Globals {
    std::mutex mutex;
    std::map<std::string, std::unique_ptr<Pairing>> pairings;
    std::weak_ptr<OpenPak::AsyncWorker> worker;
    std::map<int, std::function<void()>> listeners;
    int next_listener = 1;
};

Globals& GetGlobals() {
    static Globals globals;
    return globals;
}

/// Raise every listener; each is a HostEvent raise (thread-safe, never blocks).
void NotifyChanged() {
    std::vector<std::function<void()>> listeners;
    {
        auto& globals = GetGlobals();
        std::lock_guard lock{globals.mutex};
        for (const auto& [id, listener] : globals.listeners) {
            listeners.push_back(listener);
        }
    }
    for (const auto& listener : listeners) {
        listener();
    }
}

/// An overlay notification, sent on the pctl worker's signalling thread (the kernel-registered
/// one), as pctl's SDK default sender sends it (queue 1, the newest replaces an unreceived one).
void SendOverlay(std::uint32_t tag, std::vector<std::uint8_t> payload, const char* what) {
    std::shared_ptr<OpenPak::AsyncWorker> worker;
    {
        auto& globals = GetGlobals();
        std::lock_guard lock{globals.mutex};
        worker = globals.worker.lock();
    }
    if (!worker) {
        LOG_WARNING(Service_PCTL, "pctl: overlay {} ({}) dropped, no session", tag, what);
        return;
    }
    worker->Post([tag, payload = std::move(payload), what] {
        const auto notification = PSC::MakeNotification(tag, payload);
        const bool sent = PSC::GetOvlnAgent().Send(
            "overlay", 0x100,
            std::span<const u8, sizeof(PSC::OverlayNotification)>{
                reinterpret_cast<const u8*>(notification.data()), sizeof(PSC::OverlayNotification)});
        LOG_INFO(Service_PCTL, "pctl: overlay {} ({}) {}", tag, what, sent ? "sent" : "not queued");
    });
}

void SendSettingsChanged(std::uint32_t mask) {
    std::vector<std::uint8_t> payload(4);
    for (int i = 0; i < 4; ++i) {
        payload[i] = static_cast<std::uint8_t>(mask >> (8 * i));
    }
    SendOverlay(TagSettingsChanged, std::move(payload), "setting changed");
}

/// 4002: the parent unlinked the console (0 by push, 1 found by a sync).
void SendUnlinked(std::uint8_t source) {
    SendOverlay(TagUnlinked, {source, 0, 0, 0},
                source == 0 ? "unlinked by the parent" : "the server no longer knows the device");
}

} // namespace

struct Pairing::Impl {
    std::string server_key;

    mutable std::mutex mutex;

    std::string token;
    std::chrono::steady_clock::time_point token_expiry{};

    // The record the last code found (not kept: a code is good once).
    std::uint64_t found_device = 0;
    std::vector<Moon::Owner> found_owners;

    // The paired device (kept).
    std::uint64_t paired_device = 0;
    bool active = false;
    std::string setting_etag;
    std::int64_t settings_last_updated = 0;

    // The setting in force as the parent last sent it (kept as served, so a restart reapplies it)
    // and the PIN: the document's when it carries one, else the one set on the console.
    std::optional<Setting> setting;
    std::string setting_body;
    std::string pin;
    bool stereo_vision_restricted = false;

    // Unlocked with the PIN until reverted or the next boot (pctl keeps this in memory too).
    bool temporarily_unlocked = false;

    std::string mii_url;
    std::vector<std::uint8_t> mii;
    std::string mii_content_type;

    // The background sync.
    bool synced_this_run = false;
    bool sync_requested = false;
    std::condition_variable_any wake;
    std::jthread sync_thread;

    std::string SecretKey() const {
        return "pctl-" + server_key;
    }

    void Load();
    void SaveLocked();
};

Pairing::Pairing(std::string server_key) : impl{std::make_shared<Impl>()} {
    impl->server_key = std::move(server_key);
    impl->Load();
}

Pairing& Pairing::Current() {
    auto& globals = GetGlobals();
    // With no OpenPak server configured the console's own settings still persist, under "local".
    std::string key = OpenPak::Session::detail::ServerKey();
    if (key.empty()) {
        key = LocalKey;
    }
    Pairing* pairing;
    bool start_sync;
    {
        std::lock_guard lock{globals.mutex};
        auto& slot = globals.pairings[key];
        const bool created = slot == nullptr;
        if (created) {
            slot.reset(new Pairing(key));
        }
        pairing = slot.get();
        start_sync = created;
    }
    if (start_sync) {
        pairing->SyncSoon();
    }
    return *pairing;
}

void Pairing::SetWorker(std::weak_ptr<OpenPak::AsyncWorker> worker) {
    auto& globals = GetGlobals();
    std::lock_guard lock{globals.mutex};
    globals.worker = std::move(worker);
}

int Pairing::AddListener(std::function<void()> listener) {
    auto& globals = GetGlobals();
    std::lock_guard lock{globals.mutex};
    const int id = globals.next_listener++;
    globals.listeners.emplace(id, std::move(listener));
    return id;
}

void Pairing::RemoveListener(int id) {
    auto& globals = GetGlobals();
    std::lock_guard lock{globals.mutex};
    globals.listeners.erase(id);
}

void Pairing::Impl::Load() {
    const auto secret = OpenPak::LoadSecret(SecretKey());
    if (!secret || secret->empty()) {
        LOG_INFO(Service_PCTL, "pctl: no saved record ({})", secret ? "empty" : "none");
        return;
    }
    LOG_INFO(Service_PCTL, "pctl: record read ({} bytes)", secret->size());
    const json root = json::parse(DecodeRecord(*secret), nullptr, false);
    try {
        if (root.is_discarded() || !root.is_object()) {
            throw std::runtime_error("not an object");
        }
        std::uint64_t device = 0;
        if (!root.contains("device") ||
            std::sscanf(root.at("device").get<std::string>().c_str(), "%llx",
                        reinterpret_cast<unsigned long long*>(&device)) != 1) {
            throw std::runtime_error("no device");
        }
        paired_device = device;
        active = root.at("active").get<bool>();
        if (!active) {
            paired_device = 0;
        }
        pin = root.value("pin", std::string{});
        stereo_vision_restricted = root.value("stereoVisionRestricted", false);
        // The state this build wrote (the parent's setting with the console's own changes), else
        // the parent's document as an earlier build kept it.
        setting_body = root.value("setting", std::string{});
        if (const auto state = root.value("state", std::string{}); !state.empty()) {
            Setting loaded;
            if (LoadState(state, loaded)) {
                setting = std::move(loaded);
            }
        } else if (!setting_body.empty()) {
            Setting parsed;
            if (ParseSetting(setting_body, parsed) == Moon::Success) {
                setting = std::move(parsed);
            }
        }
        setting_etag = root.value("settingEtag", std::string{});
        settings_last_updated = root.value("settingsLastUpdated", std::int64_t{0});

        // The account getters read the found record, so a paired console starts with its owners.
        found_device = active ? paired_device : 0;
        for (const auto& owner : root.value("owners", json::array())) {
            Moon::Owner o;
            o.device_id = paired_device;
            std::sscanf(owner.at("nintendoAccountId").get<std::string>().c_str(), "%llx",
                        reinterpret_cast<unsigned long long*>(&o.nintendo_account_id));
            o.nickname = owner.at("nickname").get<std::string>();
            o.mii_url = owner.value("miiUrl", std::string{});
            found_owners.push_back(std::move(o));
        }
    } catch (const std::exception&) {
        LOG_WARNING(Service_PCTL, "pctl: unreadable pairing record; starting unpaired");
        paired_device = 0;
        active = false;
        found_device = 0;
        found_owners.clear();
        setting.reset();
        setting_body.clear();
        setting_etag.clear();
        settings_last_updated = 0;
        pin.clear();
        stereo_vision_restricted = false;
    }
}

namespace {

// The record goes to the password store as base64 under a "pctl1:" prefix: libsecret with
// GNOME Keyring was seen answering an empty secret for a JSON text full of \" escapes (the
// setting document is kept inside the record as a string), and persistently so across restarts.
constexpr std::string_view RecordPrefix = "pctl1:";
constexpr std::string_view Base64Chars =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string EncodeRecord(const std::string& json_text) {
    std::string out{RecordPrefix};
    std::size_t i = 0;
    for (; i + 2 < json_text.size(); i += 3) {
        const unsigned n = (static_cast<unsigned char>(json_text[i]) << 16) |
                           (static_cast<unsigned char>(json_text[i + 1]) << 8) |
                           static_cast<unsigned char>(json_text[i + 2]);
        out += {Base64Chars[(n >> 18) & 63], Base64Chars[(n >> 12) & 63],
                Base64Chars[(n >> 6) & 63], Base64Chars[n & 63]};
    }
    if (const std::size_t rest = json_text.size() - i; rest != 0) {
        unsigned n = static_cast<unsigned char>(json_text[i]) << 16;
        if (rest == 2) {
            n |= static_cast<unsigned char>(json_text[i + 1]) << 8;
        }
        out += Base64Chars[(n >> 18) & 63];
        out += Base64Chars[(n >> 12) & 63];
        out += rest == 2 ? Base64Chars[(n >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

/// The JSON text of a stored record: decoded when it carries the prefix, as is otherwise (an
/// earlier build kept plain JSON). Empty when the base64 is bad.
std::string DecodeRecord(const std::string& stored) {
    if (stored.compare(0, RecordPrefix.size(), RecordPrefix) != 0) {
        return stored;
    }
    std::string out;
    unsigned bits = 0;
    int count = 0;
    for (std::size_t i = RecordPrefix.size(); i < stored.size() && stored[i] != '='; ++i) {
        const auto position = Base64Chars.find(stored[i]);
        if (position == std::string_view::npos) {
            return {};
        }
        bits = (bits << 6) | static_cast<unsigned>(position);
        count += 6;
        if (count >= 8) {
            count -= 8;
            out += static_cast<char>((bits >> count) & 0xff);
        }
    }
    return out;
}

/// Keep `text` and read it back. The system password store has answered an empty secret for one
/// it just accepted (seen with libsecret, in a store/lookup loop: roughly one write in five), which
/// would drop the PIN at the next start. A few more writes make it stick.
bool StoreVerified(const std::string& key, const std::string& text) {
    for (int attempt = 1; attempt <= 3; ++attempt) {
        if (!OpenPak::StoreSecret(key, text)) {
            return false;
        }
        const auto back = OpenPak::LoadSecret(key);
        if (back && *back == text) {
            return true;
        }
        LOG_WARNING(Service_PCTL,
                    "pctl: the password store did not return the record it kept (attempt {})",
                    attempt);
    }
    return false;
}

} // namespace

void Pairing::Impl::SaveLocked() {
    if (!active && pin.empty() && !setting) {
        OpenPak::EraseSecret(SecretKey());
        return;
    }
    nlohmann::ordered_json out;
    out["device"] = fmt::format("{:016x}", paired_device);
    out["active"] = active;
    if (!setting_etag.empty()) {
        out["settingEtag"] = setting_etag;
    }
    out["settingsLastUpdated"] = settings_last_updated;
    // The console keeps its PIN and setting in pctl's savedata; the secret store stands in for it.
    if (!pin.empty()) {
        out["pin"] = pin;
    }
    if (!setting_body.empty()) {
        out["setting"] = setting_body;
    }
    if (setting) {
        out["state"] = SerializeState(*setting);
    }
    out["stereoVisionRestricted"] = stereo_vision_restricted;
    auto owners = nlohmann::ordered_json::array();
    if (found_device == paired_device) {
        for (const auto& owner : found_owners) {
            nlohmann::ordered_json o;
            o["nintendoAccountId"] = fmt::format("{:016x}", owner.nintendo_account_id);
            o["nickname"] = owner.nickname;
            if (!owner.mii_url.empty()) {
                o["miiUrl"] = owner.mii_url;
            }
            owners.push_back(std::move(o));
        }
    }
    out["owners"] = std::move(owners);
    const std::string text = EncodeRecord(out.dump());
    if (!StoreVerified(SecretKey(), text)) {
        LOG_WARNING(Service_PCTL, "pctl: the secret store did not keep the pairing record");
    } else {
        LOG_INFO(Service_PCTL, "pctl: record kept ({} bytes)", text.size());
    }
}

namespace {

/// One moon request with the console's headers, through OpenPak's pinned client.
Reply Request(const char* method, const std::string& host, const std::string& path,
              const std::string& token, const std::string& body, const std::string& if_none_match) {
    OpenPak::Session::Headers headers;
    for (auto& header : Moon::RequestHeaders(token, if_none_match)) {
        headers.push_back(std::move(header));
    }
    auto response = OpenPak::Session::Send(method, host, path, headers,
                                           body.empty() ? std::string{} : "application/json", body);
    return {response.status, std::move(response.body), std::move(response.etag),
            std::move(response.content_type)};
}

Reply Call(const char* method, const std::string& path, const std::string& token,
           const std::string& body = {}, const std::string& if_none_match = {}) {
    return Request(method, std::string{Moon::Host}, path, token, body, if_none_match);
}

/// A refusal: the identity from the body; no answer at all is 2142-2003 (how pctl reports a
/// connection that never answered is not traced).
Moon::Description Refused(const char* what, const Reply& reply) {
    if (reply.status == 0) {
        LOG_UNKNOWN_CONTRACT(Service_PCTL,
                             "pctl moon {} could not reach the server; answered 2142-2003", what);
        return Unreachable;
    }
    const Moon::Description result = Moon::MapError(reply.body);
    LOG_WARNING(Service_PCTL, "pctl: moon {} answered {}: 2142-{:04}", what, reply.status, result);
    return result;
}

} // namespace

/// The dauth device token for pctl's client id, kept an hour (dauth answers a day; an hour keeps a
/// token from outliving a server restart by much). Empty when OpenPak is off or dauth refuses.
static std::string Connect(Pairing::Impl& impl) {
    {
        std::lock_guard lock{impl.mutex};
        if (!impl.token.empty() && std::chrono::steady_clock::now() < impl.token_expiry) {
            return impl.token;
        }
    }
    if (!OpenPak::Session::Enabled()) {
        LOG_WARNING(Service_PCTL,
                    "pctl: no OpenPak server is configured, so the parent app cannot be reached");
        return {};
    }
    std::string token = OpenPak::Session::DauthToken(std::string{Moon::DauthClientId}, {});
    if (token.empty()) {
        LOG_WARNING(Service_PCTL, "pctl: no dauth device token for pctl's client");
        return {};
    }
    std::lock_guard lock{impl.mutex};
    impl.token = token;
    impl.token_expiry = std::chrono::steady_clock::now() + std::chrono::hours{1};
    return token;
}

namespace {

Moon::Description ReadOwners(Pairing::Impl& impl, const std::string& token, std::uint64_t device) {
    const Reply reply = Call("GET", Moon::DevicePath(device, "/owners"), token);
    if (reply.status != 200) {
        return Refused("owners", reply);
    }
    std::vector<Moon::Owner> owners;
    const auto result = Moon::ParseOwners(reply.body, owners);
    if (result != Moon::Success) {
        return result;
    }
    const auto count = owners.size();
    {
        std::lock_guard lock{impl.mutex};
        impl.found_device = device;
        impl.found_owners = std::move(owners);
        if (impl.active && device == impl.paired_device) {
            impl.SaveLocked();
        }
    }
    LOG_INFO(Service_PCTL, "pctl: the device has {} owner(s)", count);
    return Moon::Success;
}

} // namespace

Pairing::Restrictions Pairing::InForce() const {
    std::lock_guard lock{impl->mutex};
    Restrictions out;
    out.pin = impl->pin;
    out.temporarily_unlocked = impl->temporarily_unlocked;
    out.stereo_vision_restricted = impl->stereo_vision_restricted;
    if (const auto& s = impl->setting) {
        out.safety_level = s->safety_level;
        out.custom = {s->age_restricted ? s->custom_rating_age : std::uint8_t{0},
                      s->custom_sns_post_restricted, s->custom_free_communication_restricted};
        out.rating_organization = s->rating_organization;
        out.free_communication_applications = s->free_communication_applications;
        out.exempt_applications = s->exempt_applications;
        out.play_timer = s->play_timer;
    }
    return out;
}

bool Pairing::IsPairingActive() const {
    std::lock_guard lock{impl->mutex};
    return impl->active;
}

std::uint64_t Pairing::PairedDevice() const {
    std::lock_guard lock{impl->mutex};
    return impl->active ? impl->paired_device : 0;
}

std::int64_t Pairing::SettingsLastUpdated() const {
    std::lock_guard lock{impl->mutex};
    return impl->settings_last_updated;
}

std::uint64_t Pairing::FoundDevice() const {
    std::lock_guard lock{impl->mutex};
    return impl->found_device;
}

std::vector<Moon::Owner> Pairing::Owners() const {
    std::lock_guard lock{impl->mutex};
    return impl->found_owners;
}

std::optional<Moon::Owner> Pairing::FirstOwner() const {
    std::lock_guard lock{impl->mutex};
    if (impl->found_owners.empty()) {
        return std::nullopt;
    }
    return impl->found_owners.front();
}

std::pair<std::vector<std::uint8_t>, std::string> Pairing::Mii() const {
    std::lock_guard lock{impl->mutex};
    return {impl->mii, impl->mii_content_type};
}

void Pairing::SetPin(std::string pin) {
    {
        std::lock_guard lock{impl->mutex};
        impl->pin = std::move(pin);
        if (impl->pin.empty()) {
            impl->temporarily_unlocked = false;
        }
        impl->SaveLocked();
    }
    NotifyChanged();
}

void Pairing::SetTemporarilyUnlocked(bool unlocked) {
    {
        std::lock_guard lock{impl->mutex};
        impl->temporarily_unlocked = unlocked;
    }
    NotifyChanged();
}

void Pairing::SetStereoVisionRestricted(bool restricted) {
    std::lock_guard lock{impl->mutex};
    if (impl->pin.empty()) {
        return;
    }
    impl->stereo_vision_restricted = restricted;
    impl->SaveLocked();
}

namespace {

/// Change the setting in force the way a console setter does: under the lock, kept, and the
/// listeners told. The raw document of the parent's last sync is dropped so the next sync is seen
/// as a change and brings the parent's setting back.
template <typename Change>
void ChangeSetting(Pairing::Impl& impl, Change&& change) {
    {
        std::lock_guard lock{impl.mutex};
        if (!impl.setting) {
            impl.setting.emplace();
        }
        change(*impl.setting);
        impl.setting_body.clear();
        impl.SaveLocked();
    }
    NotifyChanged();
}

} // namespace

void Pairing::SetSafetyLevel(std::uint8_t level, int default_organization) {
    ChangeSetting(*impl, [&](Setting& setting) {
        // The organisation is fixed to the default at the first level set (.text 0x41880).
        if (setting.rating_organization < 0) {
            setting.rating_organization = default_organization;
        }
        setting.safety_level = level;
    });
}

void Pairing::SetCustomSettings(const Rules::Restriction& custom) {
    ChangeSetting(*impl, [&](Setting& setting) {
        setting.age_restricted = custom.rating_age != 0;
        setting.custom_rating_age = custom.rating_age;
        setting.custom_sns_post_restricted = custom.sns_post_restricted;
        setting.custom_free_communication_restricted = custom.free_communication_restricted;
    });
}

void Pairing::SetDefaultRatingOrganization(int organization) {
    ChangeSetting(*impl, [&](Setting& setting) { setting.rating_organization = organization; });
}

bool Pairing::AddFreeCommunicationApplication(std::uint64_t application_id, bool restricted) {
    bool added = false;
    ChangeSetting(*impl, [&](Setting& setting) {
        added = UpsertFreeCommunication(setting.free_communication_applications, application_id,
                                        restricted);
    });
    return added;
}

void Pairing::UpdateFreeCommunicationApplications(
    const std::vector<std::pair<std::uint64_t, bool>>& entries) {
    ChangeSetting(*impl, [&](Setting& setting) {
        for (const auto& [id, restricted] : entries) {
            UpsertFreeCommunication(setting.free_communication_applications, id, restricted);
        }
    });
}

void Pairing::DeleteSettings() {
    ForgetPairing();
}

Moon::Description Pairing::RequestPairing(const std::string& code) {
    const std::string token = Connect(*impl);
    if (token.empty()) {
        return Unreachable;
    }
    const Reply search = Call("GET", Moon::SearchPath(code), token);
    if (search.status != 200) {
        return Refused("search", search);
    }
    std::uint64_t device = 0;
    const auto result = Moon::ParseSearch(search.body, device);
    if (result != Moon::Success) {
        LOG_INFO(Service_PCTL, "pctl: the pairing code found no device (2142-{:04})", result);
        return result;
    }
    return ReadOwners(*impl, token, device);
}

Moon::Description Pairing::Authorize(std::uint64_t device_id, const DeviceFacts& facts) {
    const std::string token = Connect(*impl);
    if (token.empty()) {
        return Unreachable;
    }
    // The owner the guest passes (2017) is not on the wire: the hardware activate body has no
    // owner key.
    const std::string body = Moon::ActivateBody(
        facts.language, facts.region, Moon::SerialFromToken(token).value_or(""),
        facts.displayed_version, facts.internal_version, Now(), OpenPak::Push::PenneId());
    const Reply reply = Call("POST", Moon::DevicePath(device_id, "/activate"), token, body);
    if (reply.status != 200) {
        return Refused("activate", reply);
    }
    std::string etag;
    std::int64_t synchronized_at = 0;
    if (const auto result = Moon::ParseActivate(reply.body, etag, synchronized_at);
        result != Moon::Success) {
        return result;
    }
    {
        std::lock_guard lock{impl->mutex};
        impl->paired_device = device_id;
        impl->active = true;
        // Not the activate answer's etag: the first sync then fetches the setting and sends the
        // receipt, which is how the parent side learns the console holds it.
        impl->setting_etag.clear();
        impl->settings_last_updated = synchronized_at;
        impl->SaveLocked();
    }
    LOG_INFO(Service_PCTL, "pctl: paired with the parent app (activate etag {})",
             etag.empty() ? "none" : "given");
    SyncSoon();
    return Moon::Success;
}

Moon::Description Pairing::Retrieve() {
    const std::string token = Connect(*impl);
    if (token.empty()) {
        return Unreachable;
    }
    return ReadOwners(*impl, token, PairedDevice());
}

Moon::Description Pairing::Unlink(bool force) {
    Moon::Description result = Unreachable;
    if (const std::string token = Connect(*impl); !token.empty()) {
        const std::uint64_t device = PairedDevice();
        const Reply reply = Call("GET", Moon::DevicePath(device, "/owners"), token);
        std::vector<Moon::Owner> owners;
        result = reply.status == 200 ? Moon::ParseOwners(reply.body, owners)
                                     : Refused("owners", reply);
        if (result == Moon::Success) {
            // How a console unlinks on hardware (v0.43.0): list the owners and DELETE each.
            for (const auto& owner : owners) {
                const Reply deleted = Call(
                    "DELETE",
                    Moon::DevicePath(device, fmt::format("/owners/{:016x}", owner.nintendo_account_id)),
                    token);
                if (deleted.status < 200 || deleted.status > 299) {
                    result = Refused("owner delete", deleted);
                    break;
                }
            }
        }
    }
    // With `force` a refused unlink still unlinks here, as pctl answers success (.text 0x30138).
    if (result == Moon::Success || force) {
        ForgetPairing();
        LOG_INFO(Service_PCTL, "pctl: unlinked from the parent app{}",
                 result == Moon::Success ? "" : " here only; the server refused");
        return Moon::Success;
    }
    return result;
}

Moon::Description Pairing::Synchronize(bool* changed_out) {
    if (changed_out != nullptr) {
        *changed_out = false;
    }
    const std::string token = Connect(*impl);
    if (token.empty()) {
        return Unreachable;
    }
    const std::uint64_t device = PairedDevice();
    std::string known;
    {
        std::lock_guard lock{impl->mutex};
        known = impl->setting_etag;
    }

    const Reply reply = Call("GET", Moon::DevicePath(device, "/parental_control_setting"), token,
                             {}, known);
    if (reply.status == 304) {
        return Moon::Success;
    }
    if (reply.status != 200) {
        const auto refused = Refused("setting", reply);
        // The server no longer knows the device (pctl 0x4d960 on 2142-2009/2015): the parent
        // removed it. Unlinked here too, and the overlay says so (4002, source 1).
        if (refused == 2009 || refused == 2015) {
            ForgetPairing();
            SendUnlinked(1);
        }
        return refused;
    }

    Setting setting;
    if (const auto result = ParseSetting(reply.body, setting); result != Moon::Success) {
        return result;
    }
    const std::string etag = !setting.etag.empty() ? setting.etag : reply.etag;
    const std::int64_t updated_at = setting.updated_at;

    bool changed;
    std::uint32_t mask;
    std::string pin;
    {
        std::lock_guard lock{impl->mutex};
        pin = setting.unlock_code.value_or(impl->pin);
        const bool stereo = setting.stereo_vision_restricted.value_or(impl->stereo_vision_restricted);
        changed = reply.body != impl->setting_body || pin != impl->pin ||
                  stereo != impl->stereo_vision_restricted;
        mask = ChangedMask(impl->setting ? &*impl->setting : nullptr, impl->pin,
                           impl->stereo_vision_restricted, &setting, pin, stereo);
        if (changed) {
            LOG_INFO(Service_PCTL,
                     "pctl: the parent's setting is in force: level {}, PIN {}, {} "
                     "free-communication and {} exempt title(s), play timer {}",
                     SafetyLevelName(setting.safety_level), pin.empty() ? "none" : "set",
                     setting.free_communication_applications.size(),
                     setting.exempt_applications.size(), setting.play_timer[0] != 0 ? "on" : "off");
        }
        impl->setting = std::move(setting);
        impl->setting_body = reply.body;
        impl->pin = pin;
        impl->stereo_vision_restricted = stereo;
        impl->SaveLocked();
    }

    // The receipt names the PIN now in force, as the console's does (hardware, v0.43.1).
    const std::int64_t now = Now();
    const Reply receipt = Call("POST", Moon::DevicePath(device), token,
                               Moon::ReceiptBody(etag, now, pin, OpenPak::Push::PenneId()));
    if (receipt.status < 200 || receipt.status > 299) {
        return Refused("receipt", receipt);
    }

    if (changed) {
        NotifyChanged();
        // pctl's 4000 after a sync that brought a new setting.
        SendSettingsChanged(mask);
    }
    {
        std::lock_guard lock{impl->mutex};
        impl->setting_etag = etag;
        // Unknown contract (as in the Ryujinx Harbor): which time GetSettingsLastUpdated reports;
        // the document's updatedAt, else the time of this sync.
        impl->settings_last_updated = updated_at > 0 ? updated_at : now;
        impl->SaveLocked();
    }
    if (changed_out != nullptr) {
        *changed_out = changed;
    }
    return Moon::Success;
}

Moon::Description Pairing::FetchMii() {
    const auto owner = FirstOwner();
    const std::string url = owner ? owner->mii_url : std::string{};
    if (url.empty()) {
        LOG_UNKNOWN_CONTRACT(Service_PCTL,
                             "pctl: the owner has no picture URL; answered an empty picture");
        return Moon::Success;
    }
    {
        std::lock_guard lock{impl->mutex};
        if (impl->mii_url == url && !impl->mii.empty()) {
            return Moon::Success;
        }
    }
    constexpr std::string_view scheme = "https://";
    if (url.substr(0, scheme.size()) != scheme) {
        LOG_WARNING(Service_PCTL, "pctl: the owner's picture is not an https URL");
        return Moon::Success;
    }
    const std::string rest = url.substr(scheme.size());
    const auto slash = rest.find('/');
    const std::string host = rest.substr(0, slash);
    const std::string path = slash == std::string::npos ? "/" : rest.substr(slash);

    // The moon host authenticates every call; nothing else gets the device token.
    std::string token;
    if (host == Moon::Host) {
        token = Connect(*impl);
        if (token.empty()) {
            return Unreachable;
        }
    }
    const Reply reply = Request("GET", host, path, token, {}, {});
    if (reply.status != 200) {
        return Refused("picture", reply);
    }
    std::lock_guard lock{impl->mutex};
    impl->mii_url = url;
    impl->mii.assign(reply.body.begin(), reply.body.end());
    impl->mii_content_type = reply.content_type.substr(0, reply.content_type.find(';'));
    return Moon::Success;
}

void Pairing::ForgetPairing() {
    {
        std::lock_guard lock{impl->mutex};
        impl->paired_device = 0;
        impl->active = false;
        impl->setting_etag.clear();
        impl->settings_last_updated = 0;
        impl->found_device = 0;
        impl->found_owners.clear();
        // Unpaired, the console answers as it did before pairing: no setting, no PIN.
        impl->setting.reset();
        impl->setting_body.clear();
        impl->pin.clear();
        impl->stereo_vision_restricted = false;
        impl->temporarily_unlocked = false;
        impl->SaveLocked();
    }
    NotifyChanged();
}

void Pairing::SyncSoon() {
    std::lock_guard lock{impl->mutex};
    if (!impl->active || impl->server_key == LocalKey) {
        return;
    }
    impl->sync_requested = true;
    if (!impl->sync_thread.joinable()) {
        // pctl fetches the setting on its own (RetrieveSettings in an IntermittentContext); the HLE
        // has no sysmodule, so a paired console syncs 5 s after it starts, then every 10 minutes,
        // and at once on the parent's push.
        impl->sync_thread = std::jthread([this](std::stop_token stop) {
            std::unique_lock held{impl->mutex};
            auto wait = std::chrono::seconds{5};
            while (!stop.stop_requested()) {
                impl->wake.wait_for(held, stop, wait, [this] { return impl->sync_requested; });
                if (stop.stop_requested()) {
                    return;
                }
                impl->sync_requested = false;
                wait = std::chrono::minutes{10};
                if (!impl->active) {
                    continue;
                }
                // The first sync of a run fetches the setting whatever etag is kept, so the receipt
                // goes out at least once per boot.
                if (!impl->synced_this_run) {
                    impl->setting_etag.clear();
                    impl->synced_this_run = true;
                }
                held.unlock();
                bool changed = false;
                const auto result = Synchronize(&changed);
                if (result == Moon::Success) {
                    LOG_INFO(Service_PCTL, "pctl: background sync: {}",
                             changed ? "new setting received" : "up to date");
                } else {
                    LOG_INFO(Service_PCTL, "pctl: background sync: 2142-{:04}", result);
                }
                held.lock();
            }
        });
    }
    impl->wake.notify_all();
}

void Pairing::OnUnlinkPush(const std::string& body) {
    const json document = json::parse(body, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        return;
    }
    const auto id = document.find("deviceId");
    if (id == document.end() || !id->is_string()) {
        return;
    }
    unsigned long long device = 0;
    if (std::sscanf(id->get_ref<const std::string&>().c_str(), "%llx", &device) != 1 ||
        device == 0 || device != PairedDevice()) {
        return;
    }
    ForgetPairing();
    SendUnlinked(0);
}

} // namespace Service::PCTL

// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later
// Derived from openpak-client 3fd9ba7 (push.cpp).

// The Penne push connection. Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base):
// OpenPakSession.Push.cs and PenneFrames.cs.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <vector>
#include <algorithm>
#include <mutex>
#include <thread>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "common/logging.h"
#include "core/openpak/baas.h"
#include "core/openpak/platform.h"
#include "core/openpak/push.h"
#include "core/openpak/secret_store.h"
#include "core/openpak/session.h"
#include "core/openpak/session_detail.h"

namespace OpenPak::Push {

namespace {

constexpr const char* GodHost = "god.penne.srv.nintendo.net";
constexpr const char* ValHost = "val.penne.srv.nintendo.net";

// ---- FlatBuffers, read side: only as much as the envelope and a kind-1 message need ----

using Bytes = std::span<const std::uint8_t>;

std::uint32_t U32(Bytes b, std::size_t at) {
    return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8) |
           (static_cast<std::uint32_t>(b[at + 2]) << 16) |
           (static_cast<std::uint32_t>(b[at + 3]) << 24);
}

std::uint16_t U16(Bytes b, std::size_t at) {
    return static_cast<std::uint16_t>(b[at] | (b[at + 1] << 8));
}

std::optional<std::size_t> Checked(Bytes b, std::int64_t position, std::int64_t size) {
    if (position < 0 || size < 0 || position + size > static_cast<std::int64_t>(b.size())) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(position);
}

std::optional<std::size_t> Root(Bytes b) {
    if (b.size() < 4) {
        return std::nullopt;
    }
    return Checked(b, U32(b, 0), 4);
}

/// Where field `slot` of the table at `table` lives.
std::optional<std::size_t> Field(Bytes b, std::size_t table, int slot) {
    const std::int64_t vtable =
        static_cast<std::int64_t>(table) - static_cast<std::int32_t>(U32(b, table));
    const auto start = Checked(b, vtable, 4);
    if (!start) {
        return std::nullopt;
    }
    const int vtable_size = U16(b, *start);
    const int entry = 4 + 2 * slot;
    if (entry + 2 > vtable_size) {
        return std::nullopt;
    }
    const auto at = Checked(b, static_cast<std::int64_t>(*start) + entry, 2);
    if (!at) {
        return std::nullopt;
    }
    const int offset = U16(b, *at);
    if (offset == 0) {
        return std::nullopt;
    }
    return Checked(b, static_cast<std::int64_t>(table) + offset, 1);
}

std::optional<std::uint8_t> U8(Bytes b, std::size_t table, int slot) {
    if (const auto at = Field(b, table, slot)) {
        return b[*at];
    }
    return std::nullopt;
}

std::optional<std::size_t> Indirect(Bytes b, std::size_t at) {
    const auto position = Checked(b, static_cast<std::int64_t>(at), 4);
    if (!position) {
        return std::nullopt;
    }
    return Checked(b, static_cast<std::int64_t>(*position) + U32(b, *position), 4);
}

std::optional<std::size_t> Table(Bytes b, std::size_t table, int slot) {
    const auto at = Field(b, table, slot);
    return at ? Indirect(b, *at) : std::nullopt;
}

std::optional<std::string> String(Bytes b, std::size_t table, int slot) {
    const auto at = Field(b, table, slot);
    const auto position = at ? Indirect(b, *at) : std::nullopt;
    if (!position) {
        return std::nullopt;
    }
    const std::uint32_t length = U32(b, *position);
    const auto start = Checked(b, static_cast<std::int64_t>(*position) + 4, length);
    if (!start) {
        return std::nullopt;
    }
    return std::string(reinterpret_cast<const char*>(b.data() + *start), length);
}

std::optional<std::size_t> VectorTable(Bytes b, std::size_t table, int slot, std::uint32_t index) {
    const auto at = Field(b, table, slot);
    const auto vector = at ? Indirect(b, *at) : std::nullopt;
    if (!vector || index >= U32(b, *vector)) {
        return std::nullopt;
    }
    return Indirect(b, *vector + 4 + 4 * static_cast<std::size_t>(index));
}

// ---- the connection ----

struct Registration {
    std::string id;
    std::string password;
};

struct State {
    std::mutex mutex;
    std::thread thread;
    /// OPENPAK_SIMULATE_PUSH_DIR's reader, for test runs that hold no connection.
    std::thread simulated;
    std::atomic<bool> running{false};
    std::atomic<bool> connected{false};

    /// Set by Drop from another thread: the connection thread forgets the registration and
    /// ticket before its next attempt, since only it touches them.
    std::atomic<bool> forget{false};
    std::condition_variable wake;

    /// The client of the open frontline request, so Drop and Stop can cut it off mid-read.
    httplib::SSLClient* live = nullptr;

    std::optional<Registration> registration;
    std::string ticket;
    std::string frontline;
    std::int64_t ticket_expiry = 0;

    /// Background refreshes queued and not yet run, by kind: a burst of deliveries for one list
    /// costs one fetch, never one each.
    std::map<std::string, bool> queued;

    /// The consumer of every delivery, and the registration id it may ask for. Guarded by mutex.
    std::map<std::string, Hook> hooks;
    std::string penne_id;

    ~State() {
        running = false;
        wake.notify_all();
        {
            std::lock_guard lock{mutex};
            if (live != nullptr) {
                live->stop();
            }
        }
        if (thread.joinable()) {
            thread.join();
        }
        if (simulated.joinable()) {
            simulated.join();
        }
    }
};

State& Get() {
    static State state;
    return state;
}

std::int64_t Now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// The registration is a credential (id and password): it lives in the secret store, per server
/// and per user.
std::string RegistrationKey(const std::string& server_key, const std::string& uid) {
    return fmt::format("penne-{}-{}", server_key, uid);
}

std::optional<Registration> LoadRegistration(const std::string& server_key) {
    try {
        const auto text = LoadSecret(RegistrationKey(server_key, Platform::ProfileId()));
        if (!text || text->empty()) {
            return std::nullopt;
        }
        const auto json = nlohmann::json::parse(*text);
        return Registration{json.at("id").get<std::string>(), json.at("password").get<std::string>()};
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

/// POST god /v1/penne_ids: the registration this user keeps, as a console keeps its own.
std::optional<Registration> Register(const std::string& server_key) {
    const Session::Response reply =
        Session::SendAs("POST", GodHost, "/v1/penne_ids", "application/json", "{}");
    if (reply.status != 200) {
        LOG_DEBUG(WebService, "[OpenPak] Penne registration returned {}", reply.status);
        return std::nullopt;
    }
    try {
        const auto json = nlohmann::json::parse(reply.body);
        Registration registration{json.at("id").get<std::string>(),
                                  json.at("password").get<std::string>()};
        StoreSecret(RegistrationKey(server_key, Platform::ProfileId()),
                    nlohmann::json{{"id", registration.id}, {"password", registration.password}}.dump());
        return registration;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

/// POST val /v1/login_tickets with the registration: the ticket and the frontline to use it on.
bool LoginTicket(State& state, const std::string& server_key) {
    const Registration registration = *state.registration;
    const Session::Response reply = Session::SendAs(
        "POST", ValHost, "/v1/login_tickets", "application/json",
        nlohmann::json{{"id", registration.id}, {"password", registration.password}}.dump());

    if (reply.status == 401 || reply.status == 403) {
        // A registration the server no longer honours is one to replace, not to retry.
        EraseSecret(RegistrationKey(server_key, Platform::ProfileId()));
        state.registration.reset();
        return false;
    }
    if (reply.status != 200) {
        return false;
    }
    try {
        const auto json = nlohmann::json::parse(reply.body);
        state.ticket = json.at("ticket").get<std::string>();
        state.frontline = json.at("frontline_fqdn").get<std::string>();
        state.ticket_expiry = json.value("expires_at", Now() + 24 * 3600);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

/// Run a refresh on the friends module's worker, at most once queued per kind.
void Refresh(const std::string& kind, std::function<void()> work) {
    State& state = Get();
    {
        std::lock_guard lock{state.mutex};
        if (state.queued[kind]) {
            return;
        }
        state.queued[kind] = true;
    }
    Baas::RunInBackground([kind, work = std::move(work)] {
        {
            State& inner = Get();
            std::lock_guard lock{inner.mutex};
            inner.queued[kind] = false;
        }
        work();
    });
}

/// The consumer's hook, outside every lock; one that throws is logged and the stream goes on.
void RaiseHook(const std::string& kind, const std::string& body) {
    std::map<std::string, Hook> hooks;
    {
        State& state = Get();
        std::lock_guard lock{state.mutex};
        hooks = state.hooks;
    }
    for (const auto& [name, hook] : hooks) {
        try {
            hook(kind, body);
        } catch (const std::exception& e) {
            LOG_WARNING(WebService, "[OpenPak] Push hook {} failed: {}", name, e.what());
        }
    }
}

/// A delivery: re-read what it concerns, which is what the console's friends module does on
/// each of these.
void OnMessage(const Message& message) {
    const std::string kind = Kind(message);
    LOG_DEBUG(WebService, "[OpenPak] Pushed: {}", kind);

    // The consumers first: the friends module compares a pushed presence against the cache as it
    // was before the event, so the refresh it causes must not have run yet.
    RaiseHook(kind, message.body);

    if (kind == "presence_updated" || kind == "friend_request_authorized" ||
        kind == "friend_deleted") {
        Refresh("friends", [] { Baas::SyncFriendList(true); });
    }
    if (kind == "friend_request_received" || kind == "friend_request_authorized" ||
        kind == "friend_deleted") {
        Refresh("requests", [] { Baas::RefreshFriendRequests(); });
    }
    if (kind == "friend_invitation_received") {
        Refresh("invitations", [] {
            Session::RefreshInvitations();
            Baas::SyncInvitations();
        });
    }
}

/// One connection, held until it ends. Returns whether it got as far as the handover.
bool Hold(State& state, const std::string& user_id) {
    const std::string server_key = Session::detail::ServerKey();
    if (server_key.empty()) {
        return false;
    }

    if (!state.registration) {
        state.registration = LoadRegistration(server_key);
    }
    if (!state.registration) {
        state.registration = Register(server_key);
        if (!state.registration) {
            return false;
        }
    }
    {
        std::lock_guard lock{state.mutex};
        state.penne_id = state.registration->id;
    }
    if (state.ticket.empty() || Now() > state.ticket_expiry - 3600) {
        if (!LoginTicket(state, server_key)) {
            return false;
        }
    }

    auto client = Session::detail::MakeClientFor(state.frontline);
    // The server writes nothing while there is nothing to say; a quiet connection is cut and
    // re-made after this long, which also bounds how stale a half-dead one can get.
    client->set_read_timeout(300, 0);

    {
        std::lock_guard lock{state.mutex};
        state.live = client.get();
    }

    bool handed_over = false;
    FrameReader reader;

    httplib::Request request;
    request.method = "POST";
    request.path = "/";
    request.headers = {{"Authorization", "Bearer " + state.ticket},
                       {"X-Protocol-Version", "4"},
                       {"X-Power-State", "FullAwake"}};
    request.set_header("Content-Type", "application/octet-stream");
    request.body = "";
    request.content_receiver = [&](const char* data, std::size_t size, std::uint64_t,
                                   std::uint64_t) {
        if (!state.running.load() || Session::UserId({}) != user_id) {
            return false;
        }
        if (!reader.Feed({reinterpret_cast<const std::uint8_t*>(data), size})) {
            return false;
        }
        while (auto frame = reader.Next()) {
            const Bytes bytes{reinterpret_cast<const std::uint8_t*>(frame->data()), frame->size()};
            const auto type = EnvelopeType(bytes);
            if (type == TypeHandoverResult) {
                handed_over = true;
                state.connected = true;
                LOG_INFO(WebService, "[OpenPak] Push connection open");
                // Whatever happened while no connection was up is only in the lists.
                Refresh("friends", [] { Baas::SyncFriendList(true); });
                Refresh("requests", [] { Baas::RefreshFriendRequests(); });
                Refresh("invitations", [] { Session::RefreshInvitations(); });
            } else if (type == TypeReset) {
                return false;
            } else if (const auto message = ReadMessage(bytes)) {
                OnMessage(*message);
            }
        }
        return true;
    };

    const httplib::Result result = client->send(request);

    {
        std::lock_guard lock{state.mutex};
        state.live = nullptr;
    }
    state.connected = false;

    if (result && result->status == 401) {
        // Expired or revoked: a fresh ticket next time.
        state.ticket.clear();
    } else if (!result && !handed_over) {
        LOG_DEBUG(WebService, "[OpenPak] Push connection: {}", httplib::to_string(result.error()));
    }

    return handed_over;
}

} // namespace

bool FrameReader::Feed(std::span<const std::uint8_t> bytes) {
    buffer.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (buffer.size() >= 4) {
        const std::uint32_t length =
            U32({reinterpret_cast<const std::uint8_t*>(buffer.data()), buffer.size()}, 0);
        if (length == 0 || length > MaxFrame) {
            return false;
        }
    }
    return true;
}

std::optional<std::string> FrameReader::Next() {
    if (buffer.size() < 4) {
        return std::nullopt;
    }
    const std::uint32_t length =
        U32({reinterpret_cast<const std::uint8_t*>(buffer.data()), buffer.size()}, 0);
    if (length == 0 || length > MaxFrame || buffer.size() < 4 + std::size_t{length}) {
        return std::nullopt;
    }
    std::string frame = buffer.substr(4, length);
    buffer.erase(0, 4 + std::size_t{length});
    return frame;
}

std::optional<std::uint8_t> EnvelopeType(std::span<const std::uint8_t> frame) {
    const auto root = Root(frame);
    return root ? U8(frame, *root, 0) : std::nullopt;
}

std::optional<Message> ReadMessage(std::span<const std::uint8_t> frame) {
    const auto root = Root(frame);
    if (!root || U8(frame, *root, 0) != TypePutRecord) {
        return std::nullopt;
    }
    const auto payload = Table(frame, *root, 1);
    const auto entry = payload ? VectorTable(frame, *payload, 0, 0) : std::nullopt;
    if (!entry || U8(frame, *entry, 0) != 1) {
        return std::nullopt;
    }
    const auto message = Table(frame, *entry, 1);
    const auto body = message ? String(frame, *message, 4) : std::nullopt;
    if (!body) {
        return std::nullopt;
    }
    return Message{String(frame, *message, 0).value_or(""), *body};
}

std::string Kind(const Message& message) {
    const auto json = nlohmann::json::parse(message.body, nullptr, false);
    if (json.is_object() && json.contains("type") && json["type"].is_string()) {
        return json["type"].get<std::string>();
    }
    return message.name;
}

/// Test runs, which must not hold a real connection, take messages from files instead: with
/// OPENPAK_SIMULATE_PUSH_DIR set, every *.json put in that directory is read, deleted and handed
/// to the handler a frontline message reaches (the file name is the message name, the content
/// its body), as the Ryujinx Harbor's simulated pushes are.
static void StartSimulated(State& state) {
    const char* dir = std::getenv("OPENPAK_SIMULATE_PUSH_DIR");
    if (dir == nullptr || *dir == '\0') {
        return;
    }
    const std::filesystem::path directory{dir};
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    LOG_WARNING(WebService, "[OpenPak] OPENPAK_SIMULATE_PUSH_DIR is set: push messages are read "
                            "from files (test only)");

    state.simulated = std::thread([&state, directory] {
        while (state.running.load()) {
            std::vector<std::filesystem::path> files;
            std::error_code list_error;
            for (const auto& entry : std::filesystem::directory_iterator(directory, list_error)) {
                if (entry.path().extension() == ".json") {
                    files.push_back(entry.path());
                }
            }
            std::sort(files.begin(), files.end());
            for (const auto& file : files) {
                const std::string body = Platform::ReadFile(file);
                std::error_code remove_error;
                std::filesystem::remove(file, remove_error);
                const Message message{file.stem().string(), body};
                LOG_WARNING(WebService, "[OpenPak] Simulated push {}", Kind(message));
                try {
                    OnMessage(message);
                } catch (const std::exception& e) {
                    LOG_WARNING(WebService, "[OpenPak] Simulated push failed: {}", e.what());
                }
            }
            std::unique_lock lock{state.mutex};
            state.wake.wait_for(lock, std::chrono::milliseconds{500},
                                [&state] { return !state.running.load(); });
        }
    });
}

void Start() {
    if (const char* off = std::getenv("OPENPAK_NO_PUSH"); off != nullptr && std::strcmp(off, "1") == 0) {
        return;
    }

    State& state = Get();
    if (state.running.exchange(true)) {
        return;
    }

    StartSimulated(state);

    state.thread = std::thread([&state] {
        using namespace std::chrono_literals;
        std::chrono::seconds backoff = 5s;

        while (state.running.load()) {
            if (state.forget.exchange(false)) {
                {
                    std::lock_guard lock{state.mutex};
                    state.penne_id.clear();
                }
                state.registration.reset();
                state.ticket.clear();
                state.frontline.clear();
            }

            // Only a linked user has anything to be pushed: an unlinked device account's
            // registration and tickets are refused (401), so it is never asked for.
            const std::string user_id = Session::Linked({}) ? Session::UserId({}) : std::string{};
            bool held = false;

            if (!user_id.empty()) {
                try {
                    const auto opened = std::chrono::steady_clock::now();
                    held = Hold(state, user_id);
                    // A connection that lived ended normally (an idle cut, a restart): come back
                    // quickly. One that failed at once backs off.
                    held = held && std::chrono::steady_clock::now() - opened > 1min;
                } catch (const std::exception& e) {
                    LOG_DEBUG(WebService, "[OpenPak] Push connection: {}", e.what());
                }
            }

            backoff = user_id.empty() ? 5s : held ? 2s : (std::min)(backoff * 2, std::chrono::seconds{300});

            std::unique_lock lock{state.mutex};
            state.wake.wait_for(lock, backoff, [&state] { return !state.running.load(); });
        }
    });
}

void Stop() {
    State& state = Get();
    if (!state.running.exchange(false)) {
        return;
    }
    state.wake.notify_all();
    {
        std::lock_guard lock{state.mutex};
        if (state.live != nullptr) {
            state.live->stop();
        }
    }
    if (state.thread.joinable()) {
        state.thread.join();
    }
    if (state.simulated.joinable()) {
        state.simulated.join();
    }
}

void Drop() {
    State& state = Get();
    state.forget = true;
    std::lock_guard lock{state.mutex};
    if (state.live != nullptr) {
        state.live->stop();
    }
}

bool Connected() {
    return Get().connected.load();
}

void ForgetProfile(const std::string& uid) {
    const std::string server_key = Session::detail::ServerKey();
    if (!server_key.empty() && !uid.empty()) {
        EraseSecret(RegistrationKey(server_key, uid));
    }
}

void SetHook(std::string name, Hook hook) {
    State& state = Get();
    std::lock_guard lock{state.mutex};
    if (hook) {
        state.hooks[std::move(name)] = std::move(hook);
    } else {
        state.hooks.erase(name);
    }
}

std::string PenneId() {
    State& state = Get();
    std::lock_guard lock{state.mutex};
    return state.penne_id;
}

} // namespace OpenPak::Push

// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later
// Derived from openpak-client 3fd9ba7 (presence_state.cpp).

#include "core/openpak/presence_state.h"

#include <chrono>

#include <fmt/format.h>

namespace OpenPak::Friends {

PresenceState::PresenceState(Clock clock) : clock_(std::move(clock)) {}

PresenceState& PresenceState::Instance() {
    static PresenceState state;
    return state;
}

std::int64_t PresenceState::Now() const {
    if (clock_) {
        return clock_();
    }
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void PresenceState::SetRunningApplication(std::uint64_t application_id,
                                          std::uint64_t presence_group_id) {
    std::lock_guard lock(mutex_);
    application_id_ = application_id;
    presence_group_id_ = presence_group_id;
}

std::pair<std::uint64_t, std::uint64_t> PresenceState::Application() const {
    std::lock_guard lock(mutex_);
    return {application_id_, presence_group_id_ != 0 ? presence_group_id_ : application_id_};
}

DeclaredPresence PresenceState::ForLocked(const std::string& profile_id,
                                          std::uint64_t title_id) const {
    const auto found = states_.find(profile_id);
    if (found != states_.end() && found->second.title_id == title_id) {
        return found->second;
    }
    DeclaredPresence fresh;
    fresh.title_id = title_id;
    return fresh;
}

DeclaredPresence PresenceState::For(const std::string& profile_id, std::uint64_t title_id) const {
    std::lock_guard lock(mutex_);
    return ForLocked(profile_id, title_id);
}

DeclaredPresence PresenceState::For(const std::string& profile_id) const {
    std::lock_guard lock(mutex_);
    return ForLocked(profile_id, application_id_);
}

bool PresenceState::Update(const std::string& profile_id, std::uint64_t title_id,
                           std::uint8_t declaration, std::optional<std::string> app_field) {
    bool changed = false;
    {
        std::lock_guard lock(mutex_);
        const DeclaredPresence previous = ForLocked(profile_id, title_id);

        DeclaredPresence next = previous;
        next.title_id = title_id;
        if (declaration == DeclarationOpen) {
            next.session_open = true;
        } else if (declaration == DeclarationClose) {
            next.session_open = false;
        }
        if (app_field) {
            next.app_field = std::move(*app_field);
        }

        changed = next.session_open != previous.session_open ||
                  next.app_field != previous.app_field || next.title_id != previous.title_id;
        if (changed) {
            next.updated_at = Now();
        }
        states_[profile_id] = std::move(next);
    }

    if (changed) {
        std::function<void()> callback;
        {
            std::lock_guard lock(callback_mutex_);
            callback = changed_;
        }
        if (callback) {
            callback();
        }
    }
    return changed;
}

bool PresenceState::DeclareOpenOnlinePlaySession(const std::string& profile_id) {
    return Update(profile_id, Application().first, DeclarationOpen, std::nullopt);
}

bool PresenceState::DeclareCloseOnlinePlaySession(const std::string& profile_id) {
    return Update(profile_id, Application().first, DeclarationClose, std::nullopt);
}

bool PresenceState::UpdateUserPresence(const std::string& profile_id,
                                       const UserPresenceImpl& presence) {
    return Update(profile_id, Application().first, presence.online_play_declaration,
                  BlobToAppField(presence.app_key_value_storage));
}

PresencePublication PresenceState::Current(const std::string& profile_id) const {
    const auto [application_id, presence_group_id] = Application();
    PresencePublication publication;
    if (application_id == 0) {
        return publication;
    }
    const DeclaredPresence declared = For(profile_id, application_id);
    publication.state = declared.session_open ? "PLAYING" : "ONLINE";
    publication.application_id = application_id;
    publication.presence_group_id = presence_group_id;
    publication.app_field = declared.app_field;
    return publication;
}

UserPresenceViewImpl PresenceState::OwnPresenceView(const std::string& profile_id) const {
    const auto [application_id, presence_group_id] = Application();
    const DeclaredPresence declared = For(profile_id, application_id);
    return ToUserPresenceViewImpl(application_id, presence_group_id, declared.updated_at,
                                  declared.session_open, declared.app_field);
}

void PresenceState::SetChangedCallback(std::function<void()> callback) {
    std::lock_guard lock(callback_mutex_);
    changed_ = std::move(callback);
}

void PresenceState::Clear() {
    std::lock_guard lock(mutex_);
    states_.clear();
}

std::string PresencePatch(const PresencePublication& publication) {
    return fmt::format(
        R"([{{"op":"replace","path":"/presence/state","value":"{}"}},)"
        R"({{"op":"add","path":"/presence/extras/friends/appField","value":{}}},)"
        R"({{"op":"add","path":"/presence/extras/friends/appInfo:appId","value":"{:016x}"}},)"
        R"({{"op":"add","path":"/presence/extras/friends/appInfo:acdIndex","value":0}},)"
        R"({{"op":"add","path":"/presence/extras/friends/appInfo:presenceGroupId","value":"{:016x}"}}])",
        publication.state, JsonString(publication.app_field), publication.application_id,
        publication.presence_group_id);
}

} // namespace OpenPak::Friends

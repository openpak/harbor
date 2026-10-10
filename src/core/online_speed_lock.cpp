// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <mutex>

#include "common/logging.h"
#include "common/settings.h"
#include "core/online_speed_lock.h"

namespace Core::OnlineSpeedLock {
namespace {

std::mutex mutex;
int holders = 0;
// Bumped by Reset, so holders from before it release nothing.
u64 generation = 1;
std::function<void(const std::string&)> notice;

/// What the user's own settings would run the guest at, if it is not plain real time.
bool UserSpeedDiffers() {
    const auto& v = Settings::values;
    return !v.use_speed_limit.GetValue() ||
           v.current_speed_mode.GetValue() != Settings::SpeedMode::Standard ||
           v.speed_limit.GetValue() != 100;
}

} // namespace

u64 Acquire(const char* what) {
    std::function<void(const std::string&)> tell;
    u64 ticket;
    {
        std::scoped_lock lk{mutex};
        ticket = generation;
        if (holders++ != 0) {
            return ticket;
        }
        Settings::SetOnlineSpeedLocked(true);
        const bool differs = UserSpeedDiffers();
        LOG_INFO(Core, "harbor: online play ({}): speed locked to 100%{}", what,
                 differs ? " (the user's speed setting is set aside)" : "");
        if (differs) {
            tell = notice;
        }
    }
    if (tell) {
        tell("Online play: speed locked to 100%");
    }
    return ticket;
}

void Release(const char* what, u64 ticket) {
    std::scoped_lock lk{mutex};
    if (ticket != generation || holders == 0 || --holders != 0) {
        return;
    }
    Settings::SetOnlineSpeedLocked(false);
    LOG_INFO(Core, "harbor: online play ended ({}): the user's speed setting applies again", what);
}

void Reset() {
    std::scoped_lock lk{mutex};
    generation++;
    if (holders == 0) {
        return;
    }
    holders = 0;
    Settings::SetOnlineSpeedLocked(false);
    LOG_INFO(Core, "harbor: emulation stopped: the user's speed setting applies again");
}

void SetNoticeHandler(std::function<void(const std::string&)> handler) {
    std::scoped_lock lk{mutex};
    notice = std::move(handler);
}

} // namespace Core::OnlineSpeedLock

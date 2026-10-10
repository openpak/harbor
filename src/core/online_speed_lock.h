// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <functional>
#include <string>

#include "common/common_types.h"

// Online play runs at real time. Peers in a session (Pia and the like) keep their own clocks and
// drop a console whose game runs ahead of or behind them: Mario Kart 8 Deluxe with the speed
// limiter off ran at 75+ fps on a phone and every race with it ended in a communication error.
// While the running application has an accepted internet request, or local wireless open, the
// guest is held at 100% (Settings::OnlineSpeedLocked); the user's own speed settings are not
// touched and apply again when the last holder goes.
namespace Core::OnlineSpeedLock {

/// One holder per accepted application network request or open local wireless network. Acquire
/// returns the holder's ticket for Release; a ticket from before a Reset releases nothing.
[[nodiscard]] u64 Acquire(const char* what);
void Release(const char* what, u64 ticket);

/// Drop every holder (the emulated system is going away).
void Reset();

/// The frontend's short host-side notice, called (from any thread) when the lock overrides a
/// speed the user had chosen: limiter off, turbo/slow mode, or a limit other than 100%.
void SetNoticeHandler(std::function<void(const std::string&)> handler);

} // namespace Core::OnlineSpeedLock

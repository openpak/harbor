// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base), HostBattery.cs.
//
// The console's battery as psm reports it, and the overlay notices psc sends when it changes
// (firmware 22.5.0 psc, invite-re ovln-senders-b-notes.md §5; the Ryujinx Harbor's
// OverlaySendersTests): 3001 when the charger state changes ("OvlBattery"), 3002 when the charge
// falls into a lower band (1 % or less, 2-5 %, 6-15 %: "OvlBattery_Low1/Low5"; the overlay drops it
// on a charger with enough power). Payload: u32 percentage, u32 charger type. psc's sender is the
// SDK default: queue 1, option 0x100 (the newest replaces an unreceived one).
//
// Only the standard library, so it can be checked on its own: see host_battery_selfcheck.cpp.

#pragma once

#include <algorithm>
#include <array>
#include <cstring>

#include "common/common_types.h"

namespace Service::PTM {

/// nn::psm::ChargerType.
enum class ChargerType : u32 {
    Unplugged = 0,
    RegularCharger = 1,
    LowPowerCharger = 2,
    Unknown = 3,
};

struct BatteryState {
    u32 percentage{100};
    ChargerType charger{ChargerType::RegularCharger};

    bool operator==(const BatteryState&) const = default;
};

constexpr u32 OverlayTagChargerChanged = 3001;
constexpr u32 OverlayTagBatteryLow = 3002;
constexpr u32 OverlaySenderQueueLength = 1;
constexpr u64 OverlaySendOption = 0x100;

/// psc's 1 s timer buckets: 0 for 1 % or less, 1 for 2-5 %, 2 for 6-15 %, 3 above.
constexpr int BatteryBand(u32 percentage) {
    return percentage <= 1 ? 0 : percentage <= 5 ? 1 : percentage <= 15 ? 2 : 3;
}

/// What a change from `before` to `after` sends.
struct BatteryNotices {
    bool charger_changed{}; // 3001
    bool low{};             // 3002
};

constexpr BatteryNotices CompareBattery(const BatteryState& before, const BatteryState& after) {
    return {
        .charger_changed = before.charger != after.charger,
        .low = BatteryBand(after.percentage) < BatteryBand(before.percentage) &&
               BatteryBand(after.percentage) != 3,
    };
}

constexpr BatteryState ClampBattery(int percentage, ChargerType charger) {
    return {static_cast<u32>(std::clamp(percentage, 0, 100)), charger};
}

inline std::array<u8, 8> BatteryNoticePayload(const BatteryState& state) {
    std::array<u8, 8> payload{};
    const u32 charger = static_cast<u32>(state.charger);
    std::memcpy(payload.data(), &state.percentage, sizeof(u32));
    std::memcpy(payload.data() + 4, &charger, sizeof(u32));
    return payload;
}

} // namespace Service::PTM

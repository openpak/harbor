// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "core/hle/service/psc/ovln/ovln_agent.h"
#include "core/hle/service/ptm/host_battery.h"
#include "core/hle/service/service.h"

namespace Core::Timing {
struct EventType;
}

namespace Service::PTM {

class IPsmSession;

class PSM final : public ServiceFramework<PSM> {
public:
    explicit PSM(Core::System& system_);
    ~PSM() override;

private:
    /// The battery psm reports: in a HOME session a full battery on a charger unless the host
    /// (Android) has a battery of its own; outside one, stock Eden's host reading.
    BatteryState ReadBattery() const;
    /// psc's 1 s check: on a change, signal the bound sessions and send the overlay its notices.
    void PollBattery();
    void SendOverlayNotice(u32 tag, const BatteryState& state);

    void GetBatteryChargePercentage(HLERequestContext& ctx);
    void GetChargerType(HLERequestContext& ctx);
    void OpenSession(HLERequestContext& ctx);
    void GetBatteryVoltageState(HLERequestContext& ctx);
    void GetBatteryAgePercentage(HLERequestContext& ctx);
    void GetBatteryChargeInfoFields(HLERequestContext& ctx);

    std::shared_ptr<Core::Timing::EventType> battery_poll_event;
    std::mutex battery_mutex;
    std::optional<BatteryState> last_battery;
    std::vector<std::weak_ptr<IPsmSession>> sessions;
    std::shared_ptr<PSC::OvlnAgent::SenderState> ovln_sender;
};

} // namespace Service::PTM

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string_view>

#include "common/common_funcs.h"
#include "common/device_power_state.h"
#include "common/logging.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/result.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/psc/ovln/ovln_types.h"
#include "core/hle/service/ptm/psm.h"
#include "common/harbor_crash.h"

namespace Service::PTM {

class IPsmSession final : public ServiceFramework<IPsmSession> {
public:
    explicit IPsmSession(Core::System& system_)
        : ServiceFramework{system_, "IPsmSession"}, service_context{system_, "IPsmSession"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &IPsmSession::BindStateChangeEvent, "BindStateChangeEvent"},
            {1, &IPsmSession::UnbindStateChangeEvent, "UnbindStateChangeEvent"},
            {2, &IPsmSession::SetChargerTypeChangeEventEnabled, "SetChargerTypeChangeEventEnabled"},
            {3, &IPsmSession::SetPowerSupplyChangeEventEnabled, "SetPowerSupplyChangeEventEnabled"},
            {4, &IPsmSession::SetBatteryVoltageStateChangeEventEnabled, "SetBatteryVoltageStateChangeEventEnabled"},
        };
        // clang-format on

        RegisterHandlers(functions);

        state_change_event = service_context.CreateEvent("IPsmSession::state_change_event");
    }

    ~IPsmSession() override {
        service_context.CloseEvent(state_change_event);
    }

    void SignalChargerTypeChanged() {
        if (should_signal && should_signal_charger_type) {
            state_change_event->Signal(system.Kernel());
        }
    }

    void SignalPowerSupplyChanged() {
        if (should_signal && should_signal_power_supply) {
            state_change_event->Signal(system.Kernel());
        }
    }

    void SignalBatteryVoltageStateChanged() {
        if (should_signal && should_signal_battery_voltage) {
            state_change_event->Signal(system.Kernel());
        }
    }

    /// The host battery changed: a bound session's event is signalled, as the Ryujinx Harbor does.
    void SignalBatteryChanged() {
        if (should_signal) {
            state_change_event->Signal(system.Kernel());
        }
    }

private:
    void BindStateChangeEvent(HLERequestContext& ctx) {
        LOG_DEBUG(Service_PTM, "called");

        should_signal = true;

        IPC::ResponseBuilder rb{ctx, 2, 1};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, state_change_event->GetReadableEvent());
    }

    void UnbindStateChangeEvent(HLERequestContext& ctx) {
        LOG_DEBUG(Service_PTM, "called");

        should_signal = false;

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void SetChargerTypeChangeEventEnabled(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto state = rp.Pop<bool>();
        LOG_DEBUG(Service_PTM, "called, state={}", state);

        should_signal_charger_type = state;

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void SetPowerSupplyChangeEventEnabled(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto state = rp.Pop<bool>();
        LOG_DEBUG(Service_PTM, "called, state={}", state);

        should_signal_power_supply = state;

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void SetBatteryVoltageStateChangeEventEnabled(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto state = rp.Pop<bool>();
        LOG_DEBUG(Service_PTM, "called, state={}", state);

        should_signal_battery_voltage = state;

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    KernelHelpers::ServiceContext service_context;

    bool should_signal_charger_type{};
    bool should_signal_power_supply{};
    bool should_signal_battery_voltage{};
    bool should_signal{};
    Kernel::KEvent* state_change_event;
};

PSM::PSM(Core::System& system_) : ServiceFramework{system_, "psm"} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, &PSM::GetBatteryChargePercentage, "GetBatteryChargePercentage"},
        {1, &PSM::GetChargerType, "GetChargerType"},
        {2, nullptr, "EnableBatteryCharging"},
        {3, nullptr, "DisableBatteryCharging"},
        {4, nullptr, "IsBatteryChargingEnabled"},
        {5, nullptr, "AcquireControllerPowerSupply"},
        {6, nullptr, "ReleaseControllerPowerSupply"},
        {7, &PSM::OpenSession, "OpenSession"},
        {8, nullptr, "EnableEnoughPowerChargeEmulation"},
        {9, nullptr, "DisableEnoughPowerChargeEmulation"},
        {10, nullptr, "EnableFastBatteryCharging"},
        {11, nullptr, "DisableFastBatteryCharging"},
        {12, &PSM::GetBatteryVoltageState, "GetBatteryVoltageState"},
        {13, nullptr, "GetRawBatteryChargePercentage"},
        {14, nullptr, "IsEnoughPowerSupplied"},
        {15, &PSM::GetBatteryAgePercentage, "GetBatteryAgePercentage"},
        {16, nullptr, "GetBatteryChargeInfoEvent"},
        {17, &PSM::GetBatteryChargeInfoFields, "GetBatteryChargeInfoFields"},
        {18, nullptr, "GetBatteryChargeCalibratedEvent"},
    };
    // clang-format on

    RegisterHandlers(functions);

    battery_poll_event = Core::Timing::CreateEvent(
        "PSM::BatteryPoll",
        [this](s64, std::chrono::nanoseconds) -> std::optional<std::chrono::nanoseconds> {
            PollBattery();
            return std::nullopt;
        });
    constexpr std::chrono::nanoseconds battery_poll_interval = std::chrono::seconds{1};
    system.CoreTiming().ScheduleLoopingEvent(battery_poll_interval, battery_poll_interval,
                                             battery_poll_event);
}

PSM::~PSM() {
    system.CoreTiming().UnscheduleEvent(battery_poll_event);
}

BatteryState PSM::ReadBattery() const {
    const Common::PowerStatus power_status = Common::GetPowerStatus();
    if (system.GetAppletManager().IsHomeSession()) {
        // A console's battery: the host's where it has one (Android, fed by the frontend's
        // NativeLibrary.updatePowerState), else a full battery on a charger.
#ifdef __ANDROID__
        if (power_status.has_battery && power_status.percentage >= 0) {
            return ClampBattery(power_status.percentage, power_status.charging
                                                             ? ChargerType::RegularCharger
                                                             : ChargerType::Unplugged);
        }
#else
        // Test runs: HARBOR_TEST_BATTERY names a file holding "<percent> <charging 0|1>", read
        // on every poll, so a desktop run can play a battery's changes.
        if (const char* path = std::getenv("HARBOR_TEST_BATTERY"); path != nullptr) {
            std::ifstream file{path};
            int percentage = -1;
            int charging = 0;
            if (file >> percentage >> charging && percentage >= 0) {
                return ClampBattery(percentage, charging != 0 ? ChargerType::RegularCharger
                                                              : ChargerType::Unplugged);
            }
        }
#endif
        return {100, ChargerType::RegularCharger};
    }
    BatteryState state{100, ChargerType::Unplugged};
    if (power_status.has_battery && power_status.percentage >= 0) {
        state.percentage = static_cast<u32>(std::clamp(power_status.percentage, 0, 100));
    }
    if (power_status.has_battery && power_status.charging) {
        state.charger = ChargerType::RegularCharger;
    }
    return state;
}

void PSM::PollBattery() {
    HARBOR_CRUMB_ONCE("psm: first battery poll");
    const BatteryState now = ReadBattery();

    BatteryNotices notices{};
    std::vector<std::shared_ptr<IPsmSession>> bound;
    {
        std::scoped_lock lk{battery_mutex};
        if (!last_battery) {
            // The first reading is where psc starts from: nothing is announced at boot.
            last_battery = now;
            return;
        }
        if (*last_battery == now) {
            return;
        }
        notices = CompareBattery(*last_battery, now);
        last_battery = now;
        std::erase_if(sessions, [](const auto& session) { return session.expired(); });
        for (const auto& weak : sessions) {
            if (auto session = weak.lock()) {
                bound.push_back(std::move(session));
            }
        }
    }

    LOG_DEBUG(Service_PTM, "psm: battery {}%, charger {}", now.percentage,
              static_cast<u32>(now.charger));
    for (const auto& session : bound) {
        session->SignalBatteryChanged();
    }

    if (!system.GetAppletManager().IsHomeSession()) {
        return;
    }
    if (notices.charger_changed) {
        SendOverlayNotice(OverlayTagChargerChanged, now);
    }
    if (notices.low) {
        SendOverlayNotice(OverlayTagBatteryLow, now);
    }
}

void PSM::SendOverlayNotice(u32 tag, const BatteryState& state) {
    // psc's own sender on the overlay's source, as on a console (the SDK default sender).
    const auto payload = BatteryNoticePayload(state);
    const PSC::OverlayNotification message = PSC::MakeNotification(tag, payload);
    const u64 tick = system.CoreTiming().GetClockTicks();

    auto& agent = PSC::GetOvlnAgent();
    PSC::OvlnStatus status = PSC::OvlnStatus::InvalidArgument;
    std::scoped_lock lk{battery_mutex};
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!ovln_sender) {
            PSC::SourceName name{};
            constexpr std::string_view source = "overlay";
            std::copy(source.begin(), source.end(), name.name.begin());
            if (agent.OpenSender(name, OverlaySenderQueueLength, ovln_sender) !=
                PSC::OvlnStatus::Success) {
                ovln_sender.reset();
                break;
            }
        }
        status = agent.Send(*ovln_sender, OverlaySendOption, message, tick);
        if (status != PSC::OvlnStatus::InvalidArgument) {
            break;
        }
        // A new ovln session closed this sender (OvlnAgent::Reset): open another, once.
        ovln_sender.reset();
    }
    LOG_INFO(Service_PTM, "psm: overlay notice {} ({}%, charger {}) -> {}", tag, state.percentage,
             static_cast<u32>(state.charger),
             status == PSC::OvlnStatus::Success ? "queued" : "not queued");
}

void PSM::GetBatteryChargePercentage(HLERequestContext& ctx) {
    LOG_DEBUG(Service_PTM, "called");

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u32>(ReadBattery().percentage);
}

void PSM::GetChargerType(HLERequestContext& ctx) {
    LOG_DEBUG(Service_PTM, "called");

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.PushEnum(ReadBattery().charger);
}

void PSM::OpenSession(HLERequestContext& ctx) {
    LOG_DEBUG(Service_PTM, "called");

    auto session = std::make_shared<IPsmSession>(system);
    {
        std::scoped_lock lk{battery_mutex};
        sessions.push_back(session);
    }

    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<IPsmSession>(ctx, std::move(session));
}

void PSM::GetBatteryVoltageState(HLERequestContext& ctx) {
    LOG_DEBUG(Service_PTM, "(stubbed)");
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.PushRaw<u32>(0); //
}

void PSM::GetBatteryAgePercentage(HLERequestContext& ctx) {
    LOG_DEBUG(Service_PTM, "(stubbed)");
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.PushRaw<f64>(1.f);
}

struct BatteryChargeInfoFields {
    u32 input_current_limit; //mA
    u32 boost_mode_current_limit;
    u32 fast_charge_current_limit;
    u32 charge_voltage_limit;
    u32 charger_type;
    u8 hi2_mode;
    u8 battery_charging;
    INSERT_PADDING_BYTES_NOINIT(2);
    u32 vdd50_state;
    u32 temperature_celcius;
    f32 battery_charge_percentage;
    u32 battery_charge_milli_voltage;
    f32 battery_age_percentage;
    u32 usb_power_role;
    u32 usb_charger_type;
    u32 charger_input_voltage_limit;
    u32 charger_input_current_limit;
    u8 fast_battery_charging;
    u8 controller_power_supply;
    u8 otg_request;
    INSERT_PADDING_BYTES_NOINIT(1);
    INSERT_PADDING_BYTES_NOINIT(0x14); //[+17.0.0]
};
static_assert(sizeof(struct BatteryChargeInfoFields) == 0x54);
void PSM::GetBatteryChargeInfoFields(HLERequestContext& ctx) {
    LOG_DEBUG(Service_PTM, "called");
    const BatteryState battery = ReadBattery();

    BatteryChargeInfoFields r{};
    r.battery_charge_percentage = f32(battery.percentage);
    r.battery_age_percentage = 100.f;
    r.battery_charging = battery.charger != ChargerType::Unplugged ? 1 : 0;
    r.charger_type = u32(battery.charger);
    r.charger_input_voltage_limit = 100;
    r.charger_input_voltage_limit = 100;
    r.input_current_limit = 100;
    r.boost_mode_current_limit = 100;
    r.fast_charge_current_limit = 100;

    IPC::ResponseBuilder rb{ctx, 2 + sizeof(BatteryChargeInfoFields) / sizeof(u32)};
    rb.Push(ResultSuccess);
    rb.PushRaw<BatteryChargeInfoFields>(r);
}

} // namespace Service::PTM

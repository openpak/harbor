// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/redact.h"
#include "core/core.h"
#include "core/file_sys/common_funcs.h"
#include "core/hle/service/am/applet.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/am/window_system.h"
#include "core/hle/service/am/service/applet_common_functions.h"
#include "core/hle/service/cmif_serialization.h"

namespace Service::AM {

IAppletCommonFunctions::IAppletCommonFunctions(Core::System& system_,
                                               std::shared_ptr<Applet> applet_)
    : ServiceFramework{system_, "IAppletCommonFunctions"}, applet{std::move(applet_)} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, nullptr, "SetTerminateResult"},
        {10, nullptr, "ReadThemeStorage"},
        {11, nullptr, "WriteThemeStorage"},
        {20, nullptr, "PushToAppletBoundChannel"},
        {21, nullptr, "TryPopFromAppletBoundChannel"},
        {40, nullptr, "GetDisplayLogicalResolution"},
        {42, D<&IAppletCommonFunctions::SetDisplayMagnification>, "SetDisplayMagnification"},
        {50, D<&IAppletCommonFunctions::SetHomeButtonDoubleClickEnabled>, "SetHomeButtonDoubleClickEnabled"},
        {51, D<&IAppletCommonFunctions::GetHomeButtonDoubleClickEnabled>, "GetHomeButtonDoubleClickEnabled"},
        {52, D<&IAppletCommonFunctions::IsHomeButtonShortPressedBlocked>, "IsHomeButtonShortPressedBlocked"},
        {60, D<&IAppletCommonFunctions::IsVrModeCurtainRequired>, "IsVrModeCurtainRequired"},
        {61, D<&IAppletCommonFunctions::IsSleepRequiredByHighTemperature>, "IsSleepRequiredByHighTemperature"},
        {62, D<&IAppletCommonFunctions::IsSleepRequiredByLowBattery>, "IsSleepRequiredByLowBattery"},
        {70, D<&IAppletCommonFunctions::SetCpuBoostRequestPriority>, "SetCpuBoostRequestPriority"},
        {80, nullptr, "SetHandlingCaptureButtonShortPressedMessageEnabledForApplet"},
        {81, nullptr, "SetHandlingCaptureButtonLongPressedMessageEnabledForApplet"},
        {90, nullptr, "OpenNamedChannelAsParent"},
        {91, nullptr, "OpenNamedChannelAsChild"},
        {100, D<&IAppletCommonFunctions::SetApplicationCoreUsageMode>, "SetApplicationCoreUsageMode"},
        {300, D<&IAppletCommonFunctions::GetCurrentApplicationId>, "GetCurrentApplicationId"},
        {310, nullptr, "IsSystemAppletHomeMenu"}, //19.0.0+
        {320, D<&IAppletCommonFunctions::SetGpuTimeSliceBoost>, "SetGpuTimeSliceBoost"}, //19.0.0+
        {321, nullptr, "SetGpuTimeSliceBoostDueToApplication"}, //19.0.0+
        {350, D<&IAppletCommonFunctions::Unknown350>, "Unknown350"} //20.0.0+
    };
    // clang-format on

    RegisterHandlers(functions);
}

IAppletCommonFunctions::~IAppletCommonFunctions() = default;

Result IAppletCommonFunctions::SetHomeButtonDoubleClickEnabled(
    bool home_button_double_click_enabled) {
    LOG_WARNING(Service_AM, "(STUBBED) called, home_button_double_click_enabled={}",
                home_button_double_click_enabled);
    R_SUCCEED();
}

Result IAppletCommonFunctions::GetHomeButtonDoubleClickEnabled(
    Out<bool> out_home_button_double_click_enabled) {
    LOG_WARNING(Service_AM, "(STUBBED) called");
    *out_home_button_double_click_enabled = false;
    R_SUCCEED();
}

Result IAppletCommonFunctions::SetDisplayMagnification(f32 x, f32 y, f32 width, f32 height) {
    LOG_DEBUG(Service_AM, "(STUBBED) called, x={}, y={}, width={}, height={}", x, y, width,
                height);
    std::scoped_lock lk{applet->lock};
    applet->display_magnification = Common::Rectangle<f32>{x, y, x + width, y + height};
    R_SUCCEED();
}

Result IAppletCommonFunctions::SetCpuBoostRequestPriority(s32 priority) {
    LOG_WARNING(Service_AM, "(STUBBED) called");
    std::scoped_lock lk{applet->lock};
    applet->cpu_boost_request_priority = priority;
    R_SUCCEED();
}

Result IAppletCommonFunctions::GetCurrentApplicationId(Out<u64> out_application_id) {
    LOG_WARNING(Service_AM, "(STUBBED) called");
    *out_application_id = FileSys::GetBaseTitleID(system.GetApplicationProcessProgramID());
    R_SUCCEED();
}

Result IAppletCommonFunctions::SetGpuTimeSliceBoost(s64 time_span) {
    LOG_WARNING(Service_AM, "(STUBBED) called, time_span={}", time_span);
    R_SUCCEED();
}

Result IAppletCommonFunctions::Unknown350(Out<u16> out_unknown, u64 argument) {
    // qlaunch sends 0xF; the Ryujinx Harbor answers 0.
    LOG_UNKNOWN_CONTRACT(Service_AM, "IAppletCommonFunctions 350 argument={:#x}; returning 0",
                         argument);
    *out_unknown = 0;
    R_SUCCEED();
}

Result IAppletCommonFunctions::IsHomeButtonShortPressedBlocked(Out<bool> out_is_blocked) {
    // The rule the HOME short press itself follows: the foreground application blocks it.
    auto* const window_system = system.GetAppletManager().GetWindowSystem();
    *out_is_blocked = window_system != nullptr && window_system->IsHomeButtonShortPressBlocked();
    LOG_DEBUG(Service_AM, "called, blocked={}", *out_is_blocked);
    R_SUCCEED();
}

Result IAppletCommonFunctions::IsVrModeCurtainRequired(Out<bool> out_is_required) {
    // VR mode is never enabled here.
    LOG_DEBUG(Service_AM, "called");
    *out_is_required = false;
    R_SUCCEED();
}

Result IAppletCommonFunctions::IsSleepRequiredByHighTemperature(Out<bool> out_is_required) {
    LOG_DEBUG(Service_AM, "called");
    *out_is_required = false;
    R_SUCCEED();
}

Result IAppletCommonFunctions::IsSleepRequiredByLowBattery(Out<bool> out_is_required) {
    LOG_DEBUG(Service_AM, "called");
    *out_is_required = false;
    R_SUCCEED();
}

Result IAppletCommonFunctions::SetApplicationCoreUsageMode(u32 mode) {
    // The Login web applet sends mode 1. Arguments beyond the mode are undocumented.
    LOG_DEBUG(Service_AM, "called, mode={}", mode);
    std::scoped_lock lk{applet->lock};
    applet->application_core_usage_mode = mode;
    R_SUCCEED();
}

} // namespace Service::AM

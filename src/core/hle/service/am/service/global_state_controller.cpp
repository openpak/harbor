// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/core.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/am/window_system.h"
#include "core/hle/service/am/service/cradle_firmware_updater.h"
#include "core/hle/service/am/service/global_state_controller.h"
#include "core/hle/service/cmif_serialization.h"

namespace Service::AM {

IGlobalStateController::IGlobalStateController(Core::System& system_)
    : ServiceFramework{system_, "IGlobalStateController"},
      m_context{system_, "IGlobalStateController"}, m_hdcp_authentication_failed_event{m_context} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, nullptr, "RequestToEnterSleep"},
        {1, nullptr, "EnterSleep"},
        {2, nullptr, "StartSleepSequence"},
        {3, D<&IGlobalStateController::StartShutdownSequence>, "StartShutdownSequence"},
        {4, D<&IGlobalStateController::StartRebootSequence>, "StartRebootSequence"},
        {9, D<&IGlobalStateController::IsAutoPowerDownRequested>, "IsAutoPowerDownRequested"},
        {10, D<&IGlobalStateController::LoadAndApplyIdlePolicySettings>, "LoadAndApplyIdlePolicySettings"},
        {11, D<&IGlobalStateController::NotifyCecSettingsChanged>, "NotifyCecSettingsChanged"},
        {12, D<&IGlobalStateController::SetDefaultHomeButtonLongPressTime>, "SetDefaultHomeButtonLongPressTime"},
        {13, D<&IGlobalStateController::UpdateDefaultDisplayResolution>, "UpdateDefaultDisplayResolution"},
        {14, D<&IGlobalStateController::ShouldSleepOnBoot>, "ShouldSleepOnBoot"},
        {15, D<&IGlobalStateController::GetHdcpAuthenticationFailedEvent>, "GetHdcpAuthenticationFailedEvent"},
        {30, D<&IGlobalStateController::OpenCradleFirmwareUpdater>, "OpenCradleFirmwareUpdater"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

Result IGlobalStateController::StartShutdownSequence() {
    LOG_INFO(Service_AM, "called");
    system.GetAppletManager().RequestPower(AppletManager::PowerRequest::Shutdown);
    R_SUCCEED();
}

Result IGlobalStateController::StartRebootSequence() {
    LOG_INFO(Service_AM, "called");
    system.GetAppletManager().RequestPower(AppletManager::PowerRequest::Reboot);
    R_SUCCEED();
}

Result IGlobalStateController::IsAutoPowerDownRequested(Out<bool> out_is_requested) {
    LOG_DEBUG(Service_AM, "called");
    *out_is_requested = false;
    R_SUCCEED();
}

Result IGlobalStateController::NotifyCecSettingsChanged() {
    LOG_DEBUG(Service_AM, "called");
    R_SUCCEED();
}

Result IGlobalStateController::SetDefaultHomeButtonLongPressTime(s64 time_ns) {
    LOG_DEBUG(Service_AM, "called, time_ns={}", time_ns);
    if (time_ns > 0) {
        if (auto* window_system = system.GetAppletManager().GetWindowSystem()) {
            window_system->SetHomeButtonLongPressTime(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::nanoseconds{time_ns}));
        }
    }
    R_SUCCEED();
}

Result IGlobalStateController::UpdateDefaultDisplayResolution() {
    LOG_DEBUG(Service_AM, "called");
    R_SUCCEED();
}

IGlobalStateController::~IGlobalStateController() = default;

Result IGlobalStateController::LoadAndApplyIdlePolicySettings() {
    LOG_WARNING(Service_AM, "(STUBBED) called");
    R_SUCCEED();
}

Result IGlobalStateController::ShouldSleepOnBoot(Out<bool> out_should_sleep_on_boot) {
    LOG_INFO(Service_AM, "called");
    *out_should_sleep_on_boot = false;
    R_SUCCEED();
}

Result IGlobalStateController::GetHdcpAuthenticationFailedEvent(
    OutCopyHandle<Kernel::KReadableEvent> out_event) {
    LOG_INFO(Service_AM, "called");
    *out_event = m_hdcp_authentication_failed_event.GetHandle();
    R_SUCCEED();
}

Result IGlobalStateController::OpenCradleFirmwareUpdater(
    Out<SharedPointer<ICradleFirmwareUpdater>> out_cradle_firmware_updater) {
    LOG_INFO(Service_AM, "called");
    *out_cradle_firmware_updater = std::make_shared<ICradleFirmwareUpdater>(system);
    R_SUCCEED();
}

} // namespace Service::AM

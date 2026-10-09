// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/settings.h"
#include "common/redact.h"
#include "core/hle/service/am/am_results.h"
#include "core/core.h"
#include "core/hle/service/am/applet.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/am/window_system.h"
#include "core/hle/service/am/service/common_state_getter.h"
#include "core/hle/service/am/service/lock_accessor.h"
#include "core/hle/service/am/service/storage.h"
#include "core/hle/service/apm/apm_interface.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/pm/pm.h"
#include "core/hle/service/sm/sm.h"
#include "core/hle/service/vi/vi_types.h"

namespace Service::AM {

namespace {
// In a HOME session readers and writers of one button share its reader/writer lock.
std::shared_ptr<LockState> SharedLockState(Core::System& system, u32 button_type) {
    if (!system.GetAppletManager().IsHomeSession()) {
        return nullptr;
    }
    auto* const window_system = system.GetAppletManager().GetWindowSystem();
    return window_system ? window_system->GetLockState(button_type) : nullptr;
}
} // namespace


ICommonStateGetter::ICommonStateGetter(Core::System& system_, std::shared_ptr<Applet> applet)
    : ServiceFramework{system_, "ICommonStateGetter"}, m_applet{std::move(applet)} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, D<&ICommonStateGetter::GetEventHandle>, "GetEventHandle"},
        {1, D<&ICommonStateGetter::ReceiveMessage>, "ReceiveMessage"},
        {2, nullptr, "GetThisAppletKind"},
        {3, nullptr, "AllowToEnterSleep"},
        {4, nullptr, "DisallowToEnterSleep"},
        {5, D<&ICommonStateGetter::GetOperationMode>, "GetOperationMode"},
        {6, D<&ICommonStateGetter::GetPerformanceMode>, "GetPerformanceMode"},
        {7, nullptr, "GetCradleStatus"},
        {8, D<&ICommonStateGetter::GetBootMode>, "GetBootMode"},
        {9, D<&ICommonStateGetter::GetCurrentFocusState>, "GetCurrentFocusState"},
        {10, D<&ICommonStateGetter::RequestToAcquireSleepLock>, "RequestToAcquireSleepLock"},
        {11, D<&ICommonStateGetter::ReleaseSleepLock>, "ReleaseSleepLock"},
        {12, D<&ICommonStateGetter::ReleaseSleepLockTransiently>, "ReleaseSleepLockTransiently"},
        {13, D<&ICommonStateGetter::GetAcquiredSleepLockEvent>, "GetAcquiredSleepLockEvent"},
        {14, D<&ICommonStateGetter::GetWakeupCount>, "GetWakeupCount"}, //11.0.0+
        {15, D<&ICommonStateGetter::GetSleepSequenceIdleEvent>, "GetSleepSequenceIdleEvent"}, //19.0.0+
        {20, D<&ICommonStateGetter::PushToGeneralChannel>, "PushToGeneralChannel"},
        {30, D<&ICommonStateGetter::GetHomeButtonReaderLockAccessor>, "GetHomeButtonReaderLockAccessor"},
        {31, D<&ICommonStateGetter::GetReaderLockAccessorEx>, "GetReaderLockAccessorEx"}, //2.0.0+
        {32, D<&ICommonStateGetter::GetWriterLockAccessorEx>, "GetWriterLockAccessorEx"}, //7.0.0+
        {40, D<&ICommonStateGetter::GetCradleFwVersion>, "GetCradleFwVersion"}, //2.0.0+
        {50, D<&ICommonStateGetter::IsVrModeEnabled>, "IsVrModeEnabled"}, //3.0.0+
        {51, D<&ICommonStateGetter::SetVrModeEnabled>, "SetVrModeEnabled"}, //3.0.0+
        {52, D<&ICommonStateGetter::SetLcdBacklighOffEnabled>, "SetLcdBacklighOffEnabled"}, //4.0.0+
        {53, D<&ICommonStateGetter::BeginVrModeEx>, "BeginVrModeEx"}, //7.0.0+
        {54, D<&ICommonStateGetter::EndVrModeEx>, "EndVrModeEx"}, //7.0.0+
        {55, D<&ICommonStateGetter::IsInControllerFirmwareUpdateSection>, "IsInControllerFirmwareUpdateSection"}, //3.0.0+
        {59, nullptr, "SetVrPositionForDebug"}, //1.0.0+
        {60, D<&ICommonStateGetter::GetDefaultDisplayResolution>, "GetDefaultDisplayResolution"},
        {61, D<&ICommonStateGetter::GetDefaultDisplayResolutionChangeEvent>, "GetDefaultDisplayResolutionChangeEvent"},
        {62, D<&ICommonStateGetter::GetHdcpAuthenticationState>, "GetHdcpAuthenticationState"},
        {63, D<&ICommonStateGetter::GetHdcpAuthenticationStateChangeEvent>, "GetHdcpAuthenticationStateChangeEvent"},
        {64, nullptr, "SetTvPowerStateMatchingMode"},
        {65, nullptr, "GetApplicationIdByContentActionName"},
        {66, &ICommonStateGetter::SetCpuBoostMode, "SetCpuBoostMode"},
        {67, nullptr, "CancelCpuBoostMode"},
        {68, D<&ICommonStateGetter::GetBuiltInDisplayType>, "GetBuiltInDisplayType"},
        {80, D<&ICommonStateGetter::PerformSystemButtonPressingIfInFocus>, "PerformSystemButtonPressingIfInFocus"},
        {90, nullptr, "SetPerformanceConfigurationChangedNotification"},
        {91, nullptr, "GetCurrentPerformanceConfiguration"},
        {100, D<&ICommonStateGetter::SetHandlingHomeButtonShortPressedEnabled>, "SetHandlingHomeButtonShortPressedEnabled"},
        {110, nullptr, "OpenMyGpuErrorHandler"},
        {120, D<&ICommonStateGetter::GetAppletLaunchedHistory>, "GetAppletLaunchedHistory"}, //13.0.0+
        {130, D<&ICommonStateGetter::EnableStartupLogoDisappearedMessage>, "EnableStartupLogoDisappearedMessage"}, //21.0.0+
        {200, D<&ICommonStateGetter::GetOperationModeSystemInfo>, "GetOperationModeSystemInfo"},
        {300, D<&ICommonStateGetter::GetSettingsPlatformRegion>, "GetSettingsPlatformRegion"},
        {400, nullptr, "ActivateMigrationService"},
        {401, nullptr, "DeactivateMigrationService"},
        {500, D<&ICommonStateGetter::DisableSleepTillShutdown>, "DisableSleepTillShutdown"},
        {501, D<&ICommonStateGetter::SuppressDisablingSleepTemporarily>, "SuppressDisablingSleepTemporarily"},
        {502, D<&ICommonStateGetter::IsSleepEnabled>, "IsSleepEnabled"},
        {503, D<&ICommonStateGetter::IsDisablingSleepSuppressed>, "IsDisablingSleepSuppressed"},
        {600, nullptr, "SetHidInputMagnificationForApplication"}, //20.0.0+
        {610, D<&ICommonStateGetter::Unknown610>, "Unknown610"}, //21.0.0+
        {611, D<&ICommonStateGetter::Unknown611>, "Unknown611"}, //22.0.0+
        {900, D<&ICommonStateGetter::SetRequestExitToLibraryAppletAtExecuteNextProgramEnabled>, "SetRequestExitToLibraryAppletAtExecuteNextProgramEnabled"}, //11.0.0+
        {910, nullptr, "GetLaunchRequiredTick"}, //17.0.0+
        {1000, D<&ICommonStateGetter::BeginVrMode3d>, "BeginVrMode3d"}, //19.0.0+
        {1001, D<&ICommonStateGetter::EndVrMode3d>, "EndVrMode3d"}, //19.0.0+
        {1002, D<&ICommonStateGetter::IsVrModeEnabled3d>, "IsVrModeEnabled3d"}, //19.0.0+
        {1003, D<&ICommonStateGetter::GetVrLaboGoggleViewport>, "GetVrLaboGoggleViewport"}, //21.0.0+
        {1004, D<&ICommonStateGetter::GetPanelPhysicalSizeForSpecificTitle>, "GetPanelPhysicalSizeForSpecificTitle"}, //21.0.0+
        {1005, D<&ICommonStateGetter::GetPanelResolutionForSpecificTitle>, "GetPanelResolutionForSpecificTitle"}, //21.0.0+
    };
    // clang-format on

    RegisterHandlers(functions);
}

ICommonStateGetter::~ICommonStateGetter() = default;

Result ICommonStateGetter::GetEventHandle(OutCopyHandle<Kernel::KReadableEvent> out_event) {
    LOG_DEBUG(Service_AM, "called");
    *out_event = m_applet->lifecycle_manager.GetSystemEvent().GetHandle();
    R_SUCCEED();
}

Result ICommonStateGetter::ReceiveMessage(Out<AppletMessage> out_applet_message) {
    LOG_DEBUG(Service_AM, "called");

    if (!m_applet->lifecycle_manager.PopMessage(system.Kernel(), out_applet_message)) {
        LOG_ERROR(Service_AM, "Tried to pop message but none was available!");
        R_THROW(AM::ResultNoMessages);
    }

    LOG_DEBUG(Service_AM, "called, returning message={} to applet_id={}",
             static_cast<u32>(*out_applet_message), static_cast<u32>(m_applet->applet_id));

    R_SUCCEED();
}

Result ICommonStateGetter::GetCurrentFocusState(Out<FocusState> out_focus_state) {
    LOG_DEBUG(Service_AM, "called");

    std::scoped_lock lk{m_applet->lock};
    *out_focus_state = m_applet->lifecycle_manager.GetAndClearFocusState();

    R_SUCCEED();
}

// The sleep lock holds a sleep sequence back until the applet answers its RequestToPrepareSleep
// with a release (sleep_sequence.cpp). Without a window system (no am session) it is granted at once.
Result ICommonStateGetter::RequestToAcquireSleepLock() {
    LOG_DEBUG(Service_AM, "called");

    if (auto* window_system = system.GetAppletManager().GetWindowSystem()) {
        window_system->RequestToAcquireSleepLock(*m_applet);
        R_SUCCEED();
    }
    {
        std::scoped_lock lk{m_applet->lock};
        m_applet->sleep_lock_enabled = true;
    }
    m_applet->sleep_lock_event.Signal(system.Kernel());
    R_SUCCEED();
}

Result ICommonStateGetter::ReleaseSleepLock() {
    LOG_DEBUG(Service_AM, "called");

    if (auto* window_system = system.GetAppletManager().GetWindowSystem()) {
        R_RETURN(window_system->ReleaseSleepLock(*m_applet));
    }
    {
        std::scoped_lock lk{m_applet->lock};
        m_applet->sleep_lock_enabled = false;
    }
    m_applet->sleep_lock_event.Clear(system.Kernel());
    R_SUCCEED();
}

Result ICommonStateGetter::ReleaseSleepLockTransiently() {
    LOG_DEBUG(Service_AM, "called");

    if (auto* window_system = system.GetAppletManager().GetWindowSystem()) {
        R_RETURN(window_system->ReleaseSleepLockTransiently(*m_applet));
    }
    R_SUCCEED();
}

Result ICommonStateGetter::GetAcquiredSleepLockEvent(
    OutCopyHandle<Kernel::KReadableEvent> out_event) {
    LOG_DEBUG(Service_AM, "called");
    *out_event = m_applet->sleep_lock_event.GetHandle();
    R_SUCCEED();
}

Result ICommonStateGetter::GetWakeupCount(Out<u64> out_count) {
    auto* window_system = system.GetAppletManager().GetWindowSystem();
    *out_count = window_system ? window_system->GetWakeupCount() : 0;
    LOG_DEBUG(Service_AM, "called, count={}", *out_count);
    R_SUCCEED();
}

// Signalled while no sleep or shutdown sequence runs (the 23.0.1 am's power-state event at
// +0xA0; overlayDisp waits on it).
Result ICommonStateGetter::GetSleepSequenceIdleEvent(
    OutCopyHandle<Kernel::KReadableEvent> out_event) {
    LOG_DEBUG(Service_AM, "called");
    auto* window_system = system.GetAppletManager().GetWindowSystem();
    R_UNLESS(window_system != nullptr, AM::ResultNotImplemented);
    *out_event = window_system->GetSleepSequenceIdleEvent();
    R_SUCCEED();
}

Result ICommonStateGetter::DisableSleepTillShutdown() {
    LOG_INFO(Service_AM, "called");
    if (auto* window_system = system.GetAppletManager().GetWindowSystem()) {
        window_system->DisableSleepTillShutdown();
    }
    R_SUCCEED();
}

Result ICommonStateGetter::SuppressDisablingSleepTemporarily(s64 duration_ns) {
    LOG_INFO(Service_AM, "called, duration_ns={}", duration_ns);
    if (auto* window_system = system.GetAppletManager().GetWindowSystem()) {
        R_RETURN(window_system->SuppressDisablingSleepTemporarily(duration_ns));
    }
    R_SUCCEED();
}

Result ICommonStateGetter::IsSleepEnabled(Out<bool> out_is_enabled) {
    auto* window_system = system.GetAppletManager().GetWindowSystem();
    *out_is_enabled = window_system == nullptr || window_system->IsSleepEnabled();
    LOG_DEBUG(Service_AM, "called, enabled={}", *out_is_enabled);
    R_SUCCEED();
}

Result ICommonStateGetter::IsDisablingSleepSuppressed(Out<bool> out_is_suppressed) {
    auto* window_system = system.GetAppletManager().GetWindowSystem();
    *out_is_suppressed = window_system != nullptr && window_system->IsDisablingSleepSuppressed();
    LOG_DEBUG(Service_AM, "called, suppressed={}", *out_is_suppressed);
    R_SUCCEED();
}

Result ICommonStateGetter::GetReaderLockAccessorEx(
    Out<SharedPointer<ILockAccessor>> out_lock_accessor, u32 button_type) {
    LOG_INFO(Service_AM, "called, button_type={}", button_type);
    *out_lock_accessor = std::make_shared<ILockAccessor>(
        system, SharedLockState(system, button_type), LockKind::Reader);
    R_SUCCEED();
}

Result ICommonStateGetter::GetWriterLockAccessorEx(
    Out<SharedPointer<ILockAccessor>> out_lock_accessor, u32 button_type) {
    LOG_INFO(Service_AM, "called, button_type={}", button_type);
    *out_lock_accessor =
        std::make_shared<ILockAccessor>(system, SharedLockState(system, button_type));
    R_SUCCEED();
}

Result ICommonStateGetter::GetDefaultDisplayResolutionChangeEvent(
    OutCopyHandle<Kernel::KReadableEvent> out_event) {
    LOG_DEBUG(Service_AM, "called");
    *out_event = m_applet->lifecycle_manager.GetOperationModeChangedSystemEvent().GetHandle();
    R_SUCCEED();
}

Result ICommonStateGetter::GetHdcpAuthenticationState(Out<s32> out_state) {
    LOG_DEBUG(Service_AM, "called");
    *out_state = 1;
    R_SUCCEED();
}

Result ICommonStateGetter::GetHdcpAuthenticationStateChangeEvent(
    OutCopyHandle<Kernel::KReadableEvent> out_event) {
    LOG_DEBUG(Service_AM, "called");
    *out_event = m_applet->lifecycle_manager.GetHDCPStateChangedEvent().GetHandle();
    R_SUCCEED();
}

Result ICommonStateGetter::GetOperationMode(Out<OperationMode> out_operation_mode) {
    const bool use_docked_mode{Settings::IsDockedMode()};
    LOG_DEBUG(Service_AM, "called, use_docked_mode={}", use_docked_mode);
    *out_operation_mode = use_docked_mode ? OperationMode::Docked : OperationMode::Handheld;
    R_SUCCEED();
}

Result ICommonStateGetter::GetPerformanceMode(Out<APM::PerformanceMode> out_performance_mode) {
    LOG_DEBUG(Service_AM, "called");
    *out_performance_mode = system.GetAPMController().GetCurrentPerformanceMode();
    R_SUCCEED();
}

Result ICommonStateGetter::GetBootMode(Out<PM::SystemBootMode> out_boot_mode) {
    LOG_DEBUG(Service_AM, "called");
    *out_boot_mode = Service::PM::SystemBootMode::Normal;
    R_SUCCEED();
}

Result ICommonStateGetter::IsVrModeEnabled(Out<bool> out_is_vr_mode_enabled) {
    LOG_DEBUG(Service_AM, "called");

    std::scoped_lock lk{m_applet->lock};
    *out_is_vr_mode_enabled = m_applet->vr_mode_enabled;
    R_SUCCEED();
}

Result ICommonStateGetter::SetVrModeEnabled(bool is_vr_mode_enabled) {
    std::scoped_lock lk{m_applet->lock};
    m_applet->vr_mode_enabled = is_vr_mode_enabled;
    LOG_WARNING(Service_AM, "VR Mode is {}", m_applet->vr_mode_enabled ? "on" : "off");
    R_SUCCEED();
}

Result ICommonStateGetter::SetLcdBacklighOffEnabled(bool is_lcd_backlight_off_enabled) {
    LOG_UNKNOWN_CONTRACT(Service_AM, "ICommonStateGetter 52 SetLcdBacklighOffEnabled: not emulated: the backlight is not switched; enabled={}", is_lcd_backlight_off_enabled);
    R_SUCCEED();
}

Result ICommonStateGetter::BeginVrModeEx() {
    // VR mode is kept per applet and answered by IsVrModeEnabled.
    LOG_DEBUG(Service_AM, "called");
    std::scoped_lock lk{m_applet->lock};
    m_applet->vr_mode_enabled = true;
    R_SUCCEED();
}

Result ICommonStateGetter::EndVrModeEx() {
    LOG_DEBUG(Service_AM, "called");
    std::scoped_lock lk{m_applet->lock};
    m_applet->vr_mode_enabled = false;
    R_SUCCEED();
}

Result ICommonStateGetter::IsInControllerFirmwareUpdateSection(
    Out<bool> out_is_in_controller_firmware_update_section) {
    LOG_INFO(Service_AM, "called");
    *out_is_in_controller_firmware_update_section = false;
    R_SUCCEED();
}

Result ICommonStateGetter::GetDefaultDisplayResolution(Out<s32> out_width, Out<s32> out_height) {
    LOG_DEBUG(Service_AM, "called");

    if (Settings::IsDockedMode()) {
        *out_width = static_cast<u32>(Service::VI::DisplayResolution::DockedWidth);
        *out_height = static_cast<u32>(Service::VI::DisplayResolution::DockedHeight);
    } else {
        *out_width = static_cast<u32>(Service::VI::DisplayResolution::UndockedWidth);
        *out_height = static_cast<u32>(Service::VI::DisplayResolution::UndockedHeight);
    }

    R_SUCCEED();
}

void ICommonStateGetter::SetCpuBoostMode(HLERequestContext& ctx) {
    LOG_DEBUG(Service_AM, "called, forwarding to APM:SYS");

    const auto& sm = system.ServiceManager();
    const auto apm_sys = sm.GetService<APM::APM_Sys>("apm:sys");
    ASSERT(apm_sys != nullptr);

    apm_sys->SetCpuBoostMode(ctx);
}

Result ICommonStateGetter::GetBuiltInDisplayType(Out<s32> out_display_type) {
    LOG_UNKNOWN_CONTRACT(Service_AM, "ICommonStateGetter 68 GetBuiltInDisplayType: answered 0 (the values are not established)");
    *out_display_type = 0;
    R_SUCCEED();
}

Result ICommonStateGetter::PerformSystemButtonPressingIfInFocus(SystemButtonType type) {
    LOG_DEBUG(Service_AM, "called, type={}", type);

    std::scoped_lock lk{m_applet->lock};

    switch (type) {
    case SystemButtonType::HomeButtonShortPressing:
        if (!m_applet->home_button_short_pressed_blocked) {
            m_applet->lifecycle_manager.PushUnorderedMessage(system.Kernel(),
                AppletMessage::DetectShortPressingHomeButton);
        }
        break;
    case SystemButtonType::HomeButtonLongPressing:
        if (!m_applet->home_button_long_pressed_blocked) {
            m_applet->lifecycle_manager.PushUnorderedMessage(system.Kernel(),
                AppletMessage::DetectLongPressingHomeButton);
        }
        break;
    case SystemButtonType::CaptureButtonShortPressing:
        if (m_applet->handling_capture_button_short_pressed_message_enabled_for_applet) {
            m_applet->lifecycle_manager.PushUnorderedMessage(system.Kernel(),
                AppletMessage::DetectShortPressingCaptureButton);
        }
        break;
    case SystemButtonType::CaptureButtonLongPressing:
        if (m_applet->handling_capture_button_long_pressed_message_enabled_for_applet) {
            m_applet->lifecycle_manager.PushUnorderedMessage(system.Kernel(),
                AppletMessage::DetectLongPressingCaptureButton);
        }
        break;
    default:
        // Other buttons ignored for now
        break;
    }

    R_SUCCEED();
}

Result ICommonStateGetter::EnableStartupLogoDisappearedMessage() {
    // The startup logo is gone at once here, so the message comes at once.
    LOG_DEBUG(Service_AM, "called");
    m_applet->lifecycle_manager.PushUnorderedMessage(system.Kernel(), AppletMessage::StartupLogoDisappeared);
    R_SUCCEED();
}

Result ICommonStateGetter::GetOperationModeSystemInfo(Out<u32> out_operation_mode_system_info) {
    LOG_UNKNOWN_CONTRACT(Service_AM, "ICommonStateGetter 200 GetOperationModeSystemInfo: answered 0 (the meaning is not established)");
    *out_operation_mode_system_info = 0;
    R_SUCCEED();
}

Result ICommonStateGetter::GetAppletLaunchedHistory(
    Out<s32> out_count, OutArray<AppletId, BufferAttr_HipcMapAlias> out_applet_ids) {
    LOG_INFO(Service_AM, "called");

    std::shared_ptr<Applet> current_applet = m_applet;

    for (*out_count = 0;
         *out_count < static_cast<s32>(out_applet_ids.size()) && current_applet != nullptr;
         /* ... */) {
        out_applet_ids[(*out_count)++] = current_applet->applet_id;
        current_applet = current_applet->caller_applet.lock();
    }

    R_SUCCEED();
}

Result ICommonStateGetter::GetSettingsPlatformRegion(
    Out<Set::PlatformRegion> out_settings_platform_region) {
    LOG_INFO(Service_AM, "called");
    *out_settings_platform_region = Set::PlatformRegion::Global;
    R_SUCCEED();
}

Result ICommonStateGetter::SetRequestExitToLibraryAppletAtExecuteNextProgramEnabled() {
    LOG_UNKNOWN_CONTRACT(Service_AM, "ICommonStateGetter 900 SetRequestExitToLibraryAppletAtExecuteNextProgramEnabled: kept per applet, not acted on at ExecuteProgram");

    std::scoped_lock lk{m_applet->lock};
    m_applet->request_exit_to_library_applet_at_execute_next_program_enabled = true;

    R_SUCCEED();
}

Result ICommonStateGetter::PushToGeneralChannel(SharedPointer<IStorage> storage) {
    LOG_DEBUG(Service_AM, "called");
    system.PushGeneralChannelData(storage->GetData());
    R_SUCCEED();
}

Result ICommonStateGetter::GetHomeButtonReaderLockAccessor(Out<SharedPointer<ILockAccessor>> out_lock_accessor) {
    LOG_DEBUG(Service_AM, "called");
    *out_lock_accessor =
        std::make_shared<ILockAccessor>(system, SharedLockState(system, 0), LockKind::Reader);
    R_SUCCEED();
}

Result ICommonStateGetter::SetHandlingHomeButtonShortPressedEnabled(bool enabled) {
    LOG_DEBUG(Service_AM, "called, enabled={} applet_id={}", enabled, m_applet->applet_id);

    std::scoped_lock lk{m_applet->lock};
    if (system.GetAppletManager().IsHomeSession()) {
        // The applet takes the HOME short press itself while it holds the foreground.
        m_applet->handles_home_button_short_press = enabled;
    } else {
        m_applet->home_button_short_pressed_blocked = !enabled;
    }
    R_SUCCEED();
}

Result ICommonStateGetter::GetCradleFwVersion(Out<u32> out_pdc_h, Out<u32> out_pdc_a,
                                              Out<u32> out_mcu, Out<u32> out_dp2hdmi) {
    // What a console answers with no dock attached is not public: zeros, as the Ryujinx Harbor.
    LOG_WARNING(Service_AM, "unknown contract: ICommonStateGetter 40 GetCradleFwVersion -> zeros");
    *out_pdc_h = 0;
    *out_pdc_a = 0;
    *out_mcu = 0;
    *out_dp2hdmi = 0;
    R_SUCCEED();
}

Result ICommonStateGetter::Unknown610(u32 argument) {
    // qlaunch sends 0, then 611 with 1; answered with no output, as the Ryujinx Harbor does.
    LOG_UNKNOWN_CONTRACT(Service_AM, "ICommonStateGetter 610 argument={:#x}; returning success",
                         argument);
    R_SUCCEED();
}

Result ICommonStateGetter::Unknown611(u32 argument) {
    LOG_UNKNOWN_CONTRACT(Service_AM, "ICommonStateGetter 611 argument={:#x}; returning success",
                         argument);
    R_SUCCEED();
}

Result ICommonStateGetter::BeginVrMode3d() {
    std::scoped_lock lk{m_applet->lock};
    m_applet->vr_mode_enabled_3d = true;
    LOG_WARNING(Service_AM, "VR Mode is {}", m_applet->vr_mode_enabled_3d ? "on" : "off");
    R_SUCCEED();
}

Result ICommonStateGetter::EndVrMode3d() {
    std::scoped_lock lk{m_applet->lock};
    m_applet->vr_mode_enabled_3d = false;
    LOG_WARNING(Service_AM, "VR Mode is {}", m_applet->vr_mode_enabled_3d ? "on" : "off");
    R_SUCCEED();
}

Result ICommonStateGetter::IsVrModeEnabled3d(Out<bool> out_is_vr_mode_enabled_3d) {
    LOG_DEBUG(Service_AM, "called");
    std::scoped_lock lk{m_applet->lock};
    *out_is_vr_mode_enabled_3d = m_applet->vr_mode_enabled_3d;
    R_SUCCEED();
}

Result ICommonStateGetter::GetVrLaboGoggleViewport(Out<s32> out_x, Out<s32> out_y, Out<s32> out_width, Out<s32> out_height) {
    LOG_UNKNOWN_CONTRACT(Service_AM, "ICommonStateGetter 1003 GetVrLaboGoggleViewport: answered the full 1280x720 screen (not established)");
    *out_x = 0;
    *out_y = 0;
    *out_width = 1280;
    *out_height = 720;
    R_SUCCEED();
}

Result ICommonStateGetter::GetPanelPhysicalSizeForSpecificTitle(Out<f32> out_width, Out<f32> out_height) {
    LOG_UNKNOWN_CONTRACT(Service_AM, "ICommonStateGetter 1004 GetPanelPhysicalSizeForSpecificTitle: answered 137.25 x 77.2 mm (not established)");
    *out_width = 137250.0f / 1000.0f;
    *out_height = 77200.0f / 1000.0f;
    R_SUCCEED();
}

Result ICommonStateGetter::GetPanelResolutionForSpecificTitle(Out<s32> out_width, Out<s32> out_height) {
    LOG_UNKNOWN_CONTRACT(Service_AM, "ICommonStateGetter 1005 GetPanelResolutionForSpecificTitle: answered 1280x720 (not established)");
    *out_width = 1280;
    *out_height = 720;
    R_SUCCEED();
}

} // namespace Service::AM

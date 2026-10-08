// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/redact.h"
#include "core/core.h"
#include "core/hle/service/am/am_results.h"
#include "core/hle/service/am/applet.h"
#include "core/hle/service/am/service/overlay_functions.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/am/window_system.h"

namespace Service::AM {
    IOverlayFunctions::IOverlayFunctions(Core::System &system_, std::shared_ptr<Applet> applet)
        : ServiceFramework{system_, "IOverlayFunctions"}, m_applet{std::move(applet)} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, D<&IOverlayFunctions::BeginToWatchShortHomeButtonMessage>, "BeginToWatchShortHomeButtonMessage"},
        {1, D<&IOverlayFunctions::EndToWatchShortHomeButtonMessage>, "EndToWatchShortHomeButtonMessage"},
        {2, D<&IOverlayFunctions::GetApplicationIdForLogo>, "GetApplicationIdForLogo"},
        {3, D<&IOverlayFunctions::SetGpuTimeSliceBoost>, "SetGpuTimeSliceBoost"},
        {4, D<&IOverlayFunctions::SetAutoSleepTimeAndDimmingTimeEnabled>, "SetAutoSleepTimeAndDimmingTimeEnabled"},
        {5, D<&IOverlayFunctions::TerminateApplicationAndSetReason>, "TerminateApplicationAndSetReason"},
        {6, D<&IOverlayFunctions::SetScreenShotPermissionGlobally>, "SetScreenShotPermissionGlobally"},
        {10, D<&IOverlayFunctions::StartShutdownSequenceForOverlay>, "StartShutdownSequenceForOverlay"},
        {11, D<&IOverlayFunctions::StartRebootSequenceForOverlay>, "StartRebootSequenceForOverlay"},
        {20, D<&IOverlayFunctions::SetHandlingHomeButtonShortPressedEnabled>, "SetHandlingHomeButtonShortPressedEnabled"},
        {21, D<&IOverlayFunctions::SetHandlingTouchScreenInputEnabled>, "SetHandlingTouchScreenInputEnabled"},
        {30, D<&IOverlayFunctions::SetHealthWarningShowingState>, "SetHealthWarningShowingState"},
        {31, D<&IOverlayFunctions::IsHealthWarningRequired>, "IsHealthWarningRequired"},
        {40, D<&IOverlayFunctions::GetApplicationNintendoLogo>, "GetApplicationNintendoLogo"},
        {41, D<&IOverlayFunctions::GetApplicationStartupMovie>, "GetApplicationStartupMovie"},
        {50, D<&IOverlayFunctions::SetGpuTimeSliceBoostForApplication>, "SetGpuTimeSliceBoostForApplication"},
        {60, D<&IOverlayFunctions::Unknown60>, "Unknown60"},
        {70, D<&IOverlayFunctions::Unknown70>, "Unknown70"},
        {71, D<&IOverlayFunctions::Unknown71>, "Unknown71"},
        {75, D<&IOverlayFunctions::Unknown75>, "Unknown75"},
        {90, D<&IOverlayFunctions::SetRequiresGpuResourceUse>, "SetRequiresGpuResourceUse"},
        {101, D<&IOverlayFunctions::BeginToObserveHidInputForDevelop>, "BeginToObserveHidInputForDevelop"},
    };
        // clang-format on

        RegisterHandlers(functions);
    }

    IOverlayFunctions::~IOverlayFunctions() = default;

    Result IOverlayFunctions::BeginToWatchShortHomeButtonMessage() {
        LOG_DEBUG(Service_AM, "called");

        {
            std::scoped_lock lk{m_applet->lock};
            m_applet->overlay_watching_short_home_button = true;
        }

        if (auto* window_system = system.GetAppletManager().GetWindowSystem()) {
            window_system->RequestUpdate();
        }

        R_SUCCEED();
    }

    Result IOverlayFunctions::EndToWatchShortHomeButtonMessage() {
        LOG_DEBUG(Service_AM, "called");

        {
            std::scoped_lock lk{m_applet->lock};
            m_applet->overlay_watching_short_home_button = false;
        }

        if (auto* window_system = system.GetAppletManager().GetWindowSystem()) {
            window_system->RequestUpdate();
        }

        R_SUCCEED();
    }

    Result IOverlayFunctions::SetHandlingTouchScreenInputEnabled(bool enabled) {
        LOG_DEBUG(Service_AM, "called, enabled={}", enabled);

        std::scoped_lock lk{m_applet->lock};
        m_applet->overlay_handling_touch_input = enabled;
        R_SUCCEED();
    }

    Result IOverlayFunctions::GetApplicationIdForLogo(Out<u64> out_application_id) {
        LOG_DEBUG(Service_AM, "called");

        std::shared_ptr<Applet> target_applet;

        auto* window_system = system.GetAppletManager().GetWindowSystem();
        if (window_system) {
            target_applet = window_system->GetMainApplet();
            if (target_applet) {
                std::scoped_lock lk{target_applet->lock};
                LOG_DEBUG(Service_AM, "applet_id={}, program_id={:016X}, type={}",
                          static_cast<u32>(target_applet->applet_id), target_applet->program_id,
                          static_cast<u32>(target_applet->type));

                u64 id = target_applet->screen_shot_identity.application_id;
                if (id == 0) {
                    id = target_applet->program_id;
                }
                LOG_DEBUG(Service_AM, "application_id={:016X}", id);
                *out_application_id = id;
                R_SUCCEED();
            }
        }

        std::scoped_lock lk{m_applet->lock};
        u64 id = m_applet->screen_shot_identity.application_id;
        if (id == 0) {
            id = m_applet->program_id;
        }
        LOG_DEBUG(Service_AM, "application_id={:016X} (fallback)", id);
        *out_application_id = id;
        R_SUCCEED();
    }

    Result IOverlayFunctions::SetAutoSleepTimeAndDimmingTimeEnabled(bool enabled) {
        // Kept per applet for the idle timer.
        LOG_DEBUG(Service_AM, "called, enabled={}", enabled);
        std::scoped_lock lk{m_applet->lock};
        m_applet->auto_sleep_disabled = !enabled;
        R_SUCCEED();
    }

    Result IOverlayFunctions::IsHealthWarningRequired(Out<bool> is_required) {
        LOG_DEBUG(Service_AM, "called");
        std::scoped_lock lk{m_applet->lock};
        *is_required = false;
        R_SUCCEED();
    }

    Result IOverlayFunctions::SetHandlingHomeButtonShortPressedEnabled(bool enabled) {
        LOG_DEBUG(Service_AM, "called, enabled={}", enabled);
        std::scoped_lock lk{m_applet->lock};
        if (system.GetAppletManager().IsHomeSession()) {
            // While its quick settings watch HOME, the overlay keeps a short press from the HOME
            // menu.
            m_applet->overlay_handles_home_button_short_press = enabled;
        } else {
            m_applet->home_button_short_pressed_blocked = !enabled;
        }
        R_SUCCEED();
    }

    Result IOverlayFunctions::SetGpuTimeSliceBoost(u64 time_ns) {
        LOG_DEBUG(Service_AM, "called, time_ns={} (no GPU time slicing here)", time_ns);
        R_SUCCEED();
    }

    Result IOverlayFunctions::TerminateApplicationAndSetReason(u32 reason) {
        LOG_INFO(Service_AM, "called, reason={:#x}", reason);
        if (auto* window_system = system.GetAppletManager().GetWindowSystem()) {
            if (const auto application = window_system->GetMainApplet()) {
                LOG_INFO(Service_AM, "am: terminating pid {} (overlay, reason {:#x})",
                         application->aruid.pid, reason);
                application->process->Terminate();
            }
        }
        R_SUCCEED();
    }

    Result IOverlayFunctions::SetScreenShotPermissionGlobally(bool enabled) {
        LOG_DEBUG(Service_AM, "called, enabled={}", enabled);
        R_SUCCEED();
    }

    Result IOverlayFunctions::StartShutdownSequenceForOverlay() {
        LOG_INFO(Service_AM, "called");
        system.GetAppletManager().RequestPower(AppletManager::PowerRequest::Shutdown);
        R_SUCCEED();
    }

    Result IOverlayFunctions::StartRebootSequenceForOverlay() {
        LOG_INFO(Service_AM, "called");
        system.GetAppletManager().RequestPower(AppletManager::PowerRequest::Reboot);
        R_SUCCEED();
    }

    Result IOverlayFunctions::SetHealthWarningShowingState(bool showing) {
        LOG_DEBUG(Service_AM, "called, showing={}", showing);
        R_SUCCEED();
    }

    Result IOverlayFunctions::GetApplicationNintendoLogo() {
        // am answers 0x6880 when no application runs; the logo of a running title is not served.
        if (auto* window_system = system.GetAppletManager().GetWindowSystem();
            window_system && window_system->GetMainApplet()) {
            LOG_WARNING(Service_AM, "unknown contract: IOverlayFunctions 40 while a title runs; "
                                    "the logo is not served");
        }
        R_THROW(ResultNoApplicationLogo);
    }

    Result IOverlayFunctions::GetApplicationStartupMovie() {
        if (auto* window_system = system.GetAppletManager().GetWindowSystem();
            window_system && window_system->GetMainApplet()) {
            LOG_WARNING(Service_AM, "unknown contract: IOverlayFunctions 41 while a title runs; "
                                    "the movie is not served");
        }
        R_THROW(ResultNoApplicationLogo);
    }

    Result IOverlayFunctions::SetGpuTimeSliceBoostForApplication(u64 time_ns) {
        LOG_DEBUG(Service_AM, "called, time_ns={}", time_ns);
        R_SUCCEED();
    }

    Result IOverlayFunctions::Unknown60(bool value) {
        LOG_WARNING(Service_AM, "unknown contract: IOverlayFunctions 60 ({}) recorded only", value);
        R_SUCCEED();
    }

    Result IOverlayFunctions::Unknown71() {
        LOG_WARNING(Service_AM, "unknown contract: IOverlayFunctions 71 has no effect here");
        R_SUCCEED();
    }

    Result IOverlayFunctions::Unknown75() {
        // am fails with 0xc80 when the value it returns is not positive.
        LOG_WARNING(Service_AM, "unknown contract: IOverlayFunctions 75 answered with the "
                                "firmware's own failure (0xc80)");
        R_THROW(ResultUnknown75Unavailable);
    }

    Result IOverlayFunctions::SetRequiresGpuResourceUse(bool enabled) {
        LOG_DEBUG(Service_AM, "called, enabled={}", enabled);
        R_SUCCEED();
    }

    Result IOverlayFunctions::BeginToObserveHidInputForDevelop() {
        LOG_DEBUG(Service_AM, "called");
        R_SUCCEED();
    }

    Result IOverlayFunctions::Unknown70() {
        LOG_UNKNOWN_CONTRACT(Service_AM, "IOverlayFunctions 70: accepted, no effect here (the "
                                         "command is not documented)");
        R_SUCCEED();
    }
} // namespace Service::AM

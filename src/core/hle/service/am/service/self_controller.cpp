// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/logging.h"
#include "core/hle/result.h"
#include "core/hle/service/am/am_results.h"
#include "core/hle/service/am/frontend/applets.h"
#include "core/hle/service/am/service/self_controller.h"
#include "core/hle/service/caps/caps_su.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/nvnflinger/nvnflinger.h"
#include "core/hle/service/omm/idle_policy.h"
#include "core/hle/service/sm/sm.h"
#include "core/hle/service/vi/vi_results.h"

namespace Service::AM {

ISelfController::ISelfController(Core::System& system_, std::shared_ptr<Applet> applet,
                                 Kernel::KProcess* process)
    : ServiceFramework{system_, "ISelfController"}, m_process{process}, m_applet{
                                                                            std::move(applet)} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, D<&ISelfController::Exit>, "Exit"},
        {1, D<&ISelfController::LockExit>, "LockExit"},
        {2, D<&ISelfController::UnlockExit>, "UnlockExit"},
        {3, D<&ISelfController::EnterFatalSection>, "EnterFatalSection"},
        {4, D<&ISelfController::LeaveFatalSection>, "LeaveFatalSection"},
        {9, D<&ISelfController::GetLibraryAppletLaunchableEvent>, "GetLibraryAppletLaunchableEvent"},
        {10, D<&ISelfController::SetScreenShotPermission>, "SetScreenShotPermission"},
        {11, D<&ISelfController::SetOperationModeChangedNotification>, "SetOperationModeChangedNotification"},
        {12, D<&ISelfController::SetPerformanceModeChangedNotification>, "SetPerformanceModeChangedNotification"},
        {13, D<&ISelfController::SetFocusHandlingMode>, "SetFocusHandlingMode"},
        {14, D<&ISelfController::SetRestartMessageEnabled>, "SetRestartMessageEnabled"},
        {15, D<&ISelfController::SetScreenShotAppletIdentityInfo>, "SetScreenShotAppletIdentityInfo"},
        {16, D<&ISelfController::SetOutOfFocusSuspendingEnabled>, "SetOutOfFocusSuspendingEnabled"},
        {17, nullptr, "SetControllerFirmwareUpdateSection"},
        {18, nullptr, "SetRequiresCaptureButtonShortPressedMessage"},
        {19, D<&ISelfController::SetAlbumImageOrientation>, "SetAlbumImageOrientation"},
        {20, D<&ISelfController::SetDesirableKeyboardLayout>, "SetDesirableKeyboardLayout"},
        {21, nullptr, "GetScreenShotProgramId"},
        {40, D<&ISelfController::CreateManagedDisplayLayer>, "CreateManagedDisplayLayer"},
        {41, D<&ISelfController::IsSystemBufferSharingEnabled>, "IsSystemBufferSharingEnabled"},
        {42, D<&ISelfController::GetSystemSharedLayerHandle>, "GetSystemSharedLayerHandle"},
        {43, D<&ISelfController::GetSystemSharedBufferHandle>, "GetSystemSharedBufferHandle"},
        {44, D<&ISelfController::CreateManagedDisplaySeparableLayer>, "CreateManagedDisplaySeparableLayer"},
        {45, nullptr, "SetManagedDisplayLayerSeparationMode"},
        {46, nullptr, "SetRecordingLayerCompositionEnabled"},
        {50, D<&ISelfController::SetHandlesRequestToDisplay>, "SetHandlesRequestToDisplay"},
        {51, D<&ISelfController::ApproveToDisplay>, "ApproveToDisplay"},
        {60, D<&ISelfController::OverrideAutoSleepTimeAndDimmingTime>, "OverrideAutoSleepTimeAndDimmingTime"},
        {61, D<&ISelfController::SetMediaPlaybackState>, "SetMediaPlaybackState"},
        {62, D<&ISelfController::SetIdleTimeDetectionExtension>, "SetIdleTimeDetectionExtension"},
        {63, D<&ISelfController::GetIdleTimeDetectionExtension>, "GetIdleTimeDetectionExtension"},
        {64, nullptr, "SetInputDetectionSourceSet"},
        {65, D<&ISelfController::ReportUserIsActive>, "ReportUserIsActive"},
        {66, nullptr, "GetCurrentIlluminance"},
        {67, D<&ISelfController::IsIlluminanceAvailable>, "IsIlluminanceAvailable"},
        {68, D<&ISelfController::SetAutoSleepDisabled>, "SetAutoSleepDisabled"},
        {69, D<&ISelfController::IsAutoSleepDisabled>, "IsAutoSleepDisabled"},
        {70, nullptr, "ReportMultimediaError"},
        {71, nullptr, "GetCurrentIlluminanceEx"},
        {72, D<&ISelfController::SetInputDetectionPolicy>, "SetInputDetectionPolicy"},
        {80, nullptr, "SetWirelessPriorityMode"},
        {90, D<&ISelfController::GetAccumulatedSuspendedTickValue>, "GetAccumulatedSuspendedTickValue"},
        {91, D<&ISelfController::GetAccumulatedSuspendedTickChangedEvent>, "GetAccumulatedSuspendedTickChangedEvent"},
        {100, D<&ISelfController::SetAlbumImageTakenNotificationEnabled>, "SetAlbumImageTakenNotificationEnabled"},
        {110, nullptr, "SetApplicationAlbumUserData"},
        {120, D<&ISelfController::SaveCurrentScreenshot>, "SaveCurrentScreenshot"},
        {130, D<&ISelfController::SetRecordVolumeMuted>, "SetRecordVolumeMuted"},
        {230, D<&ISelfController::Unknown230>, "Unknown230"},
        {240, D<&ISelfController::Unknown240>, "Unknown240"},
        {1000, nullptr, "GetDebugStorageChannel"},
    };
    // clang-format on

    RegisterHandlers(functions);

    std::scoped_lock lk{m_applet->lock};
    m_applet->display_layer_manager.Initialize(system, m_process, m_applet->applet_id,
                                               m_applet->library_applet_mode);
}

ISelfController::~ISelfController() {
    std::scoped_lock lk{m_applet->lock};
    m_applet->display_layer_manager.Finalize();
}

Result ISelfController::Exit() {
    LOG_DEBUG(Service_AM, "called");

    m_applet->process->Terminate();

    R_SUCCEED();
}

Result ISelfController::LockExit() {
    LOG_DEBUG(Service_AM, "called");

    std::scoped_lock lk{m_applet->lock};

    if (m_applet->lifecycle_manager.GetExitRequested()) {
        // With exit already requested, ignore and terminate immediately.
        m_applet->process->Terminate();
    } else {
        // Otherwise, set exit lock state.
        m_applet->exit_locked = true;
        system.SetExitLocked(true);
    }

    R_SUCCEED();
}

Result ISelfController::UnlockExit() {
    LOG_DEBUG(Service_AM, "called");

    std::scoped_lock lk{m_applet->lock};

    m_applet->exit_locked = false;
    system.SetExitLocked(false);

    if (m_applet->lifecycle_manager.GetExitRequested()) {
        m_applet->process->Terminate();
    }

    R_SUCCEED();
}

Result ISelfController::EnterFatalSection() {
    std::scoped_lock lk{m_applet->lock};

    m_applet->fatal_section_count++;
    LOG_DEBUG(Service_AM, "called. Num fatal sections entered: {}", m_applet->fatal_section_count);

    R_SUCCEED();
}

Result ISelfController::LeaveFatalSection() {
    LOG_DEBUG(Service_AM, "called");

    // Entry and exit of fatal sections must be balanced.
    std::scoped_lock lk{m_applet->lock};
    R_UNLESS(m_applet->fatal_section_count > 0, AM::ResultFatalSectionCountImbalance);
    m_applet->fatal_section_count--;

    R_SUCCEED();
}

Result ISelfController::GetLibraryAppletLaunchableEvent(
    OutCopyHandle<Kernel::KReadableEvent> out_event) {
    LOG_WARNING(Service_AM, "(STUBBED) called");

    m_applet->library_applet_launchable_event.Signal(system.Kernel());
    *out_event = m_applet->library_applet_launchable_event.GetHandle();

    R_SUCCEED();
}

Result ISelfController::SetDesirableKeyboardLayout(u32 layout) {
    LOG_INFO(Service_AM, "called, layout={}", layout);
    std::scoped_lock lk{m_applet->lock};
    m_applet->desirable_keyboard_layout = layout;
    R_SUCCEED();
}

Result ISelfController::SetScreenShotPermission(ScreenshotPermission screen_shot_permission) {
    LOG_DEBUG(Service_AM, "called, permission={}", screen_shot_permission);

    std::scoped_lock lk{m_applet->lock};
    m_applet->screenshot_permission = screen_shot_permission;

    R_SUCCEED();
}

Result ISelfController::SetOperationModeChangedNotification(bool enabled) {
    LOG_INFO(Service_AM, "called, enabled={}", enabled);

    std::scoped_lock lk{m_applet->lock};
    m_applet->lifecycle_manager.SetOperationModeChangedNotificationEnabled(system.Kernel(), enabled);

    R_SUCCEED();
}

Result ISelfController::SetPerformanceModeChangedNotification(bool enabled) {
    LOG_INFO(Service_AM, "called, enabled={}", enabled);

    std::scoped_lock lk{m_applet->lock};
    m_applet->lifecycle_manager.SetPerformanceModeChangedNotificationEnabled(system.Kernel(), enabled);

    R_SUCCEED();
}

Result ISelfController::SetFocusHandlingMode(bool notify, bool background, bool suspend) {
    LOG_INFO(Service_AM, "called, notify={} background={} suspend={}", notify, background, suspend);

    std::scoped_lock lk{m_applet->lock};
    m_applet->lifecycle_manager.SetFocusStateChangedNotificationEnabled(system.Kernel(), notify);
    m_applet->lifecycle_manager.SetFocusHandlingMode(suspend);
    m_applet->UpdateSuspensionStateLocked(true);

    R_SUCCEED();
}

Result ISelfController::SetRestartMessageEnabled(bool enabled) {
    LOG_INFO(Service_AM, "called, enabled={}", enabled);

    std::scoped_lock lk{m_applet->lock};
    m_applet->lifecycle_manager.SetResumeNotificationEnabled(enabled);

    R_SUCCEED();
}

Result ISelfController::SetScreenShotAppletIdentityInfo(
    AppletIdentityInfo screen_shot_applet_identity_info) {
    LOG_WARNING(Service_AM, "(STUBBED) called");

    std::scoped_lock lk{m_applet->lock};
    m_applet->screen_shot_identity = screen_shot_applet_identity_info;

    R_SUCCEED();
}

Result ISelfController::SetOutOfFocusSuspendingEnabled(bool enabled) {
    LOG_INFO(Service_AM, "called, enabled={}", enabled);

    std::scoped_lock lk{m_applet->lock};
    m_applet->lifecycle_manager.SetOutOfFocusSuspendingEnabled(enabled);
    m_applet->UpdateSuspensionStateLocked(false);

    R_SUCCEED();
}

Result ISelfController::SetAlbumImageOrientation(
    Capture::AlbumImageOrientation album_image_orientation) {
    LOG_WARNING(Service_AM, "(STUBBED) called, orientation={}", album_image_orientation);

    std::scoped_lock lk{m_applet->lock};
    m_applet->album_image_orientation = album_image_orientation;

    R_SUCCEED();
}

Result ISelfController::IsSystemBufferSharingEnabled() {
    LOG_INFO(Service_AM, "called");

    std::scoped_lock lk{m_applet->lock};
    R_RETURN(m_applet->display_layer_manager.IsSystemBufferSharingEnabled());
}

Result ISelfController::GetSystemSharedBufferHandle(Out<u64> out_buffer_id) {
    LOG_INFO(Service_AM, "called");

    u64 layer_id;

    std::scoped_lock lk{m_applet->lock};
    R_RETURN(m_applet->display_layer_manager.GetSystemSharedLayerHandle(out_buffer_id, &layer_id));
}

Result ISelfController::GetSystemSharedLayerHandle(Out<u64> out_buffer_id, Out<u64> out_layer_id) {
    LOG_INFO(Service_AM, "called");

    std::scoped_lock lk{m_applet->lock};
    R_RETURN(
        m_applet->display_layer_manager.GetSystemSharedLayerHandle(out_buffer_id, out_layer_id));
}

Result ISelfController::CreateManagedDisplayLayer(Out<u64> out_layer_id) {
    LOG_INFO(Service_AM, "called");

    std::scoped_lock lk{m_applet->lock};
    R_RETURN(m_applet->display_layer_manager.CreateManagedDisplayLayer(out_layer_id));
}

Result ISelfController::CreateManagedDisplaySeparableLayer(Out<u64> out_layer_id,
                                                           Out<u64> out_recording_layer_id) {
    LOG_WARNING(Service_AM, "(STUBBED) called");

    std::scoped_lock lk{m_applet->lock};
    R_RETURN(m_applet->display_layer_manager.CreateManagedDisplaySeparableLayer(
        out_layer_id, out_recording_layer_id));
}

Result ISelfController::SetHandlesRequestToDisplay(bool enable) {
    LOG_WARNING(Service_AM, "(STUBBED) called, enable={}", enable);
    R_SUCCEED();
}

Result ISelfController::ApproveToDisplay() {
    LOG_WARNING(Service_AM, "(STUBBED) called");
    R_SUCCEED();
}

Result ISelfController::SetMediaPlaybackState(bool state) {
    LOG_DEBUG(Service_AM, "called, state={}", state);
    // idle's media timings (Sleep Mode's "while playing media") follow it.
    std::scoped_lock lk{m_applet->lock};
    m_applet->media_playback_state = state;
    R_SUCCEED();
}

// Idle detection, auto-dimming and auto-sleep: idle:sys (omm, IdlePolicy) dims the screen and then
// asks HOME to sleep after the times in System Settings, which these commands override or extend
// while the applet is in front; am reads them back from the applet in front for the timer.
Result ISelfController::OverrideAutoSleepTimeAndDimmingTime(s32 auto_sleep_handheld,
                                                            s32 auto_sleep_console,
                                                            s32 dimming_handheld,
                                                            s32 dimming_console) {
    LOG_DEBUG(Service_AM, "called, sleep {}/{} s, dimming {}/{} s (handheld/console)",
              auto_sleep_handheld, auto_sleep_console, dimming_handheld, dimming_console);

    std::scoped_lock lk{m_applet->lock};
    m_applet->auto_sleep_time_override = {auto_sleep_handheld, auto_sleep_console,
                                          dimming_handheld, dimming_console};
    R_SUCCEED();
}

Result ISelfController::SetIdleTimeDetectionExtension(
    IdleTimeDetectionExtension idle_time_detection_extension) {
    LOG_DEBUG(Service_AM, "called extension={}", idle_time_detection_extension);

    std::scoped_lock lk{m_applet->lock};
    m_applet->idle_time_detection_extension = idle_time_detection_extension;

    R_SUCCEED();
}

Result ISelfController::GetIdleTimeDetectionExtension(
    Out<IdleTimeDetectionExtension> out_idle_time_detection_extension) {
    LOG_DEBUG(Service_AM, "called");

    std::scoped_lock lk{m_applet->lock};
    *out_idle_time_detection_extension = m_applet->idle_time_detection_extension;

    R_SUCCEED();
}

Result ISelfController::ReportUserIsActive() {
    // Restarts the idle timer (idle:sys 5 on a console).
    LOG_DEBUG(Service_AM, "called");

    {
        std::scoped_lock lk{m_applet->lock};
        m_applet->last_user_activity = std::chrono::steady_clock::now();
    }
    if (auto* const idle = OMM::GetIdlePolicy()) {
        idle->ReportUserIsActive();
    }
    R_SUCCEED();
}

Result ISelfController::SetAutoSleepDisabled(bool is_auto_sleep_disabled) {
    LOG_DEBUG(Service_AM, "called. is_auto_sleep_disabled={}", is_auto_sleep_disabled);

    // On the system itself, if the previous state of is_auto_sleep_disabled
    // differed from the current value passed in, it'd signify the internal
    // window manager to update (and also increment some statistics like update counts)
    //
    // It'd also indicate this change to an idle handling context.
    //
    // However, given we're emulating this behavior, most of this can be ignored
    // and it's sufficient to simply set the member variable for querying via
    // IsAutoSleepDisabled().

    std::scoped_lock lk{m_applet->lock};
    m_applet->auto_sleep_disabled = is_auto_sleep_disabled;

    R_SUCCEED();
}

Result ISelfController::IsAutoSleepDisabled(Out<bool> out_is_auto_sleep_disabled) {
    LOG_DEBUG(Service_AM, "called.");

    std::scoped_lock lk{m_applet->lock};
    *out_is_auto_sleep_disabled = m_applet->auto_sleep_disabled;

    R_SUCCEED();
}

Result ISelfController::IsIlluminanceAvailable(Out<bool> out_is_illuminance_available) {
    LOG_WARNING(Service_AM, "(stubbed)");
    *out_is_illuminance_available = false;
    R_SUCCEED();
}

Result ISelfController::SetInputDetectionPolicy(InputDetectionPolicy input_detection_policy) {
    LOG_WARNING(Service_AM, "(STUBBED) called");
    R_SUCCEED();
}

Result ISelfController::GetAccumulatedSuspendedTickValue(
    Out<u64> out_accumulated_suspended_tick_value) {
    LOG_DEBUG(Service_AM, "called.");

    // This command returns the total number of system ticks since ISelfController creation
    // where the game was suspended. Since Yuzu doesn't implement game suspension, this command
    // can just always return 0 ticks.
    std::scoped_lock lk{m_applet->lock};
    *out_accumulated_suspended_tick_value = m_applet->suspended_ticks;

    R_SUCCEED();
}

Result ISelfController::GetAccumulatedSuspendedTickChangedEvent(
    OutCopyHandle<Kernel::KReadableEvent> out_event) {
    LOG_DEBUG(Service_AM, "called.");

    *out_event = m_applet->accumulated_suspended_tick_changed_event.GetHandle();
    R_SUCCEED();
}

Result ISelfController::SetAlbumImageTakenNotificationEnabled(bool enabled) {
    LOG_WARNING(Service_AM, "(STUBBED) called. enabled={}", enabled);

    // This service call sets an internal flag whether a notification is shown when an image is
    // captured. Currently we do not support capturing images via the capture button, so this can be
    // stubbed for now.
    std::scoped_lock lk{m_applet->lock};
    m_applet->album_image_taken_notification_enabled = enabled;

    R_SUCCEED();
}

Result ISelfController::SaveCurrentScreenshot(Capture::AlbumReportOption album_report_option) {
    LOG_INFO(Service_AM, "called, report_option={}", album_report_option);

    const auto screenshot_service =
        system.ServiceManager().GetService<Service::Capture::IScreenShotApplicationService>(
            "caps:su");

    if (screenshot_service) {
        screenshot_service->CaptureAndSaveScreenshot(album_report_option);
    }

    R_SUCCEED();
}

Result ISelfController::SetRecordVolumeMuted(bool muted) {
    LOG_WARNING(Service_AM, "(STUBBED) called. muted={}", muted);

    std::scoped_lock lk{m_applet->lock};
    m_applet->record_volume_muted = muted;

    R_SUCCEED();
}

Result ISelfController::Unknown230(u32 a, u32 b) {
    // qlaunch 22.5.0 sends (2, 2) when it opens a bottom-row destination; the firmware answers
    // with no output (Ryujinx Harbor's reading).
    LOG_WARNING(Service_AM, "unknown contract: ISelfController 230 ({}, {}); returning success", a,
                b);
    R_SUCCEED();
}

Result ISelfController::Unknown240(s32 value) {
    // am 23.0.1 checks the signed input's upper bound (3) and only logs it; NetConnect sends 0.
    LOG_INFO(Service_AM, "ISelfController 240 diagnostic value {}", value);
    R_UNLESS(value <= 3, ResultInvalidParameters);
    R_SUCCEED();
}

} // namespace Service::AM

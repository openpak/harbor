// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <deque>
#include <mutex>
#include <stack>

#include "common/math_util.h"
#include "core/hle/service/apm/apm_controller.h"
#include "core/hle/service/caps/caps_types.h"
#include "core/hle/service/cmif_types.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/os/event.h"
#include "core/hle/service/os/process.h"
#include "core/hle/service/service.h"

#include "core/hle/service/am/am_types.h"
#include "core/hle/service/am/display_layer_manager.h"
#include "core/hle/service/am/hid_registration.h"
#include "core/hle/service/am/lifecycle_manager.h"
#include "core/hle/service/am/process_holder.h"
#include "core/hle/service/am/service/storage.h"

namespace Service::AM {

struct Applet {
    explicit Applet(Core::System& system_, std::unique_ptr<Process> process_, bool is_application);
    ~Applet();

    Core::System& system;

    // Lock
    std::mutex lock{};

    // Event creation helper
    KernelHelpers::ServiceContext context;

    // Lifecycle manager
    LifecycleManager lifecycle_manager;

    // Process
    std::unique_ptr<Process> process;
    std::optional<ProcessHolder> process_holder;
    bool is_process_running{};

    // Creation state
    AppletId applet_id{};
    AppletResourceUserId aruid{};
    AppletProcessLaunchReason launch_reason{};
    AppletType type{};
    ProgramId program_id{};
    LibraryAppletMode library_applet_mode{};
    s32 previous_program_index{-1};
    ScreenshotPermission previous_screenshot_permission{ScreenshotPermission::Enable};

    // TODO: some fields above can be AppletIdentityInfo
    AppletIdentityInfo screen_shot_identity;

    // hid state
    HidRegistration hid_registration;

    // vi state
    DisplayLayerManager display_layer_manager{};

    // Applet common functions
    Result terminate_result{};
    s32 display_logical_width{};
    s32 display_logical_height{};
    Common::Rectangle<f32> display_magnification{0, 0, 1, 1};
    bool home_button_double_click_enabled{};
    bool home_button_short_pressed_blocked{};
    bool home_button_long_pressed_blocked{};
    bool vr_mode_curtain_required{};
    bool sleep_required_by_high_temperature{};
    bool sleep_required_by_low_battery{};
    s32 cpu_boost_request_priority{-1};
    bool handling_capture_button_short_pressed_message_enabled_for_applet{};
    bool handling_capture_button_long_pressed_message_enabled_for_applet{};
    u32 application_core_usage_mode{};

    // Application functions
    bool game_play_recording_supported{};
    bool media_playback_state{};
    GamePlayRecordingState game_play_recording_state{GamePlayRecordingState::Disabled};
    bool jit_service_launched{};
    bool application_crash_report_enabled{};

    // Common state
    bool sleep_lock_enabled{};
    bool vr_mode_enabled{};
    bool vr_mode_enabled_3d{};
    bool lcd_backlight_off_enabled{};
    APM::CpuBoostMode boost_mode{};
    bool request_exit_to_library_applet_at_execute_next_program_enabled{};

    // Channels
    std::vector<std::vector<u8>> user_channel_launch_parameter{};
    std::vector<std::vector<u8>> preselected_user_launch_parameter{};
    std::vector<std::vector<u8>> friend_invitation_storage_channel{};

    // Context Stack
    std::stack<SharedPointer<IStorage>> context_stack{};

    // Caller applet
    std::weak_ptr<Applet> caller_applet{};
    std::shared_ptr<AppletDataBroker> caller_applet_broker{};
    std::list<std::shared_ptr<Applet>> child_applets{};
    bool is_completed{};

    // Open ILibraryAppletAccessor objects for this applet.
    std::atomic<u32> accessor_count{};

    std::shared_ptr<Applet> reserved_applet{};
    bool unwind_after_reserved{};
    bool is_winding{};
    // The ended process has been let go as soon as am saw it end, so its memory is back in the
    // pool at once, as on the console, not when the last reference to this applet goes (a winding
    // Shop kept its whole web heap while playerSelect and Login ran). Unwinding loads it again.
    bool process_released{};

    // Self state
    bool exit_locked{};
    s32 fatal_section_count{};
    Capture::AlbumImageOrientation album_image_orientation{};
    bool handles_request_to_display{};
    ScreenshotPermission screenshot_permission{};
    IdleTimeDetectionExtension idle_time_detection_extension{};
    bool auto_sleep_disabled{};
    u64 suspended_ticks{};
    bool album_image_taken_notification_enabled{};
    bool record_volume_muted{};
    bool is_activity_runnable{};
    bool is_pad_interactible{true};
    bool is_touch_interactible{true};
    bool input_state_applied{false};
    bool window_visible{true};
    bool overlay_watching_short_home_button{false};
    bool overlay_handling_touch_input{false};
    // HOME session: ICommonStateGetter 100, the applet takes the HOME short press itself while it
    // holds the foreground; IOverlayFunctions 20, the overlay keeps a short press it watches.
    bool handles_home_button_short_press{false};
    bool overlay_handles_home_button_short_press{false};

    // Events
    Event overlay_event;
    Event gpu_error_detected_event;
    Event friend_invitation_storage_channel_event;
    Event notification_storage_channel_event;
    Event health_warning_disappeared_system_event;
    Event unknown_event;
    Event acquired_sleep_lock_event;
    Event pop_from_general_channel_event;
    Event library_applet_launchable_event;
    Event accumulated_suspended_tick_changed_event;
    Event sleep_lock_event;
    Event state_changed_event;

    // Frontend state
    std::shared_ptr<Frontend::FrontendApplet> frontend{};

    // Process state management
    void UpdateSuspensionStateLocked(bool force_message);
    void SetInteractibleLocked(bool pad_interactible, bool touch_interactible);
    void OnProcessTerminatedLocked();
};

} // namespace Service::AM

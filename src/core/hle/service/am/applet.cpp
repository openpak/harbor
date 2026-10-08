// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator
// Project// SPDX-License-Identifier: GPL-2.0-or-later

#include "audio_core/audio_core.h"
#include "core/core.h"
#include "core/hle/service/am/applet.h"
#include "core/hle/service/am/applet_manager.h"

namespace Service::AM {

Applet::Applet(Core::System& system_, std::unique_ptr<Process> process_, bool is_application)
    : system(system_), context(system_, "Applet"), lifecycle_manager(system_, context, is_application),
      process(std::move(process_)), hid_registration(system_, *process), overlay_event(context),
      gpu_error_detected_event(context), friend_invitation_storage_channel_event(context),
      notification_storage_channel_event(context), health_warning_disappeared_system_event(context),
      unknown_event(context), acquired_sleep_lock_event(context), pop_from_general_channel_event(context),
      library_applet_launchable_event(context), accumulated_suspended_tick_changed_event(context),
      sleep_lock_event(context), state_changed_event(context) {

    aruid.pid = process->GetProcessId();
    program_id = process->GetProgramId();
}

Applet::~Applet() = default;

void Applet::UpdateSuspensionStateLocked(bool force_message) {
    // Remove any forced resumption.
    lifecycle_manager.RemoveForceResumeIfPossible();

	const bool update_requested_focus_state = lifecycle_manager.UpdateRequestedFocusState();
    const bool curr_activity_runnable = lifecycle_manager.IsRunnable();
    const bool prev_activity_runnable = is_activity_runnable;
    const bool was_changed = curr_activity_runnable != prev_activity_runnable;

    if (was_changed && process_exiting) {
        // Nothing to pause or resume: the process is ending.
    } else if (was_changed) {
        if (curr_activity_runnable) {
            process->Suspend(false);
        } else {
            process->Suspend(true);
			lifecycle_manager.RequestResumeNotification();
        }

        is_activity_runnable = curr_activity_runnable;

        // A paused title's audio renderers stop mixing too (else its last voices keep playing).
        if (lifecycle_manager.IsApplication() && system.GetAppletManager().IsHomeSession()) {
            system.AudioCore().SetSuspended(aruid.pid, !curr_activity_runnable);
            LOG_INFO(Service_AM, "am: application pid {} {}", aruid.pid,
                     curr_activity_runnable ? "resumed" : "suspended");
        }
    }

    if (lifecycle_manager.GetForcedSuspend()) {
        // TODO: why is this allowed?
        return;
    }

    // Signal if the focus state was changed or the process state was changed.
    if (update_requested_focus_state || was_changed || force_message) {
        lifecycle_manager.SignalSystemEventIfNeeded(context.kernel);
    }
}

void Applet::SetInteractibleLocked(bool pad_interactible, bool touch_interactible) {
    // The first state is always applied: hid keeps an aruid without input until told otherwise.
    if (input_state_applied && is_pad_interactible == pad_interactible &&
        is_touch_interactible == touch_interactible) {
        return;
    }
    input_state_applied = true;

    is_pad_interactible = pad_interactible;
    is_touch_interactible = touch_interactible;

    const bool exit_requested = lifecycle_manager.GetExitRequested();
    const bool pad_enabled = pad_interactible && !exit_requested;
    const bool touch_enabled = touch_interactible && !exit_requested;

    LOG_INFO(Service_AM, "am: input pid {} (applet {}) pad={} touch={}", aruid.pid,
             static_cast<u32>(applet_id), pad_enabled, touch_enabled);

    hid_registration.EnableAppletToGetInput(pad_enabled, touch_enabled);
}

void Applet::OnProcessTerminatedLocked() {
    is_completed = true;
    state_changed_event.Signal(context.kernel);
}

} // namespace Service::AM

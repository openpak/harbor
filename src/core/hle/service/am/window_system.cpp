// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <utility>

#include "common/settings.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/service/am/am_results.h"
#include "core/hle/service/am/applet.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/am/event_observer.h"
#include "core/hle/service/am/process_creation.h"
#include "core/hle/service/am/service/lock_accessor.h"
#include "core/hle/service/am/window_system.h"

namespace Service::AM {

namespace {
// How long a caller's ChangeIntoForeground waits for it to close a completed applet's accessor.
constexpr std::chrono::nanoseconds ForegroundHoldTimeout = std::chrono::seconds{1};
} // namespace

WindowSystem::WindowSystem(Core::System& system) : m_system(system) {
    m_foreground_hold_event = Core::Timing::CreateEvent(
        "am:ForegroundHold",
        [this](s64, std::chrono::nanoseconds) -> std::optional<std::chrono::nanoseconds> {
            this->ReleaseExpiredForegroundHolds();
            return std::nullopt;
        });
}

WindowSystem::~WindowSystem() {
    m_system.CoreTiming().UnscheduleEvent(m_foreground_hold_event);
    m_system.GetAppletManager().SetWindowSystem(nullptr);
}

void WindowSystem::SetEventObserver(EventObserver* observer) {
    m_event_observer = observer;
    m_system.GetAppletManager().SetWindowSystem(this);
}

void WindowSystem::RequestUpdate() {
    if (m_event_observer) {
        m_event_observer->RequestUpdate();
    }
}

void WindowSystem::Update() {
    {
        std::scoped_lock lk{m_lock};

        LOG_DEBUG(Service_AM, "called, home_menu={} application={} overlay={}",
                  m_home_menu != nullptr, m_application != nullptr, m_overlay_display != nullptr);

        // Loop through all applets and remove terminated applets.
        this->PruneTerminatedAppletsLocked();

        // Same wording as the Ryujinx Harbor's log, so the two diff side by side.
        const u64 foreground_pid =
            m_foreground_requested_applet ? m_foreground_requested_applet->aruid.pid : 0;
        if (foreground_pid != m_logged_foreground_pid) {
            LOG_INFO(Service_AM, "am: foreground pid {} -> pid {}",
                     m_logged_foreground_pid ? fmt::format("{}", m_logged_foreground_pid) : "none",
                     foreground_pid ? fmt::format("{}", foreground_pid) : "none");
            m_logged_foreground_pid = foreground_pid;
        }

        // If the home menu is being locked into the foreground, handle that.
        if (!this->LockHomeMenuIntoForegroundLocked()) {
            const bool overlay_takes_input = this->DoesOverlayTakeInputLocked();

            this->UpdateAppletStateLocked(m_overlay_display, true, overlay_takes_input);
            this->UpdateAppletStateLocked(m_home_menu, m_foreground_requested_applet == m_home_menu, overlay_takes_input);
            this->UpdateAppletStateLocked(m_application, m_foreground_requested_applet == m_application, overlay_takes_input);
        }
    }

    this->NotifyApplicationChangedIfNeeded();
}

bool WindowSystem::TrackApplet(std::shared_ptr<Applet> applet, bool is_application) {
    {
        std::scoped_lock lk{m_lock};

        if (applet->applet_id == AppletId::QLaunch) {
            if (m_home_menu != nullptr) {
                LOG_ERROR(Service_AM, "am: a home menu is already tracked (pid {}); pid {} refused",
                          m_home_menu->aruid.pid, applet->aruid.pid);
                return false;
            }
            m_home_menu = applet.get();
        } else if (applet->applet_id == AppletId::OverlayDisplay) {
            m_overlay_display = applet.get();
        } else if (is_application) {
            if (m_application != nullptr) {
                LOG_WARNING(Service_AM,
                            "am: application pid {} ({:016x}) has not completed; pid {} refused",
                            m_application->aruid.pid, m_application->program_id,
                            applet->aruid.pid);
                return false;
            }
            m_application = applet.get();
        }

        this->UpdateCurrentApplicationLocked();

        m_event_observer->TrackAppletProcess(*applet);
        m_applets.emplace(applet->aruid.pid, std::move(applet));
    }

    this->NotifyApplicationChangedIfNeeded();
    return true;
}

bool WindowSystem::HasApplication() {
    std::scoped_lock lk{m_lock};
    return m_application != nullptr;
}

void WindowSystem::UpdateCurrentApplicationLocked() {
    const Applet* const candidate = m_application != nullptr ? m_application : m_home_menu;
    if (candidate == nullptr) {
        return;
    }

    auto* const process = candidate->process->GetHandle();
    if (process == nullptr || process == m_system.Kernel().ApplicationProcess()) {
        return;
    }

    LOG_INFO(Service_AM, "Current application is now {:016X}", candidate->program_id);

    m_system.Kernel().SetApplicationProcess(process);
    Settings::SetCurrentProgramID(candidate->program_id);

    m_pending_application_notification = candidate->program_id;
}

void WindowSystem::NotifyApplicationChangedIfNeeded() {
    std::optional<u64> program_id;
    {
        std::scoped_lock lk{m_lock};
        program_id = std::exchange(m_pending_application_notification, std::nullopt);
    }

    if (!program_id.has_value()) {
        return;
    }

    m_system.NotifyApplicationChanged(*program_id);
}

std::shared_ptr<Applet> WindowSystem::GetByAppletResourceUserId(u64 aruid) {
    std::scoped_lock lk{m_lock};

    const auto it = m_applets.find(aruid);
    if (it == m_applets.end()) {
        return nullptr;
    }

    return it->second;
}

std::shared_ptr<Applet> WindowSystem::GetMainApplet() {
    std::scoped_lock lk{m_lock};

    if (m_application) {
        return m_applets.at(m_application->aruid.pid);
    }

    return nullptr;
}

std::shared_ptr<Applet> WindowSystem::GetOverlayDisplayApplet() {
    std::scoped_lock lk{m_lock};

    if (m_overlay_display) {
        return m_applets.at(m_overlay_display->aruid.pid);
    }

    return nullptr;
}

void WindowSystem::RequestHomeMenuToGetForeground() {
    {
        std::scoped_lock lk{m_lock};
        m_foreground_requested_applet = m_home_menu;
    }

    m_event_observer->RequestUpdate();
}

void WindowSystem::RequestApplicationToGetForeground() {
    {
        std::scoped_lock lk{m_lock};
        m_foreground_requested_applet = m_application;
    }

    m_event_observer->RequestUpdate();
}

void WindowSystem::RequestLockHomeMenuIntoForeground() {
    {
        std::scoped_lock lk{m_lock};
        m_home_menu_foreground_locked = true;
    }

    m_event_observer->RequestUpdate();
}

void WindowSystem::RequestUnlockHomeMenuIntoForeground() {
    {
        std::scoped_lock lk{m_lock};
        m_home_menu_foreground_locked = false;
    }

    m_event_observer->RequestUpdate();
}

void WindowSystem::RequestAppletVisibilityState(Applet& applet, bool visible) {
    {
        std::scoped_lock lk{applet.lock};
        applet.window_visible = visible;
    }

    m_event_observer->RequestUpdate();
}

void WindowSystem::OnOperationModeChanged() {
    std::scoped_lock lk{m_lock};

    for (const auto& [aruid, applet] : m_applets) {
        std::scoped_lock lk2{applet->lock};
        applet->lifecycle_manager.OnOperationAndPerformanceModeChanged(m_system.Kernel());
    }
}

void WindowSystem::OnExitRequested() {
    std::scoped_lock lk{m_lock};

    for (const auto& [aruid, applet] : m_applets) {
        std::scoped_lock lk2{applet->lock};
        applet->lifecycle_manager.RequestExit(m_system.Kernel());
    }
}

void WindowSystem::SendButtonAppletMessageLocked(AppletMessage message) {
    const auto is_blocked = [message](const Applet& applet) {
        if (message == AppletMessage::DetectShortPressingHomeButton &&
            applet.applet_id == AppletId::OverlayDisplay &&
            !applet.overlay_watching_short_home_button) {
            return true;
        }

        switch (message) {
        case AppletMessage::DetectShortPressingHomeButton:
            return applet.home_button_short_pressed_blocked;
        case AppletMessage::DetectLongPressingHomeButton:
            return applet.home_button_long_pressed_blocked;
        default:
            return false;
        }
    };

    const auto send_to = [&](Applet* applet) {
        if (!applet) {
            return;
        }
        std::scoped_lock lk{applet->lock};
        if (is_blocked(*applet)) {
            LOG_DEBUG(Service_AM, "Applet {} is blocking message {}",
                      static_cast<u32>(applet->applet_id), static_cast<u32>(message));
            return;
        }
        applet->lifecycle_manager.PushUnorderedMessage(m_system.Kernel(), message);
    };

    send_to(m_home_menu);
    send_to(m_overlay_display);
    send_to(m_application);

    if (m_event_observer) {
        m_event_observer->RequestUpdate();
    }
}

void WindowSystem::PushButtonMessageLocked(Applet* applet, AppletMessage message) {
    if (applet == nullptr) {
        return;
    }
    {
        std::scoped_lock lk{applet->lock};
        applet->lifecycle_manager.PushUnorderedMessage(m_system.Kernel(), message);
    }
    LOG_DEBUG(Service_AM, "am: -> pid {} {}", applet->aruid.pid, static_cast<u32>(message));
    if (m_event_observer) {
        m_event_observer->RequestUpdate();
    }
}

// HOME session routing. Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base): HOME
// short goes to the HOME menu, or to the foreground applet that takes it (ICommonStateGetter 100),
// with a watching overlay first; HOME long and the power middle press go to the overlay only; the
// application never receives them.
void WindowSystem::RouteSystemButtonLocked(SystemButtonType type) {
    Applet* const foreground = m_foreground_requested_applet;
    const auto foreground_application_blocks = [&](bool long_press) {
        if (foreground == nullptr || foreground != m_application) {
            return false;
        }
        std::scoped_lock lk{foreground->lock};
        return long_press ? foreground->home_button_long_pressed_blocked
                          : foreground->home_button_short_pressed_blocked;
    };

    switch (type) {
    case SystemButtonType::HomeButtonShortPressing: {
        if (m_home_menu == nullptr) {
            LOG_INFO(Service_AM, "am: HOME short press ignored: no home menu is running");
            return;
        }

        if (m_overlay_display != nullptr) {
            bool watching, keeps;
            {
                std::scoped_lock lk{m_overlay_display->lock};
                watching = m_overlay_display->overlay_watching_short_home_button;
                keeps = m_overlay_display->overlay_handles_home_button_short_press;
            }
            if (watching) {
                this->PushButtonMessageLocked(m_overlay_display,
                                              AppletMessage::DetectShortPressingHomeButton);
                if (keeps) {
                    return;
                }
            }
        }

        if (foreground_application_blocks(false)) {
            // TODO(ovln): the overlay's "HOME button cannot be used now" notice.
            LOG_INFO(Service_AM, "am: HOME short press blocked by the foreground application pid {}",
                     foreground->aruid.pid);
            return;
        }

        Applet* target = m_home_menu;
        if (foreground != nullptr && foreground->applet_id != AppletId::Application) {
            std::scoped_lock lk{foreground->lock};
            if (foreground->handles_home_button_short_press) {
                target = foreground;
            }
        }
        this->PushButtonMessageLocked(target, AppletMessage::DetectShortPressingHomeButton);
        return;
    }
    case SystemButtonType::HomeButtonLongPressing:
        if (m_overlay_display == nullptr) {
            LOG_INFO(Service_AM, "am: HOME long press ignored: no overlay applet is running");
            return;
        }
        if (foreground_application_blocks(true)) {
            LOG_INFO(Service_AM, "am: HOME long press blocked by the foreground application pid {}",
                     foreground->aruid.pid);
            return;
        }
        this->PushButtonMessageLocked(m_overlay_display,
                                      AppletMessage::DetectLongPressingHomeButton);
        return;
    case SystemButtonType::PowerButtonShortPressing:
    case SystemButtonType::PowerButtonLongPressing:
        if (m_overlay_display == nullptr) {
            LOG_INFO(Service_AM, "am: power button ignored: no overlay applet is running");
            return;
        }
        this->PushButtonMessageLocked(m_overlay_display,
                                      AppletMessage::DetectMiddlePressingPowerButton);
        return;
    case SystemButtonType::CaptureButtonShortPressing:
    case SystemButtonType::CaptureButtonLongPressing: {
        // Only an applet that asked for the capture button hears it; otherwise the press is a
        // screenshot, which is vi/caps work.
        const bool is_long = type == SystemButtonType::CaptureButtonLongPressing;
        bool sent = false;
        for (const auto& [aruid, applet] : m_applets) {
            bool wants;
            {
                std::scoped_lock lk{applet->lock};
                wants = is_long
                            ? applet->handling_capture_button_long_pressed_message_enabled_for_applet
                            : applet->handling_capture_button_short_pressed_message_enabled_for_applet;
            }
            if (wants) {
                this->PushButtonMessageLocked(applet.get(),
                                              is_long ? AppletMessage::DetectLongPressingCaptureButton
                                                      : AppletMessage::DetectShortPressingCaptureButton);
                sent = true;
            }
        }
        if (!sent) {
            LOG_INFO(Service_AM, "am: capture button {} press: no applet handles it",
                     is_long ? "long" : "short");
        }
        return;
    }
    default:
        return;
    }
}

std::shared_ptr<LockState> WindowSystem::GetLockState(u32 button_type) {
    std::scoped_lock lk{m_lock};
    auto& state = m_lock_states[button_type];
    if (!state) {
        state = std::make_shared<LockState>(m_system);
    }
    return state;
}

bool WindowSystem::IsHomeButtonShortPressBlocked() {
    std::scoped_lock lk{m_lock};
    Applet* const foreground = m_foreground_requested_applet;
    if (foreground == nullptr || foreground != m_application) {
        return false;
    }
    std::scoped_lock lk2{foreground->lock};
    return foreground->home_button_short_pressed_blocked;
}

void WindowSystem::OnSystemButtonPress(SystemButtonType type) {
    std::scoped_lock lk{m_lock};
    if (m_system.GetAppletManager().IsHomeSession()) {
        this->RouteSystemButtonLocked(type);
        return;
    }
    switch (type) {
    case SystemButtonType::HomeButtonShortPressing:
        SendButtonAppletMessageLocked(AppletMessage::DetectShortPressingHomeButton);
        break;
    case SystemButtonType::HomeButtonLongPressing:
        SendButtonAppletMessageLocked(AppletMessage::DetectLongPressingHomeButton);
        break;
    case SystemButtonType::CaptureButtonShortPressing:
        SendButtonAppletMessageLocked(AppletMessage::DetectShortPressingCaptureButton);
        break;
    case SystemButtonType::CaptureButtonLongPressing:
        SendButtonAppletMessageLocked(AppletMessage::DetectLongPressingCaptureButton);
        break;
    default:
        break;
    }
}

void WindowSystem::OnPowerButtonPressed(ButtonPressDuration type) {
    // A console's power button: short is sleep (not emulated), the middle press opens the
    // overlay's power menu.
    if (type == ButtonPressDuration::MiddlePressing) {
        OnSystemButtonPress(SystemButtonType::PowerButtonLongPressing);
    }
}

void WindowSystem::OnHomeButtonPressed(ButtonPressDuration type) {
    // Map duration to SystemButtonType for legacy callers
    switch (type) {
    case ButtonPressDuration::ShortPressing:
        OnSystemButtonPress(SystemButtonType::HomeButtonShortPressing);
        break;
    case ButtonPressDuration::MiddlePressing:
    case ButtonPressDuration::LongPressing:
        OnSystemButtonPress(SystemButtonType::HomeButtonLongPressing);
        break;
    }
}

void WindowSystem::PruneTerminatedAppletsLocked() {
    for (auto it = m_applets.begin(); it != m_applets.end(); /* ... */) {
        const auto& [aruid, applet] = *it;

        std::scoped_lock lk{applet->lock};

        if (!applet->process->IsTerminated()) {
            // Not terminated.
            it = std::next(it);
            continue;
        }

        // A winding applet has had its own process killed but is kept alive as a transparent slot
        // while the reserved applet running in its place finishes (see WindAndDoReserved()).
        if (applet->is_winding) {
            if (!applet->child_applets.empty()) {
                it = std::next(it);
                continue;
            }

            const bool unwind = applet->unwind_after_reserved;
            applet->is_winding = false;
            applet->unwind_after_reserved = false;

            if (unwind && this->RestartAppletProcessLocked(applet.get())) {
                const u64 new_aruid = applet->aruid.pid;
                const auto next = std::next(it);

                if (new_aruid != aruid) {
                    auto node = m_applets.extract(it);
                    node.key() = new_aruid;
                    m_applets.insert(std::move(node));
                }

                applet->process->Run();
                m_event_observer->RequestUpdate();
                it = next;
                continue;
            }

            applet->reserved_applet.reset();
        }

        // Terminated, so ensure all child applets are terminated.
        if (!applet->child_applets.empty()) {
            this->TerminateChildAppletsLocked(applet.get());

            // Not ready to unlink until all child applets are terminated.
            it = std::next(it);
            continue;
        }

        // Erase from caller applet's list of children.
        if (auto caller_applet = applet->caller_applet.lock(); caller_applet) {
            std::scoped_lock lk2{caller_applet->lock};
            std::erase(caller_applet->child_applets, applet);
            applet->caller_applet.reset();

            // qlaunch deadlocks when ChangeIntoForeground reaches it before it has taken the
            // applet's result and closed its accessor (its foreground handler waits on a job its
            // paused workers hold). So in a HOME session the message follows the accessor's
            // release, with a one-second fallback; application callers are not held. Behaviour
            // follows the Ryujinx Harbor (MIT-licensed Ryujinx base).
            if (m_system.GetAppletManager().IsHomeSession() &&
                applet->type == AppletType::LibraryApplet && applet->accessor_count > 0 &&
                !caller_applet->lifecycle_manager.IsApplication()) {
                this->HoldForegroundMessageLocked(caller_applet, applet);
            }

            // We don't need to update the activity state of the caller applet yet.
            // It will be recalculated once we fall out of the termination handling path.
        }

        // If this applet was foreground, it no longer is.
        if (applet.get() == m_foreground_requested_applet) {
            m_foreground_requested_applet = nullptr;
        }

        // If this was the home menu, we should clean up.
        if (applet.get() == m_home_menu) {
            m_home_menu = nullptr;
            m_foreground_requested_applet = m_application;
        }

        // If this was the application, we should try to switch to the home menu.
        if (applet.get() == m_application) {
            m_application = nullptr;
            m_foreground_requested_applet = m_home_menu;

            // If we have a home menu, send it the application exited message.
            if (m_home_menu) {
                m_home_menu->lifecycle_manager.PushUnorderedMessage(m_system.Kernel(), AppletMessage::ApplicationExited);
            }
        }

        if (applet.get() == m_overlay_display) {
            m_overlay_display = nullptr;
        }

        this->UpdateCurrentApplicationLocked();

        // Finalize applet.
        applet->OnProcessTerminatedLocked();

        // Request update to ensure quiescence.
        m_event_observer->RequestUpdate();

        // Unlink and advance.
        it = m_applets.erase(it);
    }

    // If the last applet has exited, exit the system.
    if (m_applets.empty()) {
        m_system.Exit();
    }
}

void WindowSystem::HoldForegroundMessageLocked(const std::shared_ptr<Applet>& caller,
                                               const std::shared_ptr<Applet>& child) {
    // Caller lock is held.
    caller->lifecycle_manager.SetForegroundMessageHeld(m_system.Kernel(), true);
    m_foreground_holds.push_back({
        .caller = caller,
        .child = child.get(),
        .deadline_ns = (m_system.CoreTiming().GetGlobalTimeNs() + ForegroundHoldTimeout).count(),
    });
    m_system.CoreTiming().ScheduleEvent(ForegroundHoldTimeout, m_foreground_hold_event);
    LOG_DEBUG(Service_AM, "am: -> pid {} ChangeIntoForeground held until its library applet is released",
              caller->aruid.pid);
}

void WindowSystem::ReleaseForegroundHold(const Applet& child) {
    {
        std::scoped_lock lk{m_lock};
        this->ReleaseForegroundHoldsLocked(&child, false);
    }
    this->RequestUpdate();
}

void WindowSystem::ReleaseExpiredForegroundHolds() {
    {
        std::scoped_lock lk{m_lock};
        this->ReleaseForegroundHoldsLocked(nullptr, true);
    }
    this->RequestUpdate();
}

void WindowSystem::ReleaseForegroundHoldsLocked(const Applet* child, bool timed_out) {
    const s64 now = m_system.CoreTiming().GetGlobalTimeNs().count();
    std::vector<std::shared_ptr<Applet>> released;

    std::erase_if(m_foreground_holds, [&](const ForegroundHold& hold) {
        const bool matches = timed_out ? hold.deadline_ns <= now : hold.child == child;
        if (!matches) {
            return false;
        }
        if (auto caller = hold.caller.lock()) {
            released.push_back(std::move(caller));
        }
        return true;
    });

    for (const auto& caller : released) {
        const bool still_held = std::ranges::any_of(m_foreground_holds, [&](const auto& hold) {
            return hold.caller.lock() == caller;
        });
        if (still_held) {
            continue;
        }
        std::scoped_lock lk{caller->lock};
        caller->lifecycle_manager.SetForegroundMessageHeld(m_system.Kernel(), false);
        LOG_INFO(Service_AM, "am: foreground notification released for pid {} ({})",
                 caller->aruid.pid, timed_out ? "one-second fallback" : "accessor closed");
    }
}

bool WindowSystem::RestartAppletProcessLocked(Applet* applet) {
    if (!ReinitializeProcess(m_system, *applet->process, applet->program_id)) {
        LOG_ERROR(Service_AM, "Failed to restart winding applet_id={}",
                  static_cast<u32>(applet->applet_id));
        return false;
    }

    applet->aruid.pid = applet->process->GetProcessId();
    applet->is_process_running = false;
    applet->is_completed = false;

    applet->hid_registration.RegisterCurrentProcess();

    applet->lifecycle_manager.ResetForRelaunch();
    applet->is_activity_runnable = false;

    applet->launch_reason.flag = 1;

    m_event_observer->TrackAppletProcess(*applet);
    return true;
}

bool WindowSystem::LockHomeMenuIntoForegroundLocked() {
    // If the home menu is not locked into foreground, then there's nothing to do.
    if (m_home_menu == nullptr || !m_home_menu_foreground_locked) {
        m_home_menu_foreground_locked = false;
        return false;
    }

    // Terminate any direct child applets of the home menu.
    std::scoped_lock lk{m_home_menu->lock};

    this->TerminateChildAppletsLocked(m_home_menu);

    // When there are zero child applets left, we can proceed with the update.
    if (m_home_menu->child_applets.empty()) {
        m_home_menu->window_visible = true;
        m_foreground_requested_applet = m_home_menu;
        return false;
    }

    return true;
}

void WindowSystem::TerminateChildAppletsLocked(Applet* applet) {
    auto child_applets = applet->child_applets;

    applet->lock.unlock();
    for (const auto& child_applet : child_applets) {
        std::scoped_lock lk{child_applet->lock};
        child_applet->process->Terminate();
        child_applet->terminate_result = AM::ResultLibraryAppletTerminated;
    }
    applet->lock.lock();
}

bool WindowSystem::IsOverlayOpenLocked(const Applet& overlay) const {
    return overlay.window_visible && overlay.overlay_watching_short_home_button;
}

bool WindowSystem::DoesOverlayTakeInputLocked() const {
    if (m_overlay_display == nullptr) {
        return false;
    }

    std::scoped_lock lk{m_overlay_display->lock};
    return this->IsOverlayOpenLocked(*m_overlay_display);
}

bool WindowSystem::IsInInputChainLocked(const Applet& applet) const {
    // Behaviour follows the Ryujinx Harbor's GetInputRecipients (MIT-licensed Ryujinx base).
    for (const auto& child_applet : applet.child_applets) {
        std::scoped_lock lk{child_applet->lock};
        if (child_applet->is_winding) {
            return false;
        }
        if (!child_applet->is_process_running || !child_applet->window_visible) {
            continue;
        }
        switch (child_applet->library_applet_mode) {
        case LibraryAppletMode::AllForeground:
        case LibraryAppletMode::AllForegroundInitiallyHidden:
            return false;
        case LibraryAppletMode::PartialForeground:
        case LibraryAppletMode::PartialForegroundIndirectDisplay:
            // An inline helper keeps its caller interactive while it is in the chain itself.
            if (!this->IsInInputChainLocked(*child_applet)) {
                return false;
            }
            break;
        default:
            break;
        }
    }
    return true;
}

void WindowSystem::UpdateAppletStateLocked(Applet* applet, bool is_foreground, bool overlay_takes_input,
                                           s32 caller_z_index) {
    // With no applet, we don't have anything to do.
    if (!applet) {
        return;
    }

    std::scoped_lock lk{applet->lock};

    const bool inherited_foreground = applet->is_process_running && is_foreground;
    const auto visible_state =
        inherited_foreground ? ActivityState::ForegroundVisible : ActivityState::BackgroundVisible;
    const auto obscured_state = inherited_foreground ? ActivityState::ForegroundObscured
                                                     : ActivityState::BackgroundObscured;

    const bool has_obscuring_child_applets = [&] {
        for (const auto& child_applet : applet->child_applets) {
            std::scoped_lock lk2{child_applet->lock};
            if (child_applet->is_winding) {
                return true;
            }
            const auto mode = child_applet->library_applet_mode;
            if (child_applet->is_process_running && child_applet->window_visible &&
                (mode == LibraryAppletMode::AllForeground ||
                 mode == LibraryAppletMode::AllForegroundInitiallyHidden)) {
                return true;
            }
        }

        return false;
    }();

    const bool is_overlay = applet->applet_id == AppletId::OverlayDisplay;

    // Update visibility state.
    const bool should_be_visible =
        is_overlay ? applet->window_visible : (is_foreground && applet->window_visible);
    applet->display_layer_manager.SetWindowVisibility(should_be_visible);

    // In a HOME session only the input-focus chain reads input: the front-most applet, plus the
    // caller of an inline (partial-foreground) helper. A caller behind a full-screen applet gets
    // none, so presses made in that applet never reach it.
    const bool in_input_chain =
        !m_system.GetAppletManager().IsHomeSession() || this->IsInInputChainLocked(*applet);
    const bool needs_hid_input =
        is_overlay ? this->IsOverlayOpenLocked(*applet)
                   : (is_foreground && applet->window_visible && !overlay_takes_input &&
                      in_input_chain);

    applet->SetInteractibleLocked(needs_hid_input, needs_hid_input);

    // Update focus state and suspension.
    const bool is_obscured = has_obscuring_child_applets || !applet->window_visible;
    const auto state = applet->lifecycle_manager.GetActivityState();

    if (is_obscured && state != obscured_state) {
        // Set obscured state.
        applet->lifecycle_manager.SetActivityState(obscured_state);
        applet->UpdateSuspensionStateLocked(true);
    } else if (!is_obscured && state != visible_state) {
        // Set visible state.
        applet->lifecycle_manager.SetActivityState(visible_state);
        applet->UpdateSuspensionStateLocked(true);
    }

    // Layer ordering. Composition sorts back-to-front. Now with enums for calrity.
    s32 z_index = Background;
    if (is_overlay) {
        // In a HOME session the overlay's pop-ups draw over everything, as on a console.
        z_index = (this->IsOverlayOpenLocked(*applet) ||
                   (m_system.GetAppletManager().IsHomeSession() && applet->window_visible))
                      ? Overlay
                      : Background;
    } else if (inherited_foreground) {
        z_index = is_obscured ? Foreground : ForegroundVisible;
        // A visible child draws above its caller, explicitly rather than by layer creation order.
        if (caller_z_index >= Foreground && applet->window_visible) {
            z_index = (std::max)(z_index, caller_z_index + 1);
        }
    }
    applet->display_layer_manager.SetOverlayZIndex(z_index);

    // Recurse into child applets.
    for (const auto& child_applet : applet->child_applets) {
        this->UpdateAppletStateLocked(child_applet.get(), is_foreground, overlay_takes_input,
                                      z_index);
    }
}

} // namespace Service::AM

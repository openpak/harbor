// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "common/common_types.h"
#include "core/hle/result.h"
#include "core/hle/service/am/am_types.h"

namespace Core {
class System;
}

namespace Core::Timing {
struct EventType;
}

namespace Kernel {
class KReadableEvent;
}

namespace Service::OMM {
struct IdleHandlingContext;
}

namespace Service::AM {

struct Applet;
struct LockState;
struct SleepSequence;
class EventObserver;

enum class ButtonPressDuration {
    ShortPressing,
    MiddlePressing,
    LongPressing,
};

class WindowSystem {
public:
    explicit WindowSystem(Core::System& system);
    ~WindowSystem();

public:
    void SetEventObserver(EventObserver* event_observer);
    void Update();
    void RequestUpdate();

public:
    // False when the slot is taken: a second home menu, or an application that has not completed.
    [[nodiscard]] bool TrackApplet(std::shared_ptr<Applet> applet, bool is_application);
    bool HasApplication();
    std::shared_ptr<Applet> GetByAppletResourceUserId(u64 aruid);
    std::shared_ptr<Applet> GetMainApplet();
    std::shared_ptr<Applet> GetOverlayDisplayApplet();

public:
    void RequestHomeMenuToGetForeground();
    void RequestApplicationToGetForeground();
    void RequestLockHomeMenuIntoForeground();
    void RequestUnlockHomeMenuIntoForeground();
    void RequestAppletVisibilityState(Applet& applet, bool visible);

public:
    void OnOperationModeChanged();
    void OnExitRequested();

    // HOME session shutdown sequence (Turn Off, Restart, the host stopping the console), as am
    // ends applets on a console's power off: every applet gets Exit, the ones still running after
    // ShutdownExitTimeout are terminated, and once none is left the system exits.
    static constexpr std::chrono::milliseconds ShutdownExitTimeout{2000};
    void RequestShutdownSequence();
    void OnHomeButtonPressed(ButtonPressDuration type);
    void OnSystemButtonPress(SystemButtonType type);
    void OnCaptureButtonPressed(ButtonPressDuration type) {}
    void OnPowerButtonPressed(ButtonPressDuration type);

    // HOME session: terminate the running application on the window system's thread (the host's
    // force-terminate key). Ignored when no application is running.
    void RequestForceTerminateApplication();

    // HOME session: the lock behind every reader and writer accessor of one button.
    std::shared_ptr<LockState> GetLockState(u32 button_type);

    // The foreground application blocks HOME short presses (IApplicationFunctions 30/32).
    bool IsHomeButtonShortPressBlocked();

    // IGlobalStateController 12: how long HOME is held before it is a long press.
    void SetHomeButtonLongPressTime(std::chrono::milliseconds time) {
        m_home_button_long_press_ms = time.count();
    }
    std::chrono::milliseconds GetHomeButtonLongPressTime() const {
        return std::chrono::milliseconds{m_home_button_long_press_ms.load()};
    }

    // An accessor of a completed library applet was closed: its caller's held
    // ChangeIntoForeground goes out.
    void ReleaseForegroundHold(const Applet& child);

    // Sleep, as the 23.0.1 am does it (sleep_sequence.cpp). StartSleepSequence is
    // IGlobalStateController 2 (the HOME menu's Sleep Mode, its answer to a short power press)
    // and IApplicationFunctions 72; with sleep disabled it is a shutdown.
    Result StartSleepSequence(std::shared_ptr<Applet> requester);
    bool IsSleepEnabled();
    bool IsRebootEnabled();
    void DisableSleepTillShutdown();
    Result SuppressDisablingSleepTemporarily(s64 duration_ns);
    bool IsDisablingSleepSuppressed();
    u64 GetWakeupCount();
    Kernel::KReadableEvent* GetSleepSequenceIdleEvent();
    // The sleep lock (ICommonStateGetter 10-12) of one applet.
    void RequestToAcquireSleepLock(Applet& applet);
    Result ReleaseSleepLock(Applet& applet);
    Result ReleaseSleepLockTransiently(Applet& applet);
    // idle:sys: the context of the applet in front, and its auto-sleep time being reached.
    void GetIdleHandlingContext(OMM::IdleHandlingContext& out_context);
    void OnAutoPowerDown();
    void LoadIdleSettingsIfReady();

private:
    void UpdateCurrentApplicationLocked();
    void NotifyApplicationChangedIfNeeded();
    void PruneTerminatedAppletsLocked();
    void UpdateShutdownSequenceLocked();
    void ForceTerminateApplicationLocked();
    bool RestartAppletProcessLocked(Applet* applet);
    bool LockHomeMenuIntoForegroundLocked();
    void TerminateChildAppletsLocked(Applet* applet);
    bool IsOverlayOpenLocked(const Applet& overlay) const;
    bool DoesOverlayTakeInputLocked() const;
    bool IsInInputChainLocked(const Applet& applet) const;

    void HoldForegroundMessageLocked(const std::shared_ptr<Applet>& caller,
                                     const std::shared_ptr<Applet>& child);
    void ReleaseExpiredForegroundHolds();
    void ReleaseForegroundHoldsLocked(const Applet* child, bool timed_out);

    void UpdateAppletStateLocked(Applet* applet, bool is_foreground, bool overlay_takes_input,
                                 s32 caller_z_index = -1);
    void SendButtonAppletMessageLocked(AppletMessage message);
    void OnPowerButtonShortPressLocked();
    bool SwallowButtonAroundSleepLocked(SystemButtonType type);
    void UpdateSleepSequenceLocked();
    void EnterSleepLocked();
    void WakeLocked(const char* reason);
    void GrantSleepLockLocked(Applet& applet);
    void AcknowledgePrepareSleepLocked(Applet& applet);
    void RouteSystemButtonLocked(SystemButtonType type);
    void PushButtonMessageLocked(Applet* applet, AppletMessage message);

private:
    // System reference.
    Core::System& m_system;

    // Event observer.
    EventObserver* m_event_observer{};

    // Lock.
    std::mutex m_lock{};

    // Home menu state.
    bool m_home_menu_foreground_locked{};
    Applet* m_foreground_requested_applet{};

    // Foreground roots.
    Applet* m_home_menu{};
    Applet* m_application{};
    Applet* m_overlay_display{};

    // Applet map by aruid.
    std::map<u64, std::shared_ptr<Applet>> m_applets{};
    std::optional<u64> m_pending_application_notification{};
    u64 m_logged_foreground_pid{};
    // HOME session default until the HOME menu sets its own (IGlobalStateController 12).
    std::atomic<s64> m_home_button_long_press_ms{500};
    std::atomic<bool> m_force_terminate_requested{};

    // Shutdown sequence state (under m_lock).
    enum class ShutdownStage { None, Requested, WaitingForExit, Terminating, Ended };
    ShutdownStage m_shutdown_stage{ShutdownStage::None};
    std::chrono::steady_clock::time_point m_shutdown_deadline{};
    std::atomic<bool> m_shutdown_timed_out{};
    std::shared_ptr<Core::Timing::EventType> m_shutdown_event{};
    std::map<u32, std::shared_ptr<LockState>> m_lock_states{};

    // Callers whose ChangeIntoForeground waits for a completed library applet's accessor to close.
    struct ForegroundHold {
        std::weak_ptr<Applet> caller;
        const Applet* child;
        s64 deadline_ns;
    };
    std::vector<ForegroundHold> m_foreground_holds{};
    std::shared_ptr<Core::Timing::EventType> m_foreground_hold_event{};

    // Sleep state (under m_lock).
    std::unique_ptr<SleepSequence> m_sleep;
    std::atomic<bool> m_idle_settings_requested{};
};

} // namespace Service::AM

// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>

#include "core/hle/service/am/am_types.h"

namespace Core {
class System;
}

namespace Service {
class Process;
}

namespace Service::AM {

class WindowSystem;

enum class LaunchType {
    FrontendInitiated,
    ApplicationInitiated,
};

struct FrontendAppletParameters {
    ProgramId program_id{};
    AppletId applet_id{};
    AppletType applet_type{};
    LaunchType launch_type{};
    s32 program_index{};
    s32 previous_program_index{-1};
};

// The frontend booted qlaunch: a HOME session.
inline bool IsHomeSessionRoot(const FrontendAppletParameters& params) {
    return params.applet_id == AppletId::QLaunch;
}

class AppletManager {
public:
    explicit AppletManager(Core::System& system);
    ~AppletManager();

    void CreateAndInsertByFrontendAppletParameters(std::unique_ptr<Process> process,
                                                   const FrontendAppletParameters& params);
    // The host stops emulation. In a HOME session this runs the shutdown sequence and returns
    // once every applet has ended (or the sequence timed out), so no guest code runs into
    // services that are shutting down.
    void RequestExit();
    void OperationModeChanged();

public:
    void SetWindowSystem(WindowSystem* window_system);
    [[nodiscard]] WindowSystem* GetWindowSystem() const { return m_window_system; }

    // HOME session: the frontend booted qlaunch, so am behaves like a console's (firmware
    // library applets, the overlay applet, no host-made user). Stock behaviour otherwise.
    [[nodiscard]] bool IsHomeSession() const { return m_home_session.load(); }

    // Forwarded to the window system, if one is running.
    void ReleaseForegroundHold(const Applet& child);

    // Host keys (HOME session). The power button held: the overlay's power menu. Force-terminate:
    // ends the application the HOME menu launched, as a kill does on a console.
    void PowerButtonMiddlePress();
    void ForceTerminateApplication();

    // The power button pressed briefly (OnSystemButtonPress PowerButtonShortPressing). Awake, the
    // HOME menu hears it and starts the sleep sequence (sleep_sequence.cpp); asleep, the console
    // wakes. At the deepest point of the sequence the frontend's sleep handler pauses the guest
    // and blanks the screen, and wakes it on the power button, HOME or any button by calling this
    // again.
    void PowerButtonShortPress();
    [[nodiscard]] bool IsSleeping() const { return m_sleeping.load(); }
    // Called when the console goes to sleep (true) or wakes (false), with am's locks held: the
    // handler must only queue its work.
    void SetSleepHandler(std::function<void(bool)> handler);
    // From the window system's sleep sequence: the console reached its deepest state, or woke.
    void OnSleepStateChanged(bool sleeping);
    // idle:sys dims the screen (true) after the dimming time and brings it back on input. The
    // handler is called with idle's lock held and must only queue its work.
    void SetDimHandler(std::function<void(bool)> handler);
    void OnDimChanged(bool dimmed);

    // The guest asked for a reboot or a shutdown (IGlobalStateController 3/4, IOverlayFunctions
    // 10/11). In a HOME session the applets end first (the window system's shutdown sequence),
    // then the session stops; the frontend reads this afterwards to start it again with the same
    // data or to close.
    enum class PowerRequest { None, Shutdown, Reboot };
    void RequestPower(PowerRequest request);
    [[nodiscard]] PowerRequest GetPowerRequest() const { return m_power_request.load(); }
    [[nodiscard]] bool IsRestartRequested() const { return GetPowerRequest() == PowerRequest::Reboot; }

    // The window system's shutdown sequence has no applet left.
    void OnAppletsEnded();

private:
    Core::System& m_system;

    std::mutex m_lock;
    std::condition_variable m_cv;

    // HOME session shutdown: set once every applet has ended (under m_ended_lock).
    std::mutex m_ended_lock;
    std::condition_variable m_ended_cv;
    bool m_applets_ended{};

    WindowSystem* m_window_system{};
    std::atomic<bool> m_home_session{};
    std::atomic<PowerRequest> m_power_request{PowerRequest::None};

    std::atomic<bool> m_sleeping{};
    std::mutex m_sleep_lock;
    std::function<void(bool)> m_sleep_handler;
    std::function<void(bool)> m_dim_handler;
    bool m_dimmed{};

    FrontendAppletParameters m_pending_parameters{};
    std::unique_ptr<Process> m_pending_process{};
};

} // namespace Service::AM

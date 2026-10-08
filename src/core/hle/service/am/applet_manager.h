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

    // The power button pressed briefly (OnSystemButtonPress PowerButtonShortPressing): the console
    // goes to sleep, or wakes when it sleeps. am keeps the state; the frontend's sleep handler
    // pauses the guest and blanks the screen, and wakes it on the power button, HOME or any
    // button by calling this again.
    void PowerButtonShortPress();
    [[nodiscard]] bool IsSleeping() const { return m_sleeping.load(); }
    // Called when the console goes to sleep (true) or wakes (false), with am's locks held: the
    // handler must only queue its work.
    void SetSleepHandler(std::function<void(bool)> handler);
    // From the window system (PowerButtonShortPressing in a HOME session).
    void ToggleSleep();

    // The guest asked for a reboot (IGlobalStateController 4, IOverlayFunctions 11). The session
    // stops; the frontend reads this afterwards to start it again with the same data.
    enum class PowerRequest { None, Shutdown, Reboot };
    void RequestPower(PowerRequest request);
    [[nodiscard]] PowerRequest GetPowerRequest() const { return m_power_request.load(); }
    [[nodiscard]] bool IsRestartRequested() const { return GetPowerRequest() == PowerRequest::Reboot; }

private:
    Core::System& m_system;

    std::mutex m_lock;
    std::condition_variable m_cv;

    WindowSystem* m_window_system{};
    std::atomic<bool> m_home_session{};
    std::atomic<PowerRequest> m_power_request{PowerRequest::None};

    std::atomic<bool> m_sleeping{};
    std::mutex m_sleep_lock;
    std::function<void(bool)> m_sleep_handler;

    FrontendAppletParameters m_pending_parameters{};
    std::unique_ptr<Process> m_pending_process{};
};

} // namespace Service::AM

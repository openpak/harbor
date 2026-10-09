// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <chrono>
#include <memory>
#include <mutex>

#include "common/common_types.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/os/event.h"

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

// What the idle timer needs from am about the applet in front (am's SetHandlingContext on a
// console, built from that applet's ISelfController calls).
struct IdleHandlingContext {
    // A HOME session with an applet in front; otherwise the timer stands still.
    bool valid{};
    // A sleep or shutdown sequence runs: the console is not idle.
    bool sequence_running{};
    // OverrideAutoSleepTimeAndDimmingTime, in seconds (0: no override).
    s32 sleep_handheld_s{};
    s32 sleep_console_s{};
    s32 dim_handheld_s{};
    s32 dim_console_s{};
    // SetIdleTimeDetectionExtension: 0 none, 1 Extended, 2 ExtendedUnsafe.
    u32 extension{};
    bool auto_sleep_disabled{};
    bool media_playing{};
};

// idle:sys as omm runs it on firmware 23.0.1 (the idle module of 0100000000000045, flat at
// 0x08500000): the time since the last input against a dimming time and an auto-sleep time worked
// out from System Settings (Sleep Mode), the TV settings and am's context for the applet in front.
// At the dimming time the screen dims; at the auto-sleep time "auto power down" is requested and
// its event signalled, and am tells HOME (AutoPowerDown), which starts the sleep sequence. Any
// input (buttons, sticks, the touch screen, a USB keyboard or mouse) or ReportUserIsActive starts
// the count over.
class IdlePolicy {
public:
    explicit IdlePolicy(Core::System& system);
    ~IdlePolicy();

    // idle:sys 0, 1, 2 (dimmed), 4 and 5.
    Kernel::KReadableEvent* GetAutoPowerDownEvent();
    bool IsAutoPowerDownRequested();
    bool IsDimmed();
    void LoadAndApplySettings();
    // From am's thread once HOME is up: the first settings load (never from the timer's thread).
    void LoadSettingsOnce();
    void ReportUserIsActive();

    // The times for a context and the current settings, 0 for never (exposed for the self-check).
    struct Settings {
        u32 handheld_plan{3};
        u32 console_plan{6};
        bool sleeps_while_playing_media{true};
        bool prevents_screen_burn_in{true};
    };
    static std::chrono::nanoseconds AutoSleepTime(const Settings& settings,
                                                  const IdleHandlingContext& context, bool docked);
    static std::chrono::nanoseconds DimTime(const Settings& settings,
                                            const IdleHandlingContext& context, bool docked);

    // Harness runs: no dimming or auto-sleep while suppressed (the default under HARBOR_CONTROL).
    static void SetHarnessSuppressed(bool suppressed);

private:
    void Tick();
    bool PollInput();
    void OnActivityLocked();
    IdleHandlingContext EffectiveContextLocked(const IdleHandlingContext& incoming,
                                               std::chrono::steady_clock::time_point now);

    Core::System& m_system;
    KernelHelpers::ServiceContext m_context;
    Event m_auto_power_down_event;
    std::shared_ptr<Core::Timing::EventType> m_tick_event;

    std::mutex m_lock;
    Settings m_settings{};
    bool m_settings_loaded{};
    std::chrono::steady_clock::time_point m_last_activity{};
    std::chrono::steady_clock::time_point m_last_tick{};
    u64 m_input_signature{};
    bool m_dimmed{};
    bool m_auto_power_down{};
    // The context in force, and a looser one waiting out its grace time.
    IdleHandlingContext m_effective{};
    bool m_effective_valid{};
    IdleHandlingContext m_pending{};
    bool m_pending_valid{};
    std::chrono::steady_clock::time_point m_pending_at{};
    std::chrono::nanoseconds m_logged_sleep_time{-1};
    std::chrono::nanoseconds m_logged_dim_time{-1};
};

// The running idle:sys (null outside a session): am's ISelfController and IGlobalStateController
// reach it here, as they reach idle:sys over IPC on a console.
IdlePolicy* GetIdlePolicy();

} // namespace Service::OMM

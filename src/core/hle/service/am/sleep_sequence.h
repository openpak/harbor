// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <chrono>
#include <memory>

#include "common/common_types.h"
#include "core/hle/result.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/os/event.h"

namespace Core {
class System;
}

namespace Core::Timing {
struct EventType;
}

namespace Service::AM {

struct Applet;

// am's answer to a sleep-lock call with no lock held, a transient release with no sleep pending,
// and a second sleep sequence while one runs (0x3E880 in the 23.0.1 am).
constexpr Result ResultSleepSequenceInvalidState{ErrorModule::AM, 500};

// am's sleep state on firmware 23.0.1 (the power-state object at +0x378 of am's main object,
// constructor 0x08577a14), owned by the window system and read under its lock.
//
// IsSleepEnabled (ICommonStateGetter 502, IHomeMenuFunctions 40) is `enabled || now <=
// suppressed_until`: `enabled` starts true and only DisableSleepTillShutdown (500) clears it (with
// the reboot flag); SuppressDisablingSleepTemporarily (501) re-allows sleep for a while. A sleep
// sequence started while sleep is disabled runs as a shutdown.
struct SleepSequence {
    explicit SleepSequence(Core::System& system);
    ~SleepSequence();

    enum class Stage {
        Awake,
        // Sleep was asked for; the next window-system update starts it.
        Requested,
        // The sleep window holds the foreground: applets lose focus, sleep-lock holders were sent
        // RequestToPrepareSleep and are waited for.
        Preparing,
        // The deepest state: psc modules are in EssentialServicesSleepReady, the host paused.
        Asleep,
    };

    Core::System& system;
    KernelHelpers::ServiceContext context;
    // ICommonStateGetter 15: signalled while no sequence runs (signalled at construction, cleared
    // when one starts, signalled when it ends).
    Event idle_event;
    std::shared_ptr<Core::Timing::EventType> timer_event;

    Stage stage{Stage::Awake};
    u64 sequence_id{};
    u64 wakeup_count{};
    bool enabled{true};
    // IHomeMenuFunctions 41; StartRebootSequence powers off instead while it is false.
    bool reboot_enabled{true};
    s64 suppressed_until_ns{};
    // The applet that started the sequence (StartSleepSequence with its flag set).
    std::weak_ptr<Applet> requester{};
    std::chrono::steady_clock::time_point settle_deadline{};
    std::chrono::steady_clock::time_point lock_deadline{};
    bool lock_timeout_logged{};
    // HOME or Capture was held when the console woke: that press woke it and goes no further.
    bool swallow_home{};
    bool swallow_capture{};

    // How long the applets get to take the focus change in before the host pauses (a console's am
    // moves on once the sleep window is up; the guest here needs frames to see it).
    static constexpr std::chrono::milliseconds SettleTime{500};
    // How long a held sleep lock defers sleep before am goes ahead.
    static constexpr std::chrono::seconds SleepLockTimeout{30};

    [[nodiscard]] bool IsSleepEnabled() const;
    [[nodiscard]] bool IsDisablingSleepSuppressed() const;
    [[nodiscard]] bool IsActive() const {
        return stage != Stage::Awake;
    }
    // While the sleep window holds the foreground, no applet does.
    [[nodiscard]] bool TakesForeground() const {
        return stage == Stage::Preparing || stage == Stage::Asleep;
    }
    void ScheduleUpdate(std::chrono::nanoseconds delay);
};

} // namespace Service::AM

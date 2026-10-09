// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <array>
#include <atomic>
#include <cstdlib>

#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/am/window_system.h"
#include "core/hle/service/omm/idle_policy.h"
#include "core/hle/service/set/system_settings_server.h"
#include "core/hle/service/sm/sm.h"
#include "hid_core/frontend/emulated_console.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/frontend/emulated_devices.h"
#include "hid_core/hid_core.h"

namespace Service::OMM {

namespace {

using namespace std::chrono_literals;

std::atomic<IdlePolicy*> g_idle_policy{};

// Harness runs (HARBOR_CONTROL) neither dim nor auto-sleep unless HARBOR_AUTO_SLEEP=1 or the
// harness turns it on (`auto-sleep on`): other streams' tests are not put to sleep.
std::atomic_bool g_harness_off{std::getenv("HARBOR_CONTROL") != nullptr &&
                               std::getenv("HARBOR_AUTO_SLEEP") == nullptr};

constexpr std::chrono::nanoseconds TickPeriod = 500ms;

// Default auto-sleep times by Sleep Mode plan (0x085c8160 handheld, 0x085c8220 console, the
// second table of each while media plays). Console plan 6 is 15 minutes; 0 is never.
constexpr std::array<std::chrono::seconds, 6> HandheldSleep{60s, 180s, 300s, 600s, 1800s, 0s};
constexpr std::array<std::chrono::seconds, 6> HandheldSleepMedia{14400s, 14400s, 14400s,
                                                                 14400s, 14400s, 0s};
constexpr std::array<std::chrono::seconds, 7> ConsoleSleep{3600s,  7200s, 10800s, 21600s,
                                                           43200s, 0s,    900s};
constexpr std::array<std::chrono::seconds, 7> ConsoleSleepMedia{14400s, 14400s, 14400s, 0s,
                                                                0s,     0s,     14400s};
// Default dimming times by handheld plan (fwdbg idle!handheld_dim_time_level_N, 0x08619a00).
constexpr std::array<std::chrono::seconds, 6> HandheldDim{45s, 120s, 240s, 300s, 300s, 300s};
// Docked with "prevent screen burn-in", and the handheld fallback.
constexpr std::chrono::seconds DefaultDim = 300s;

template <size_t N>
std::chrono::seconds Lookup(const std::array<std::chrono::seconds, N>& table, u32 plan) {
    return plan < N ? table[plan] : 0s;
}

bool IsDocked() {
    return Settings::values.use_docked_mode.GetValue() == Settings::ConsoleMode::Docked;
}

void Mix(u64& hash, u64 value) {
    hash = (hash ^ value) * 0x100000001B3ULL;
}

} // namespace

IdlePolicy* GetIdlePolicy() {
    return g_idle_policy.load();
}

void IdlePolicy::SetHarnessSuppressed(bool suppressed) {
    g_harness_off = suppressed;
    LOG_INFO(Service_PTM, "idle: auto-sleep and dimming {} for this harness run",
             suppressed ? "off" : "on");
}

// 0x08549780 (SetHandlingContext): a context that lets the console sleep sooner takes effect only
// after a grace time (media playback ending 300 s, auto-sleep allowed again or a shorter idle
// time detection extension 90 s); any other change takes effect at once.
IdleHandlingContext IdlePolicy::EffectiveContextLocked(const IdleHandlingContext& incoming,
                                                       std::chrono::steady_clock::time_point now) {
    const auto same = [](const IdleHandlingContext& a, const IdleHandlingContext& b) {
        return a.sleep_handheld_s == b.sleep_handheld_s && a.sleep_console_s == b.sleep_console_s &&
               a.dim_handheld_s == b.dim_handheld_s && a.dim_console_s == b.dim_console_s &&
               a.extension == b.extension && a.auto_sleep_disabled == b.auto_sleep_disabled &&
               a.media_playing == b.media_playing;
    };
    if (!m_effective_valid) {
        m_effective = incoming;
        m_effective_valid = true;
        return m_effective;
    }
    if (same(incoming, m_effective)) {
        m_pending_valid = false;
        return m_effective;
    }
    std::chrono::seconds grace{};
    if (m_effective.media_playing && !incoming.media_playing) {
        grace = 300s;
    } else if (m_effective.auto_sleep_disabled && !incoming.auto_sleep_disabled) {
        grace = 90s;
    } else if (incoming.extension < m_effective.extension) {
        grace = 90s;
    }
    if (grace == 0s) {
        m_effective = incoming;
        m_pending_valid = false;
        return m_effective;
    }
    if (!m_pending_valid || !same(incoming, m_pending)) {
        m_pending = incoming;
        m_pending_valid = true;
        m_pending_at = now + grace;
        LOG_INFO(Service_PTM, "idle: a looser context takes effect in {} s", grace.count());
    } else if (now >= m_pending_at) {
        m_effective = incoming;
        m_pending_valid = false;
    }
    return m_effective;
}

// 0x08548e10.
std::chrono::nanoseconds IdlePolicy::AutoSleepTime(const Settings& settings,
                                                   const IdleHandlingContext& context,
                                                   bool docked) {
    if (context.auto_sleep_disabled ||
        (context.media_playing && !settings.sleeps_while_playing_media)) {
        return 0s;
    }
    std::chrono::nanoseconds time{};
    const s32 override_s = docked ? context.sleep_console_s : context.sleep_handheld_s;
    if (override_s > 0) {
        time = std::chrono::seconds{override_s};
    } else if (docked) {
        time = Lookup(context.media_playing ? ConsoleSleepMedia : ConsoleSleep,
                      settings.console_plan);
    } else {
        time = Lookup(context.media_playing ? HandheldSleepMedia : HandheldSleep,
                      settings.handheld_plan);
    }
    // An extended idle time detection keeps the console awake at least 30 (Extended) or 60
    // (ExtendedUnsafe) minutes.
    if (time > 0s && context.extension == 1 && time < 1800s) {
        time = 1800s;
    } else if (time > 0s && context.extension == 2 && time < 3600s) {
        time = 3600s;
    }
    return time;
}

// 0x08549320. omm gates dimming on its own context flag (+0x39) next to auto-sleep's (+0x38);
// which am state feeds it is not traced, so SetAutoSleepDisabled stops dimming as well.
std::chrono::nanoseconds IdlePolicy::DimTime(const Settings& settings,
                                             const IdleHandlingContext& context, bool docked) {
    if (context.auto_sleep_disabled ||
        (context.media_playing && !settings.sleeps_while_playing_media)) {
        return 0s;
    }
    std::chrono::nanoseconds time{};
    const s32 override_s = docked ? context.dim_console_s : context.dim_handheld_s;
    if (override_s > 0) {
        time = std::chrono::seconds{override_s};
    } else if (context.media_playing) {
        time = 0s;
    } else if (docked) {
        time = settings.prevents_screen_burn_in ? DefaultDim : 0s;
    } else {
        time = Lookup(HandheldDim, settings.handheld_plan);
    }
    if (context.extension == 2) {
        return 0s;
    }
    if (time > 0s && context.extension == 1 && time < 900s) {
        time = 900s;
    }
    return time;
}

IdlePolicy::IdlePolicy(Core::System& system)
    : m_system{system}, m_context{system, "idle:sys"}, m_auto_power_down_event{m_context} {
    m_last_activity = std::chrono::steady_clock::now();
    m_tick_event = Core::Timing::CreateEvent(
        "idle:Tick",
        [this](s64, std::chrono::nanoseconds) -> std::optional<std::chrono::nanoseconds> {
            this->Tick();
            return TickPeriod;
        });
    m_system.CoreTiming().ScheduleLoopingEvent(TickPeriod, TickPeriod, m_tick_event);
    g_idle_policy = this;
    if (g_harness_off) {
        LOG_INFO(Service_PTM, "idle: auto-sleep and dimming are off in this harness run "
                              "(HARBOR_AUTO_SLEEP=1 or `auto-sleep on` turns them on)");
    }
    // A new session starts at full brightness, whatever the last one ended with.
    m_system.GetAppletManager().OnDimChanged(false);
}

IdlePolicy::~IdlePolicy() {
    g_idle_policy = nullptr;
    m_system.CoreTiming().UnscheduleEvent(m_tick_event);
}

Kernel::KReadableEvent* IdlePolicy::GetAutoPowerDownEvent() {
    return m_auto_power_down_event.GetHandle();
}

bool IdlePolicy::IsAutoPowerDownRequested() {
    std::scoped_lock lk{m_lock};
    return m_auto_power_down;
}

bool IdlePolicy::IsDimmed() {
    std::scoped_lock lk{m_lock};
    return m_dimmed;
}

void IdlePolicy::LoadAndApplySettings() {
    auto set_sys =
        m_system.ServiceManager().GetService<Set::ISystemSettingsServer>("set:sys", false);
    if (!set_sys) {
        return;
    }
    Set::SleepSettings sleep{};
    Set::TvSettings tv{};
    set_sys->GetSleepSettings(&sleep);
    set_sys->GetTvSettings(&tv);
    std::scoped_lock lk{m_lock};
    m_settings = {
        .handheld_plan = static_cast<u32>(sleep.handheld_sleep_plan),
        .console_plan = static_cast<u32>(sleep.console_sleep_plan),
        .sleeps_while_playing_media = sleep.flags.SleepsWhilePlayingMedia != 0,
        .prevents_screen_burn_in = tv.flags.PreventsScreenBurnIn != 0,
    };
    m_settings_loaded = true;
    LOG_INFO(Service_PTM, "idle: settings handheld plan {} console plan {} media {} burn-in {}",
             m_settings.handheld_plan, m_settings.console_plan,
             m_settings.sleeps_while_playing_media, m_settings.prevents_screen_burn_in);
}

void IdlePolicy::ReportUserIsActive() {
    std::scoped_lock lk{m_lock};
    this->OnActivityLocked();
}

void IdlePolicy::OnActivityLocked() {
    m_last_activity = std::chrono::steady_clock::now();
    if (m_auto_power_down) {
        m_auto_power_down = false;
        m_auto_power_down_event.Clear(m_system.Kernel());
    }
    if (m_dimmed) {
        m_dimmed = false;
        LOG_INFO(Service_PTM, "idle: the screen comes back to full brightness");
        m_system.GetAppletManager().OnDimChanged(false);
    }
}

// ins:r on a console: any button, stick, touch, keyboard or mouse change.
bool IdlePolicy::PollInput() {
    auto& hid = m_system.HIDCore();
    u64 hash = 0xCBF29CE484222325ULL;
    for (size_t i = 0; i < 10; ++i) {
        const auto* controller = hid.GetEmulatedControllerByIndex(i);
        if (controller == nullptr || !controller->IsConnected()) {
            continue;
        }
        Mix(hash, static_cast<u64>(controller->GetNpadButtons().raw));
        Mix(hash, controller->GetHomeButtons().raw);
        Mix(hash, controller->GetCaptureButtons().raw);
        // Sticks in 16 steps a side, so resting noise does not count as a user.
        const auto sticks = controller->GetSticks();
        for (const s32 axis : {sticks.left.x, sticks.left.y, sticks.right.x, sticks.right.y}) {
            Mix(hash, static_cast<u64>(axis / 2048));
        }
    }
    for (const auto& finger : hid.GetEmulatedConsole()->GetTouch()) {
        Mix(hash, finger.pressed ? 1 + static_cast<u64>(finger.position.x * 64.0f) * 64 +
                                       static_cast<u64>(finger.position.y * 64.0f)
                                 : 0);
    }
    const auto* devices = hid.GetEmulatedDevices();
    for (const u8 byte : devices->GetKeyboard().key) {
        Mix(hash, byte);
    }
    Mix(hash, devices->GetMouseButtons().raw);

    const bool changed = hash != m_input_signature;
    m_input_signature = hash;
    return changed;
}

void IdlePolicy::Tick() {
    auto& applet_manager = m_system.GetAppletManager();
    auto* window_system = applet_manager.GetWindowSystem();
    if (!applet_manager.IsHomeSession() || window_system == nullptr) {
        return;
    }
    if (!m_settings_loaded) {
        this->LoadAndApplySettings();
    }
    IdleHandlingContext context{};
    window_system->GetIdleHandlingContext(context);

    bool auto_power_down = false;
    bool dim = false;
    {
        std::scoped_lock lk{m_lock};
        const bool input = this->PollInput();
        // A host pause (the tick stood still) does not count as idle time.
        const auto now = std::chrono::steady_clock::now();
        const bool resumed = now - m_last_tick > 4 * TickPeriod;
        m_last_tick = now;
        if (input || resumed || context.sequence_running || !context.valid ||
            g_harness_off.load()) {
            this->OnActivityLocked();
            return;
        }
        context = this->EffectiveContextLocked(context, now);
        const bool docked = IsDocked();
        const auto sleep_time = AutoSleepTime(m_settings, context, docked);
        const auto dim_time = DimTime(m_settings, context, docked);
        if (sleep_time != m_logged_sleep_time || dim_time != m_logged_dim_time) {
            m_logged_sleep_time = sleep_time;
            m_logged_dim_time = dim_time;
            LOG_INFO(Service_PTM, "idle: dims after {} s, sleeps after {} s ({})",
                     std::chrono::duration_cast<std::chrono::seconds>(dim_time).count(),
                     std::chrono::duration_cast<std::chrono::seconds>(sleep_time).count(),
                     docked ? "docked" : "handheld");
        }
        // Wall time, as a console's idle counts it; the tick stops while the guest is paused and
        // a wake starts the count over.
        const std::chrono::nanoseconds idle = now - m_last_activity;
        if (!m_dimmed && dim_time > 0s && idle >= dim_time) {
            m_dimmed = dim = true;
        }
        if (!m_auto_power_down && sleep_time > 0s && idle >= sleep_time) {
            m_auto_power_down = auto_power_down = true;
            m_auto_power_down_event.Signal(m_system.Kernel());
        }
    }
    if (dim) {
        LOG_INFO(Service_PTM, "idle: the screen dims");
        applet_manager.OnDimChanged(true);
    }
    if (auto_power_down) {
        LOG_INFO(Service_PTM, "idle: auto-sleep time reached; auto power down requested");
        window_system->OnAutoPowerDown();
    }
}

} // namespace Service::OMM

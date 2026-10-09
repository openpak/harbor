// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// The console's sleep and wake, as am does them on firmware 23.0.1 (addresses are in the 23.0.1
// am main, flat at 0x08500000).
//
// - A short power press reaches am as an spsm notification (ReceiveNotificationMessage 5,
//   0x0859357c) and goes to the applets that listen for it as DetectShortPressingPowerButton. am
//   never starts sleep on its own: the only callers of the sequence (0x08577f48) are
//   IGlobalStateController 2 StartSleepSequence, 3/4 (shutdown, reboot) and IApplicationFunctions
//   70-72. The HOME menu answers the press, its power menu's Sleep Mode and idle's auto-sleep
//   (AutoPowerDown) with StartSleepSequence.
// - StartSleepSequence runs the shutdown sequence instead when IsSleepEnabled is false.
// - The sequence puts am's sleep window in front of everything (every applet loses the foreground),
//   asks each sleep-lock holder to get ready (RequestToPrepareSleep) and waits for its release,
//   then (omm DisableAudioVisual, EnterSleepAndWait; spsm walks the psc modules down) sleeps.
// - On wake the wake counter goes up (ICommonStateGetter 14), the display comes back, the sleep
//   window goes, the applets get their focus back, transient sleep-lock holders get the lock again,
//   the applet that started the sequence asking to be told gets FinishedSleepSequence and the
//   "no sequence running" event (ICommonStateGetter 15) is signalled.

#include <array>

#include "common/logging.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/service/am/am_results.h"
#include "core/hle/service/am/applet.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/am/event_observer.h"
#include "core/hle/service/am/sleep_sequence.h"
#include "core/hle/service/am/window_system.h"
#include "core/hle/service/omm/idle_policy.h"
#include "core/hle/service/psc/pm_state.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"

namespace Service::AM {

namespace {

using PSC::PmState;

constexpr std::array SleepStates{PmState::MinimumAwake, PmState::SleepReady,
                                 PmState::EssentialServicesSleepReady};
constexpr std::array WakeStates{PmState::EssentialServicesAwake, PmState::MinimumAwake,
                                PmState::FullAwake};

// SuppressDisablingSleepTemporarily refuses longer durations (0x08580c40).
constexpr s64 MaxSleepSuppressionNs = 0x4E94914F000;

s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

SleepSequence::SleepSequence(Core::System& system_)
    : system{system_}, context{system_, "am:SleepSequence"}, idle_event{context} {
    idle_event.Signal(system.Kernel());
    timer_event = Core::Timing::CreateEvent(
        "am:SleepSequence",
        [this](s64, std::chrono::nanoseconds) -> std::optional<std::chrono::nanoseconds> {
            if (auto* window_system = system.GetAppletManager().GetWindowSystem()) {
                window_system->RequestUpdate();
            }
            return std::nullopt;
        });
}

SleepSequence::~SleepSequence() {
    system.CoreTiming().UnscheduleEvent(timer_event);
}

bool SleepSequence::IsSleepEnabled() const {
    return enabled || NowNs() <= suppressed_until_ns;
}

bool SleepSequence::IsDisablingSleepSuppressed() const {
    return !enabled && NowNs() <= suppressed_until_ns;
}

void SleepSequence::ScheduleUpdate(std::chrono::nanoseconds delay) {
    system.CoreTiming().UnscheduleEvent(timer_event);
    system.CoreTiming().ScheduleEvent(delay, timer_event);
}

Result WindowSystem::StartSleepSequence(std::shared_ptr<Applet> requester) {
    if (!m_system.GetAppletManager().IsHomeSession()) {
        LOG_INFO(Service_AM, "am: sleep ignored outside a HOME session");
        R_SUCCEED();
    }
    bool shutdown = false;
    {
        std::scoped_lock lk{m_lock};
        if (m_sleep->IsActive() || m_shutdown_stage != ShutdownStage::None) {
            LOG_INFO(Service_AM, "am: sleep refused: a sleep or shutdown sequence is running");
            R_THROW(ResultSleepSequenceInvalidState);
        }
        if (!m_sleep->IsSleepEnabled()) {
            shutdown = true;
        } else {
            m_sleep->stage = SleepSequence::Stage::Requested;
            ++m_sleep->sequence_id;
            m_sleep->requester = requester;
            m_sleep->lock_timeout_logged = false;
            m_sleep->idle_event.Clear(m_system.Kernel());
            LOG_INFO(Service_AM, "am: sleep sequence {} requested by pid {}", m_sleep->sequence_id,
                     requester ? requester->aruid.pid : 0);
        }
    }
    if (shutdown) {
        // 0x08577e3c: the same sequence, as a power off.
        LOG_INFO(Service_AM, "am: sleep is disabled: the sleep sequence runs as a shutdown");
        m_system.GetAppletManager().RequestPower(AppletManager::PowerRequest::Shutdown);
        R_SUCCEED();
    }
    this->RequestUpdate();
    R_SUCCEED();
}

void WindowSystem::UpdateSleepSequenceLocked() {
    const auto now = std::chrono::steady_clock::now();
    auto& sleep = *m_sleep;

    switch (sleep.stage) {
    case SleepSequence::Stage::Awake:
    case SleepSequence::Stage::Asleep:
        return;
    case SleepSequence::Stage::Requested: {
        sleep.stage = SleepSequence::Stage::Preparing;
        sleep.settle_deadline = now + SleepSequence::SettleTime;
        sleep.lock_deadline = now + SleepSequence::SleepLockTimeout;
        size_t holders = 0;
        for (const auto& [aruid, applet] : m_applets) {
            std::scoped_lock lk{applet->lock};
            if (applet->process_released || applet->sleep_lock_count == 0) {
                continue;
            }
            // 0x085542b4: the request carries the sequence id; the release acknowledges it.
            applet->sleep_prepare_request_id = sleep.sequence_id;
            applet->lifecycle_manager.RequestToPrepareSleep(m_system.Kernel());
            ++holders;
            LOG_INFO(Service_AM, "am: -> pid {} RequestToPrepareSleep (it holds the sleep lock)",
                     applet->aruid.pid);
        }
        LOG_INFO(Service_AM,
                 "am: sleep sequence {}: the sleep window takes the foreground; {} sleep-lock "
                 "holder(s) to wait for",
                 sleep.sequence_id, holders);
        sleep.ScheduleUpdate(SleepSequence::SettleTime);
        return;
    }
    case SleepSequence::Stage::Preparing:
        break;
    }

    u64 waiting_pid = 0;
    size_t waiting = 0;
    for (const auto& [aruid, applet] : m_applets) {
        std::scoped_lock lk{applet->lock};
        if (!applet->process_released && !applet->process_exiting &&
            applet->sleep_prepare_request_id == sleep.sequence_id) {
            waiting_pid = applet->aruid.pid;
            ++waiting;
        }
    }
    if (waiting > 0 && now < sleep.lock_deadline) {
        if (!sleep.lock_timeout_logged) {
            sleep.lock_timeout_logged = true;
            LOG_INFO(Service_AM, "am: sleep deferred: {} applet(s) hold the sleep lock (pid {})",
                     waiting, waiting_pid);
        }
        sleep.ScheduleUpdate(std::chrono::milliseconds{500});
        return;
    }
    if (waiting > 0) {
        LOG_WARNING(Service_AM,
                    "unknown contract: am sleep lock timeout: pid {} did not release the sleep "
                    "lock in {} s; the console sleeps anyway",
                    waiting_pid, SleepSequence::SleepLockTimeout.count());
    }
    if (now < sleep.settle_deadline) {
        sleep.ScheduleUpdate(sleep.settle_deadline - now);
        return;
    }
    this->EnterSleepLocked();
}

void WindowSystem::EnterSleepLocked() {
    auto& sleep = *m_sleep;
    sleep.stage = SleepSequence::Stage::Asleep;
    LOG_INFO(Service_AM, "am: sleep sequence {}: the console sleeps", sleep.sequence_id);
    // A console's omm EnterSleepAndWait: spsm walks the psc modules down, the SoC sleeps.
    PSC::GetPmStateController().DispatchSequence(m_system.Kernel(), SleepStates);
    m_system.GetAppletManager().OnSleepStateChanged(true);
}

void WindowSystem::WakeLocked(const char* reason) {
    auto& sleep = *m_sleep;
    const bool was_asleep = sleep.stage == SleepSequence::Stage::Asleep;
    if (was_asleep) {
        PSC::GetPmStateController().DispatchSequence(m_system.Kernel(), WakeStates);
        ++sleep.wakeup_count;
        m_system.GetAppletManager().OnSleepStateChanged(false);
    }
    LOG_INFO(Service_AM, "am: sleep sequence {}: {} ({}); wake count {}", sleep.sequence_id,
             was_asleep ? "the console wakes" : "cancelled before the console slept", reason,
             sleep.wakeup_count);
    sleep.stage = SleepSequence::Stage::Awake;
    if (was_asleep) {
        auto& hid = m_system.HIDCore();
        const auto* handheld = hid.GetEmulatedController(Core::HID::NpadIdType::Handheld);
        const auto* player1 = hid.GetEmulatedController(Core::HID::NpadIdType::Player1);
        sleep.swallow_home =
            handheld->GetHomeButtons().home.Value() || player1->GetHomeButtons().home.Value();
        sleep.swallow_capture = handheld->GetCaptureButtons().capture.Value() ||
                                player1->GetCaptureButtons().capture.Value();
    }
    // The applet that asked to be told (StartSleepSequence's flag) hears the end (0x0853db74).
    if (const auto requester = sleep.requester.lock(); requester) {
        std::scoped_lock lk{requester->lock};
        if (!requester->process_released) {
            requester->lifecycle_manager.PushUnorderedMessage(m_system.Kernel(),
                                                              AppletMessage::FinishedSleepSequence);
            LOG_INFO(Service_AM, "am: -> pid {} FinishedSleepSequence", requester->aruid.pid);
        }
    }
    sleep.requester.reset();
    m_system.CoreTiming().UnscheduleEvent(sleep.timer_event);

    for (const auto& [aruid, applet] : m_applets) {
        std::scoped_lock lk{applet->lock};
        if (applet->sleep_prepare_request_id != 0) {
            applet->sleep_prepare_request_id = 0;
            applet->lifecycle_manager.ClearRequestToPrepareSleep(m_system.Kernel());
        }
        // A transient release lets one sleep through; the lock comes back after it.
        if (applet->sleep_lock_count > 0 && !applet->process_released) {
            this->GrantSleepLockLocked(*applet);
        }
    }
    sleep.idle_event.Signal(m_system.Kernel());
    // The press that woke the console is the user's: the idle timer starts over.
    if (auto* idle = OMM::GetIdlePolicy()) {
        idle->ReportUserIsActive();
    }
    if (m_event_observer) {
        m_event_observer->RequestUpdate();
    }
}

void WindowSystem::GetIdleHandlingContext(OMM::IdleHandlingContext& out_context) {
    std::scoped_lock lk{m_lock};
    out_context = {};
    out_context.sequence_running =
        m_sleep->IsActive() || m_shutdown_stage != ShutdownStage::None;
    Applet* const front = m_foreground_requested_applet;
    // The timer starts once HOME and its overlay are up (as a console's idle has nothing to do
    // before HOME's first SetHandlingContext).
    if (!m_system.GetAppletManager().IsHomeSession() || front == nullptr ||
        m_home_menu == nullptr || m_overlay_display == nullptr) {
        return;
    }
    std::scoped_lock lk2{front->lock};
    out_context.valid = true;
    out_context.sleep_handheld_s = front->auto_sleep_time_override[0];
    out_context.sleep_console_s = front->auto_sleep_time_override[1];
    out_context.dim_handheld_s = front->auto_sleep_time_override[2];
    out_context.dim_console_s = front->auto_sleep_time_override[3];
    out_context.extension = static_cast<u32>(front->idle_time_detection_extension);
    out_context.auto_sleep_disabled = front->auto_sleep_disabled;
    out_context.media_playing = front->media_playback_state;
}

// On am's thread (WindowSystem::Update), outside its lock: idle:sys loads its settings once HOME
// and its overlay are up, from here rather than from its timer's host thread.
void WindowSystem::LoadIdleSettingsIfReady() {
    if (m_idle_settings_requested.load()) {
        return;
    }
    {
        std::scoped_lock lk{m_lock};
        if (!m_system.GetAppletManager().IsHomeSession() || m_home_menu == nullptr ||
            m_overlay_display == nullptr) {
            return;
        }
    }
    if (auto* idle = OMM::GetIdlePolicy()) {
        m_idle_settings_requested = true;
        idle->LoadSettingsOnce();
    }
}

// idle's auto-sleep time: on a console omm's idle signals it, spsm notifies am (notification 3,
// 0x0859357c) and am sets AutoPowerDown for the applets that listen (0x0855456c); HOME answers
// with the sleep sequence.
void WindowSystem::OnAutoPowerDown() {
    std::scoped_lock lk{m_lock};
    if (m_home_menu == nullptr || m_sleep->IsActive()) {
        return;
    }
    LOG_WARNING(Service_AM, "unknown contract: am AutoPowerDown routing (sent to the HOME menu "
                            "only)");
    {
        std::scoped_lock lk2{m_home_menu->lock};
        m_home_menu->lifecycle_manager.RequestAutoPowerDown(m_system.Kernel());
    }
    LOG_INFO(Service_AM, "am: -> pid {} AutoPowerDown", m_home_menu->aruid.pid);
    if (m_event_observer) {
        m_event_observer->RequestUpdate();
    }
}

void WindowSystem::OnPowerButtonShortPressLocked() {
    switch (m_sleep->stage) {
    case SleepSequence::Stage::Asleep:
    case SleepSequence::Stage::Preparing:
        this->WakeLocked("power button");
        return;
    case SleepSequence::Stage::Requested:
        return;
    case SleepSequence::Stage::Awake:
        break;
    }
    if (m_home_menu == nullptr) {
        LOG_INFO(Service_AM, "am: power button ignored: no home menu is running");
        return;
    }
    // spsm's notifications 5-7 are the power button's presses; am hands a short one to each applet
    // whose listener takes it (a per-applet flag, listener +0x72, set from the applet's kind, not
    // traced: 0x085537b0); the HOME menu is the one that answers it with sleep.
    LOG_WARNING(Service_AM, "unknown contract: am DetectShortPressingPowerButton routing (sent to "
                            "the HOME menu only)");
    this->PushButtonMessageLocked(m_home_menu, AppletMessage::DetectShortPressingPowerButton);
}

// A sleeping console hears only the power button (which wakes it); the HOME or Capture press
// that woke it is not a press for the applets either (HOME shows its lock screen instead).
bool WindowSystem::SwallowButtonAroundSleepLocked(SystemButtonType type) {
    auto& sleep = *m_sleep;
    const bool is_home = type == SystemButtonType::HomeButtonShortPressing ||
                         type == SystemButtonType::HomeButtonLongPressing;
    const bool is_capture = type == SystemButtonType::CaptureButtonShortPressing ||
                            type == SystemButtonType::CaptureButtonLongPressing;
    if (!is_home && !is_capture) {
        return false;
    }
    if (sleep.stage == SleepSequence::Stage::Asleep) {
        return true;
    }
    bool& swallow = is_home ? sleep.swallow_home : sleep.swallow_capture;
    if (!swallow) {
        return false;
    }
    swallow = false;
    LOG_INFO(Service_AM, "am: the {} press that woke the console goes no further",
             is_home ? "HOME" : "Capture");
    return true;
}

bool WindowSystem::IsSleepEnabled() {
    std::scoped_lock lk{m_lock};
    return m_sleep->IsSleepEnabled();
}

bool WindowSystem::IsRebootEnabled() {
    std::scoped_lock lk{m_lock};
    return m_sleep->reboot_enabled;
}

void WindowSystem::DisableSleepTillShutdown() {
    std::scoped_lock lk{m_lock};
    m_sleep->enabled = false;
    m_sleep->suppressed_until_ns = 0;
    m_sleep->reboot_enabled = false;
}

Result WindowSystem::SuppressDisablingSleepTemporarily(s64 duration_ns) {
    R_UNLESS(duration_ns <= MaxSleepSuppressionNs, ResultInvalidParameters);
    std::scoped_lock lk{m_lock};
    if (!m_sleep->enabled) {
        m_sleep->suppressed_until_ns = NowNs() + duration_ns;
    }
    R_SUCCEED();
}

bool WindowSystem::IsDisablingSleepSuppressed() {
    std::scoped_lock lk{m_lock};
    return m_sleep->IsDisablingSleepSuppressed();
}

u64 WindowSystem::GetWakeupCount() {
    std::scoped_lock lk{m_lock};
    return m_sleep->wakeup_count;
}

Kernel::KReadableEvent* WindowSystem::GetSleepSequenceIdleEvent() {
    return m_sleep->idle_event.GetHandle();
}

void WindowSystem::GrantSleepLockLocked(Applet& applet) {
    if (applet.sleep_lock_granted) {
        return;
    }
    applet.sleep_lock_granted = true;
    applet.sleep_lock_event.Signal(m_system.Kernel());
}

void WindowSystem::AcknowledgePrepareSleepLocked(Applet& applet) {
    if (applet.sleep_prepare_request_id == 0) {
        return;
    }
    LOG_INFO(Service_AM, "am: pid {} let sleep sequence {} through", applet.aruid.pid,
             applet.sleep_prepare_request_id);
    applet.sleep_prepare_request_id = 0;
    applet.lifecycle_manager.ClearRequestToPrepareSleep(m_system.Kernel());
    if (m_event_observer) {
        m_event_observer->RequestUpdate();
    }
}

void WindowSystem::RequestToAcquireSleepLock(Applet& applet) {
    std::scoped_lock lk{m_lock};
    std::scoped_lock lk2{applet.lock};
    ++applet.sleep_lock_count;
    applet.sleep_lock_enabled = true;
    // Granted while no sequence runs; one that runs grants it on wake.
    if (!m_sleep->IsActive()) {
        this->GrantSleepLockLocked(applet);
    }
}

Result WindowSystem::ReleaseSleepLock(Applet& applet) {
    std::scoped_lock lk{m_lock};
    std::scoped_lock lk2{applet.lock};
    R_UNLESS(applet.sleep_lock_count > 0, ResultSleepSequenceInvalidState);
    if (--applet.sleep_lock_count == 0) {
        applet.sleep_lock_enabled = false;
        applet.sleep_lock_granted = false;
        applet.sleep_lock_event.Clear(m_system.Kernel());
        this->AcknowledgePrepareSleepLocked(applet);
    }
    R_SUCCEED();
}

Result WindowSystem::ReleaseSleepLockTransiently(Applet& applet) {
    std::scoped_lock lk{m_lock};
    std::scoped_lock lk2{applet.lock};
    // Only an answer to RequestToPrepareSleep (0x08552158).
    R_UNLESS(applet.sleep_prepare_request_id != 0, ResultSleepSequenceInvalidState);
    applet.sleep_lock_granted = false;
    applet.sleep_lock_event.Clear(m_system.Kernel());
    this->AcknowledgePrepareSleepLocked(applet);
    R_SUCCEED();
}

} // namespace Service::AM

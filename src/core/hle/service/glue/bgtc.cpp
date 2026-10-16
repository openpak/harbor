// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>
#include "common/logging.h"
#include "core/core.h"
#include "core/hle/service/acc/openpak_account.h"
#include "core/hle/service/acc/profile_manager.h"
#include "core/hle/service/glue/bgtc.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_readable_event.h"
#include "core/hle/service/ipc_helpers.h"

namespace Service::Glue {

BGTC_T::BGTC_T(Core::System& system_) : ServiceFramework{system_, "bgtc:t"} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {100, &BGTC_T::OpenTaskService, "OpenTaskService"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

BGTC_T::~BGTC_T() = default;

void BGTC_T::OpenTaskService(HLERequestContext& ctx) {
    LOG_DEBUG(Service_BGTC, "called");

    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<ITaskService>(ctx, system);
}

ITaskService::ITaskService(Core::System& system_)
    : ServiceFramework{system_, "ITaskService"}, service_context{system_, "ITaskService"} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {1, &ITaskService::Succeed, "NotifyTaskStarting"},
        {2, &ITaskService::Succeed, "NotifyTaskFinished"},
        {3, &ITaskService::GetTriggerEvent, "GetTriggerEvent"},
        {4, &ITaskService::IsInHalfAwake, "IsInHalfAwake"},
        {5, &ITaskService::Succeed, "NotifyClientName"},
        {6, &ITaskService::IsInFullAwake, "IsInFullAwake"},
        {11, &ITaskService::ScheduleTask, "ScheduleTask"},
        {12, nullptr, "GetScheduledTaskInterval"},
        {13, &ITaskService::Succeed, "UnscheduleTask"},
        {14, &ITaskService::GetScheduleEvent, "GetScheduleEvent"},
        {15, &ITaskService::SchedulePeriodicTask, "SchedulePeriodicTask"},
        {16, nullptr, "Unknown16"},
        {101, nullptr, "GetOperationMode"},
        {102, nullptr, "WillDisconnectNetworkWhenEnteringSleep"},
        {103, nullptr, "WillStayHalfAwakeInsteadSleep"},
        {200, nullptr, "Unknown200"},
    };
    // clang-format on

    RegisterHandlers(functions);
    trigger_event = service_context.CreateEvent("ITaskService:TriggerEvent");
    schedule_event = service_context.CreateEvent("ITaskService:ScheduleEvent");
}

void ITaskService::Succeed(HLERequestContext& ctx) {
    LOG_DEBUG(Service_BGTC, "called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void ITaskService::IsInHalfAwake(HLERequestContext& ctx) {
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<bool>(false);
}

void ITaskService::IsInFullAwake(HLERequestContext& ctx) {
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<bool>(true);
}

void ITaskService::GetTriggerEvent(HLERequestContext& ctx) {
    // [OpenPak] A task that only waits on its trigger (friends) runs when bgtc lets it: here at
    // once and then every minute, until the module asks for a schedule of its own.
    if (!schedule) {
        Schedule(1, 60);
    }
    IPC::ResponseBuilder rb{ctx, 2, 1};
    rb.Push(ResultSuccess);
    rb.PushCopyObjects(ctx, &trigger_event->GetReadableEvent());
}

void ITaskService::GetScheduleEvent(HLERequestContext& ctx) {
    IPC::ResponseBuilder rb{ctx, 2, 1};
    rb.Push(ResultSuccess);
    rb.PushCopyObjects(ctx, &schedule_event->GetReadableEvent());
}

// [OpenPak] bgtc on a console runs the modules' background tasks (news and bcat downloads,
// friends sync, play reports) on the schedule they ask for; Harbor fires their schedule and
// trigger events the same way: once after the first delay, then every interval.
struct ITaskService::ScheduleState {
    std::mutex mutex;
    bool alive{true};
    u64 generation{};
    Kernel::KernelCore* kernel{};
    Kernel::KEvent* trigger{};
    Kernel::KEvent* schedule{};
};

void ITaskService::Schedule(u32 first_seconds, u32 interval_seconds) {
    if (!schedule) {
        schedule = std::make_shared<ScheduleState>();
        schedule->kernel = &system.Kernel();
        schedule->trigger = trigger_event;
        schedule->schedule = schedule_event;
    }
    u64 generation;
    {
        std::scoped_lock lk{schedule->mutex};
        generation = ++schedule->generation;
    }
    LOG_INFO(Service_BGTC, "task scheduled: first in {} s, then every {} s", first_seconds,
             interval_seconds);
    // The first run waits for the open user's console-server sign-in (a console has its account
    // caches at boot; Harbor signs in a few seconds after HOME starts): news, bcat and friends
    // ask the account service in that first run and give up for the session on a refusal.
    const std::string uid = Service::Account::OpenPakAccount::UidString(system.GetProfileManager().GetLastOpenedUser());
    std::thread{[state = schedule, generation, first_seconds, interval_seconds, uid] {
        u32 wait = std::max<u32>(first_seconds, 1);
        if (!uid.empty()) {
            Service::Account::OpenPakAccount::SignedInWithin(uid, std::chrono::seconds{60});
        }
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds{wait});
            std::scoped_lock lk{state->mutex};
            if (!state->alive || state->generation != generation) {
                return;
            }
            state->schedule->Signal(*state->kernel);
            state->trigger->Signal(*state->kernel);
            if (interval_seconds == 0) {
                return;
            }
            wait = interval_seconds;
        }
    }}.detach();
}

// 11: (u32 seconds) a task once.
void ITaskService::ScheduleTask(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto seconds = rp.Pop<u32>();
    Schedule(seconds, 0);
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

// 15: (u32 first delay, u32 interval, u32 flags) a task again and again.
void ITaskService::SchedulePeriodicTask(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto first = rp.Pop<u32>();
    const auto interval = rp.Pop<u32>();
    Schedule(first, interval);
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

ITaskService::~ITaskService() {
    if (schedule) {
        std::scoped_lock lk{schedule->mutex};
        schedule->alive = false;
    }
    service_context.CloseEvent(trigger_event);
    service_context.CloseEvent(schedule_event);
}

BGTC_SC::BGTC_SC(Core::System& system_) : ServiceFramework{system_, "bgtc:sc"} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {1, nullptr, "GetState"},
        {2, nullptr, "GetStateChangedEvent"},
        {3, nullptr, "NotifyEnteringHalfAwake"},
        {4, nullptr, "NotifyLeavingHalfAwake"},
        {5, nullptr, "SetIsUsingSleepUnsupportedDevices"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

BGTC_SC::~BGTC_SC() = default;

} // namespace Service::Glue

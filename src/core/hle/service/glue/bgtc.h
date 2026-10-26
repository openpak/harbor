// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/service.h"

namespace Core {
class System;
}

namespace Service::Glue {

class BGTC_T final : public ServiceFramework<BGTC_T> {
public:
    explicit BGTC_T(Core::System& system_);
    ~BGTC_T() override;

    void OpenTaskService(HLERequestContext& ctx);
};

class ITaskService final : public ServiceFramework<ITaskService> {
public:
    explicit ITaskService(Core::System& system_);
    ~ITaskService() override;

private:
    // [OpenPak] Enough of bgtc:t for the firmware's own programs (friends) to register their
    // background tasks: nothing is ever scheduled, so the events stay unsignalled.
    void Succeed(HLERequestContext& ctx);
    void EnableStayHalfAwake(HLERequestContext& ctx);
    void WillDisconnectNetworkWhenEnteringSleep(HLERequestContext& ctx);
    void WillStayHalfAwakeInsteadSleep(HLERequestContext& ctx);
    bool stay_half_awake_enabled{};
    void IsInHalfAwake(HLERequestContext& ctx);
    void IsInFullAwake(HLERequestContext& ctx);
    void GetTriggerEvent(HLERequestContext& ctx);
    void GetScheduleEvent(HLERequestContext& ctx);
    void ScheduleTask(HLERequestContext& ctx);
    void SchedulePeriodicTask(HLERequestContext& ctx);
    void Schedule(u32 first_seconds, u32 interval_seconds);

    // [OpenPak] The schedule a firmware module asked for, fired from a host thread that outlives
    // nothing: it checks `alive` under the lock before touching the events.
    struct ScheduleState;
    std::shared_ptr<ScheduleState> schedule;

    KernelHelpers::ServiceContext service_context;
    Kernel::KEvent* trigger_event;
    Kernel::KEvent* schedule_event;
};

class BGTC_SC final : public ServiceFramework<BGTC_SC> {
public:
    explicit BGTC_SC(Core::System& system_);
    ~BGTC_SC() override;
};

} // namespace Service::Glue

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/omm/idle_policy.h"
#include "core/hle/service/omm/policy_manager_system.h"

namespace Service::OMM {

IPolicyManagerSystem::IPolicyManagerSystem(Core::System& system_)
    : ServiceFramework{system_, "idle:sys"}, m_policy{std::make_unique<IdlePolicy>(system_)} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, D<&IPolicyManagerSystem::GetAutoPowerDownEvent>, "GetAutoPowerDownEvent"},
        {1, D<&IPolicyManagerSystem::IsAutoPowerDownRequested>, "IsAutoPowerDownRequested"},
        {2, D<&IPolicyManagerSystem::IsDimmed>, "Unknown2"},
        {3, &IPolicyManagerSystem::SetHandlingContext, "SetHandlingContext"},
        {4, D<&IPolicyManagerSystem::LoadAndApplySettings>, "LoadAndApplySettings"},
        {5, D<&IPolicyManagerSystem::ReportUserIsActive>, "ReportUserIsActive"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

IPolicyManagerSystem::~IPolicyManagerSystem() = default;

Result IPolicyManagerSystem::GetAutoPowerDownEvent(
    OutCopyHandle<Kernel::KReadableEvent> out_event) {
    LOG_DEBUG(Service_PTM, "called");
    *out_event = m_policy->GetAutoPowerDownEvent();
    R_SUCCEED();
}

Result IPolicyManagerSystem::IsAutoPowerDownRequested(Out<bool> out_is_requested) {
    *out_is_requested = m_policy->IsAutoPowerDownRequested();
    LOG_DEBUG(Service_PTM, "called, requested={}", *out_is_requested);
    R_SUCCEED();
}

// 23.0.1 omm answers 2 with the dimmed flag (0x085495d0 -> 0x08548a50).
Result IPolicyManagerSystem::IsDimmed(Out<bool> out_is_dimmed) {
    *out_is_dimmed = m_policy->IsDimmed();
    LOG_DEBUG(Service_PTM, "called, dimmed={}", *out_is_dimmed);
    R_SUCCEED();
}

// am is HLE and hands IdlePolicy its context directly; a guest caller is not expected.
void IPolicyManagerSystem::SetHandlingContext(HLERequestContext& ctx) {
    LOG_WARNING(Service_PTM,
                "unknown contract: idle:sys 3 SetHandlingContext from a guest; ignored");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

Result IPolicyManagerSystem::LoadAndApplySettings() {
    LOG_INFO(Service_PTM, "called");
    m_policy->LoadAndApplySettings();
    R_SUCCEED();
}

Result IPolicyManagerSystem::ReportUserIsActive() {
    LOG_DEBUG(Service_PTM, "called");
    m_policy->ReportUserIsActive();
    R_SUCCEED();
}

} // namespace Service::OMM

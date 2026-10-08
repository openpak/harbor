// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/core.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/psc/pm_module.h"

namespace Service::PSC {

IPmModule::IPmModule(Core::System& system_)
    : ServiceFramework{system_, "IPmModule"}, m_context{system_, "IPmModule"}, m_event{m_context} {
    // clang-format off
        static const FunctionInfo functions[] = {
            {0, D<&IPmModule::Initialize>, "Initialize"},
            {1, D<&IPmModule::GetRequest>, "GetRequest"},
            {2, D<&IPmModule::Acknowledge>, "Acknowledge"},
            {3, D<&IPmModule::Finalize>, "Finalize"},
            {4, D<&IPmModule::AcknowledgeEx>, "AcknowledgeEx"},
        };
    // clang-format on

    RegisterHandlers(functions);
}

IPmModule::~IPmModule() {
    GetPmStateController().Unregister(this);
}

Result IPmModule::Initialize(OutCopyHandle<Kernel::KReadableEvent> out_event, u32 module_id,
                             InBuffer<BufferAttr_HipcMapAlias> dependencies) {
    LOG_INFO(Service_PSC, "called, module_id={} dependencies={}", module_id,
             dependencies.size() / sizeof(u32));
    {
        std::scoped_lock lk{m_lock};
        m_module_id = module_id;
        if (!m_initialized) {
            m_initialized = true;
            GetPmStateController().Register(this);
        }
    }
    *out_event = m_event.GetHandle();
    R_SUCCEED();
}

Result IPmModule::GetRequest(Out<PmState> out_state, Out<u32> out_flags) {
    std::scoped_lock lk{m_lock};
    LOG_DEBUG(Service_PSC, "called, module_id={} state={}", m_module_id, PmStateName(m_state));
    *out_state = m_state;
    *out_flags = m_flags;
    R_SUCCEED();
}

Result IPmModule::Acknowledge() {
    std::scoped_lock lk{m_lock};
    LOG_DEBUG(Service_PSC, "called, module_id={}", m_module_id);
    m_acknowledged = true;
    R_SUCCEED();
}

Result IPmModule::Finalize() {
    LOG_INFO(Service_PSC, "called, module_id={}", m_module_id);
    GetPmStateController().Unregister(this);
    std::scoped_lock lk{m_lock};
    m_initialized = false;
    R_SUCCEED();
}

Result IPmModule::AcknowledgeEx(PmState state) {
    std::scoped_lock lk{m_lock};
    LOG_DEBUG(Service_PSC, "called, module_id={} state={}", m_module_id, PmStateName(state));
    if (state != m_state) {
        LOG_WARNING(Service_PSC, "psc: module {} acknowledged {} while {} is pending", m_module_id,
                    PmStateName(state), PmStateName(m_state));
    }
    m_acknowledged = true;
    R_SUCCEED();
}

void IPmModule::Request(Kernel::KernelCore& kernel_core, PmState state, u32 flags) {
    {
        std::scoped_lock lk{m_lock};
        m_state = state;
        m_flags = flags;
        m_acknowledged = false;
    }
    m_event.Signal(kernel_core);
}

bool IPmModule::IsAcknowledged() const {
    std::scoped_lock lk{m_lock};
    return m_acknowledged;
}

} // namespace Service::PSC

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <mutex>

#include "core/hle/service/cmif_types.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/os/event.h"
#include "core/hle/service/psc/pm_state.h"
#include "core/hle/service/service.h"

namespace Service::PSC {

// psc:m GetPmModule. Layouts as libstratosphere's psc::sf::IPmModule: 0 Initialize(module id,
// dependency list) -> event, 1 GetRequest -> (state, flags), 2 Acknowledge, 3 Finalize,
// 4 AcknowledgeEx(state).
class IPmModule final : public ServiceFramework<IPmModule> {
public:
    explicit IPmModule(Core::System& system_);
    ~IPmModule() override;

    // From PmStateController::Dispatch.
    void Request(Kernel::KernelCore& kernel_core, PmState state, u32 flags);
    [[nodiscard]] bool IsAcknowledged() const;

private:
    Result Initialize(OutCopyHandle<Kernel::KReadableEvent> out_event, u32 module_id,
                      InBuffer<BufferAttr_HipcMapAlias> dependencies);
    Result GetRequest(Out<PmState> out_state, Out<u32> out_flags);
    Result Acknowledge();
    Result Finalize();
    Result AcknowledgeEx(PmState state);

    KernelHelpers::ServiceContext m_context;
    Event m_event;
    mutable std::mutex m_lock;
    bool m_initialized{};
    u32 m_module_id{};
    PmState m_state{PmState::FullAwake};
    u32 m_flags{};
    bool m_acknowledged{true};
};

} // namespace Service::PSC

// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <algorithm>
#include <string>

#include "common/logging.h"
#include "core/hle/service/psc/pm_module.h"
#include "core/hle/service/psc/pm_state.h"

namespace Service::PSC {

const char* PmStateName(PmState state) {
    switch (state) {
    case PmState::FullAwake:
        return "FullAwake";
    case PmState::MinimumAwake:
        return "MinimumAwake";
    case PmState::SleepReady:
        return "SleepReady";
    case PmState::EssentialServicesSleepReady:
        return "EssentialServicesSleepReady";
    case PmState::EssentialServicesAwake:
        return "EssentialServicesAwake";
    case PmState::ShutdownReady:
        return "ShutdownReady";
    default:
        return "Invalid";
    }
}

void PmStateController::Register(IPmModule* module) {
    std::scoped_lock lk{m_lock};
    m_modules.push_back(module);
}

void PmStateController::Unregister(IPmModule* module) {
    std::scoped_lock lk{m_lock};
    std::erase(m_modules, module);
}

void PmStateController::Dispatch(Kernel::KernelCore& kernel, PmState state, u32 flags) {
    std::scoped_lock lk{m_lock};
    size_t unacknowledged = 0;
    for (auto* module : m_modules) {
        if (!module->IsAcknowledged()) {
            ++unacknowledged;
        }
        module->Request(kernel, state, flags);
    }
    LOG_INFO(Service_PSC, "psc: PmState {} -> {} ({} module(s){})", PmStateName(m_state),
             PmStateName(state), m_modules.size(),
             unacknowledged ? fmt::format(", {} did not acknowledge the last one", unacknowledged)
                            : std::string{});
    m_state = state;
}

void PmStateController::DispatchSequence(Kernel::KernelCore& kernel,
                                         std::span<const PmState> states) {
    for (const auto state : states) {
        Dispatch(kernel, state);
    }
}

PmState PmStateController::GetState() const {
    std::scoped_lock lk{m_lock};
    return m_state;
}

PmStateController& GetPmStateController() {
    static PmStateController controller;
    return controller;
}

} // namespace Service::PSC

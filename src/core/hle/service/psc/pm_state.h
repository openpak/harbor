// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <chrono>
#include <mutex>
#include <span>
#include <vector>

#include "common/common_types.h"

namespace Kernel {
class KernelCore;
}

namespace Service::PSC {

// nn::psc::PmState. A console's spsm walks every registered PmModule through these on sleep
// (FullAwake -> MinimumAwake -> SleepReady -> EssentialServicesSleepReady) and back on wake
// (EssentialServicesAwake -> MinimumAwake -> FullAwake); ShutdownReady before a power off.
enum class PmState : u32 {
    FullAwake = 0,
    MinimumAwake = 1,
    SleepReady = 2,
    EssentialServicesSleepReady = 3,
    EssentialServicesAwake = 4,
    ShutdownReady = 5,
    Invalid = 6,
};

const char* PmStateName(PmState state);

class IPmModule;

// psc's module registry. Here am stands in for spsm (am's sleep sequence, where a console's am
// calls omm EnterSleepAndWait): it dispatches the states, every initialized module gets its event
// signalled and reads the state with GetRequest. HLE services do not take part, so nothing waits
// for the acknowledgements; a missing one is logged.
class PmStateController {
public:
    // How long one state waits for its modules before the sequence goes on without them.
    static constexpr std::chrono::milliseconds AcknowledgeTimeout{3000};

    void Register(IPmModule* module);
    void Unregister(IPmModule* module);

    // Sends `state` to every module, in registration order (a console orders them by their
    // dependency lists; no guest module here depends on another).
    void Dispatch(Kernel::KernelCore& kernel, PmState state, u32 flags = 0);

    // A whole transition, one state after the other.
    void DispatchSequence(Kernel::KernelCore& kernel, std::span<const PmState> states);

    [[nodiscard]] PmState GetState() const;

private:
    mutable std::mutex m_lock;
    std::vector<IPmModule*> m_modules;
    PmState m_state{PmState::FullAwake};
};

PmStateController& GetPmStateController();

} // namespace Service::PSC

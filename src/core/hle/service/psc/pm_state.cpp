// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>

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
    m_stalled.erase(module);
    std::erase(m_modules, module);
}

void PmStateController::Dispatch(Kernel::KernelCore& kernel, PmState state, u32 flags) {
    std::vector<IPmModule*> pending;
    {
        std::scoped_lock lk{m_lock};
        for (auto* module : m_modules) {
            // A module that timed out earlier and has not acknowledged since is not waited for
            // again (a program that died or was parked after a fatal would cost every state
            // the full timeout); it is requested all the same.
            const bool stalled = m_stalled.contains(module) && !module->IsAcknowledged();
            if (!stalled) {
                m_stalled.erase(module);
            }
            module->Request(kernel, state, flags);
            if (!stalled) {
                pending.push_back(module);
            }
        }
        LOG_INFO(Service_PSC, "psc: PmState {} -> {} ({} module(s))", PmStateName(m_state),
                 PmStateName(state), m_modules.size());
        m_state = state;
    }

    // psc moves to the next state only once every module has acknowledged this one. A module
    // signalled again before it read the last request sees only the newest state (GetRequest
    // returns one): olsc missed MinimumAwake that way, never closed its save-data prohibiters and
    // aborted on FullAwake releasing what it never took. A module that never acknowledges (dead,
    // or waiting on a lock the caller holds) delays the transition by AcknowledgeTimeout only.
    const auto deadline = std::chrono::steady_clock::now() + AcknowledgeTimeout;
    while (!pending.empty()) {
        {
            std::scoped_lock lk{m_lock};
            // Finalize/destruction takes a module out of m_modules: its pointer may be gone.
            std::erase_if(pending, [this](IPmModule* module) {
                return std::find(m_modules.begin(), m_modules.end(), module) == m_modules.end() ||
                       module->IsAcknowledged();
            });
        }
        if (pending.empty()) {
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            LOG_WARNING(Service_PSC,
                        "unknown contract: psc: {} module(s) did not acknowledge {} in {} ms; "
                        "continuing",
                        pending.size(), PmStateName(state), AcknowledgeTimeout.count());
            std::scoped_lock lk{m_lock};
            m_stalled.insert(pending.begin(), pending.end());
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
}

PmStateController::~PmStateController() {
    if (m_worker.joinable()) {
        m_worker.join();
    }
}

void PmStateController::DispatchSequenceAsync(Kernel::KernelCore& kernel,
                                              std::span<const PmState> states) {
    if (m_worker.joinable()) {
        m_worker.join(); // the previous sequence; a new one waits behind it anyway
    }
    m_worker = std::thread{[this, &kernel, states] { DispatchSequence(kernel, states); }};
}

void PmStateController::DispatchSequence(Kernel::KernelCore& kernel,
                                         std::span<const PmState> states) {
    std::scoped_lock sequence{m_sequence_lock};
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

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <condition_variable>
#include <list>
#include <mutex>
#include <optional>
#include <vector>

#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "core/hle/result.h"
#include "core/hle/service/hle_ipc.h"
#include "core/hle/service/os/multi_wait.h"
#include "core/hle/service/os/mutex.h"

namespace Core {
class System;
}

namespace Kernel {
class KEvent;
class KServerPort;
class KServerSession;
} // namespace Kernel

namespace Service {

class Port;
class Session;

/// [Harbor] The process id of the client whose request this host thread is serving (also while
/// one of its sessions is being destroyed), or nullopt outside of request handling. Lets a service
/// that calls into another service's per-process state (ssl into bsd) name the right process.
std::optional<u64> GetCurrentRequestProcessId();

class ServerManager {
public:
    explicit ServerManager(Core::System& system);
    ~ServerManager();

    Result RegisterSession(Kernel::KServerSession* session,
                           std::shared_ptr<SessionRequestManager> manager);
    Result RegisterNamedService(const std::string& service_name,
                                SessionRequestHandlerFactory&& handler_factory,
                                u32 max_sessions = 64);
    Result RegisterNamedService(const std::string& service_name,
                                std::shared_ptr<SessionRequestHandler>&& handler,
                                u32 max_sessions = 64);
    Result ManageNamedPort(const std::string& service_name,
                           SessionRequestHandlerFactory&& handler_factory, u32 max_sessions = 64);
    Result ManageDeferral(Kernel::KEvent** out_event);

    /// [Harbor] Re-run this server's deferred requests at least every `period` while any is
    /// waiting. For requests that wait on host state (a bsd poll or recv parked until a host
    /// socket is ready) rather than on a guest event. Idempotent; creates the deferral event if
    /// the server has none.
    void EnableDeferralHeartbeat(std::chrono::nanoseconds period);

    /// [Harbor] Re-run the deferred requests now (an eventfd write that may end a parked poll).
    void SignalDeferral();

    Result LoopProcess();
    void StartAdditionalHostThreads(const char* name, size_t num_threads);

    static void RunServer(std::unique_ptr<ServerManager>&& server);

private:
    void LinkToDeferredList(MultiWaitHolder* holder);
    void LinkDeferred();
    MultiWaitHolder* WaitSignaled();
    Result Process(MultiWaitHolder* holder);
    bool WaitAndProcessImpl();
    Result LoopProcessImpl();

    Result OnPortEvent(Port* port);
    Result OnSessionEvent(Session* session);
    Result OnDeferralEvent();
    Result CompleteSyncRequest(Session* session);

private:
    void DestroySession(Session* session);
    void HeartbeatLoop();

private:
    Core::System& m_system;
    Mutex m_selection_mutex;

    // Events
    Kernel::KEvent* m_wakeup_event{};
    Kernel::KEvent* m_deferral_event{};

    // Deferred wait list
    std::mutex m_deferred_list_mutex{};
    MultiWait m_deferred_list{};

    // Guest state tracking
    MultiWait m_multi_wait{};
    Common::IntrusiveListBaseTraits<Port>::ListType m_servers{};
    Common::IntrusiveListBaseTraits<Session>::ListType m_sessions{};
    std::list<Session*> m_deferred_sessions{};
    std::optional<MultiWaitHolder> m_wakeup_holder{};
    std::optional<MultiWaitHolder> m_deferral_holder{};

    // Deferral heartbeat (Harbor)
    std::mutex m_heartbeat_mutex{};
    std::condition_variable m_heartbeat_cv{};
    std::chrono::nanoseconds m_heartbeat_period{};
    bool m_heartbeat_stop{};
    bool m_owns_deferral_event{};
    std::jthread m_heartbeat_thread{};

    // Host state tracking
    Common::Event m_stopped{};
    std::vector<std::jthread> m_threads{};
    std::stop_source m_stop_source{};
};

} // namespace Service

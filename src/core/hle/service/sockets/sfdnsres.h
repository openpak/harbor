// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

#include "common/socket_types.h"
#include "core/hle/service/service.h"
#include "core/hle/service/sockets/sockets.h"

namespace Core {
class System;
}

namespace Service::Sockets {

class SFDNSRES final : public ServiceFramework<SFDNSRES> {
public:
    explicit SFDNSRES(Core::System& system_);
    ~SFDNSRES() override;

    // Makes eventfd `fd` of process `process_id` readable (adds 1 to its counter); false when it
    // is not an eventfd of that process. Installed by bsd; thread-safe, called from the IPC
    // thread and from the resolver worker.
    using EventFdSignal = std::function<bool(u64 process_id, s32 fd)>;
    static void SetEventFdSignal(EventFdSignal signal);

private:
    void GetHostByNameRequest(HLERequestContext& ctx);
    void GetGaiStringErrorRequest(HLERequestContext& ctx);
    void GetHostByNameRequestWithOptions(HLERequestContext& ctx);
    void GetAddrInfoRequest(HLERequestContext& ctx);
    void GetAddrInfoRequestWithOptions(HLERequestContext& ctx);
    void ResolverSetOptionRequest(HLERequestContext& ctx);
    void ReportTelemetry(HLERequestContext& ctx);
    void QueryAddrInfoRequestWithOptions(HLERequestContext& ctx);
    void QueryNameInfoRequestWithOptions(HLERequestContext& ctx);
    void FetchAddrInfoRequestWithOptions(HLERequestContext& ctx);
    void FetchNameInfoRequestWithOptions(HLERequestContext& ctx);
    void CancelQuery(HLERequestContext& ctx);

    static std::mutex& EventFdMutex() {
        static std::mutex mutex;
        return mutex;
    }
    static EventFdSignal& EventFdStorage() {
        static EventFdSignal signal;
        return signal;
    }
    static void SignalEventFd(u64 process_id, s32 fd);

    struct Query {
        std::string host;
        std::variant<std::vector<Network::AddrInfo>, GetAddrInfoError> result{
            GetAddrInfoError::FAIL};
        bool ready = false;
        bool name_info = false;
    };
    struct PendingLookup {
        u32 handle = 0;
        std::string host;
        std::optional<std::string> service;
        u64 process_id = 0;
        s32 event_fd = -1;
    };

    u32 NewQueryHandle();
    void WorkerLoop();

    std::mutex query_mutex;
    std::condition_variable query_cv;
    std::unordered_map<u32, Query> queries;
    std::deque<PendingLookup> pending;
    u32 next_query = 0;
    bool stop_worker = false;
    std::thread worker;
};

class DNS_PRIV final : public ServiceFramework<DNS_PRIV> {
public:
    explicit DNS_PRIV(Core::System& system_);
    ~DNS_PRIV() override;
};

} // namespace Service::Sockets

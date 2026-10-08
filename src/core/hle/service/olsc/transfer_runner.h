// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// olsc's transfer task queue (olsc-transfer.md §4): RSC 12 queues an upload, RSC 1 a download;
// one task runs at a time on a host thread, through the console protocol (openpak/olsc_transfer)
// and fs's transfer service; ITransferTaskListController reads the queue and its start/end
// events fire as tasks start and end.

#pragma once

#include <array>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "common/common_types.h"
#include "common/uuid.h"
#include "core/openpak/olsc_transfer.h"

namespace Core {
class System;
}

namespace OpenPak {
class AsyncWorker;
class HostEvent;
} // namespace OpenPak

namespace Service::OLSC {

struct TransferTask {
    u64 id{};
    Common::UUID user{};
    u64 application_id{};
    u8 type{};   ///< 0 upload user save, 1 download user save (2/3 device: not built)
    u8 flag{};   ///< detail +0x21
    u8 state{};  ///< TransferTaskInfo +0x09 for a queued task: 0 waiting, 3 failed
    bool running{};
    u64 archive_id{};
    OpenPak::Olsc::Transfer::Progress progress{};
    u32 last_result{};
};

/// TransferTaskInfo (0x30), as fs-transfer/olsc-findings record it.
using TransferTaskInfo = std::array<u8, 0x30>;
TransferTaskInfo PackTaskInfo(const TransferTask& task);

/// The TTLC filters: 0 user-save tasks (types 0-1), 1 device-save tasks (2-3), 2 all.
bool MatchesFilter(const TransferTask& task, u8 filter);

class TransferQueue {
public:
    explicit TransferQueue(Core::System& system);
    ~TransferQueue();

    /// The session's queue, shared by every olsc object (made on first use, kept while any
    /// olsc service object lives).
    static std::shared_ptr<TransferQueue> For(Core::System& system);

    u64 RegisterUpload(const Common::UUID& user, u64 application_id, bool flag);
    /// The archive must be in the user's cached list (else nullopt: 2179-1200).
    std::optional<u64> RegisterDownload(const Common::UUID& user, u64 archive_id);

    std::vector<TransferTask> Tasks() const;
    /// DeleteTransferTask: a queued or finished task goes; a running one stays.
    void Delete(u64 task_id);
    /// RaiseTransferTaskPriority: a queued task moves to the front.
    void Raise(u64 task_id);
    std::optional<TransferTask> Current() const;

    void AddStartEvent(std::shared_ptr<OpenPak::HostEvent> event);
    void AddEndEvent(std::shared_ptr<OpenPak::HostEvent> event);

private:
    void Run(std::stop_token stop);
    void Execute(TransferTask& task);
    void Raise(std::vector<std::weak_ptr<OpenPak::HostEvent>>& events);

    Core::System& system;
    mutable std::mutex mutex;
    std::condition_variable_any wake;
    std::deque<TransferTask> tasks;
    u64 next_id{1};
    std::vector<std::weak_ptr<OpenPak::HostEvent>> start_events;
    std::vector<std::weak_ptr<OpenPak::HostEvent>> end_events;
    std::jthread thread;
};

} // namespace Service::OLSC

// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// olsc's transfer task queue and its autonomy agent (olsc-transfer.md §4, olsc-autonomy.md):
// RSC 0/12 queue an upload, RSC 1 a download; the agent queues automatic ones (a periodic sweep
// per user, an application's exit, IDaemonController 4). One task runs at a time on a host
// thread, through the console protocol (openpak/olsc_transfer) and fs's transfer service;
// ITransferTaskListController reads the queue and its start/end events fire as tasks start and
// end.

#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "common/uuid.h"
#include "core/hle/result.h"
#include "core/openpak/olsc_transfer.h"

namespace Core {
class System;
}

namespace OpenPak {
class AsyncWorker;
class HostEvent;
namespace Olsc {
class DaemonSettings;
}
} // namespace OpenPak

namespace Service::OLSC {

/// Task ranks = the task database's two queues (0x1bcb40): `basic` for user requests, an
/// application's exit and pushes; `postponed` for the periodic sweep.
inline constexpr u8 RankBasic = 0x7f;
inline constexpr u8 RankPostponed = 0xbf;

struct TransferTask {
    u64 id{};
    Common::UUID user{};
    u64 application_id{};
    u8 type{};   ///< 0 upload user save, 1 download user save (2/3 device: not built)
    u8 flag{};   ///< detail +0x21
    u8 rank{RankBasic}; ///< detail +0x51, TransferTaskInfo +0x0a
    u8 state{};  ///< TransferTaskInfo +0x09: 0 queued, 2 failed, 3 done (kept until newness 0)
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

/// A local save's commit id and time stamp as olsc reads them (fsp 616 / 57): the extra data's
/// commit id, or the first 8 bytes of its SHA-256 when it has none (as fs exports it).
struct LocalSaveState {
    u64 commit_id{};
    s64 timestamp{};
};
std::optional<LocalSaveState> ReadLocalSave(Core::System& system, const Common::UUID& user,
                                            u64 application_id);

class TransferQueue {
public:
    explicit TransferQueue(Core::System& system);
    ~TransferQueue();

    /// The session's queue, shared by every olsc object (made on first use, kept while any
    /// olsc service object lives).
    static std::shared_ptr<TransferQueue> For(Core::System& system);

    /// An application started / exited. A console's am holds IDaemonController 10
    /// (IForbiddenSaveDataIndication) while an application runs; releasing it queues the title
    /// for every user (olsc-autonomy.md §2.3). No queue yet: nothing to do.
    static void OnApplicationStarted(Core::System& system, u64 application_id);
    static void OnApplicationExited(Core::System& system, u64 application_id);

    /// The daemon's settings and SeriesInfo (olsc's own system save on a console).
    std::shared_ptr<OpenPak::Olsc::DaemonSettings> Settings();

    u64 RegisterUpload(const Common::UUID& user, u64 application_id, bool flag,
                       u8 rank = RankBasic);
    /// The archive must be in the user's cached list (else nullopt: 2179-1200).
    std::optional<u64> RegisterDownload(const Common::UUID& user, u64 archive_id,
                                        u8 rank = RankBasic);

    /// The autonomy registrations (RSC 15 / RSC 25 behind 0x1c7a40 / 0x1ca6b0): the checks in
    /// the sysmodule's order and its 2179-16xx refusals.
    Result TryAutoUpload(const Common::UUID& user, u64 application_id, u8 rank);
    Result TryAutoDownload(const Common::UUID& user, u64 application_id, u8 rank);

    /// RSC 14: 0 in sync, 1 cloud newer, 2 local newer, 3 conflict (olsc-autonomy.md §4.1).
    u8 Newness(const Common::UUID& user, u64 application_id);
    /// After a cache refresh: SeriesInfo for a save the cloud already holds byte for byte
    /// (same commit and time stamp) that newness would call a conflict (§4.3).
    void Repair(const Common::UUID& user);

    /// TTLC 8 / IDaemonController 11 stoppers: while any is open no task starts / the autonomy
    /// agent registers nothing.
    void HoldExecution(bool hold);
    void HoldAutonomy(bool hold);

    /// IDaemonController 4: the user's periodic sweep is due now.
    void MakeDue(const Common::UUID& user);
    /// IDaemonController 1/3/6 turning a setting off: the queued tasks it covered go.
    void RemoveQueued(const Common::UUID& user, std::optional<u64> application_id, u8 type);

    std::vector<TransferTask> Tasks() const;
    /// DeleteTransferTask: a queued or finished task goes; a running one stays.
    void Delete(u64 task_id);
    /// RaiseTransferTaskPriority: a queued task moves to the front.
    void Raise(u64 task_id);
    std::optional<TransferTask> Current() const;
    /// GetTransferTaskProgress: {total bytes, bytes done, throughput in bits/s}; the running
    /// task's live counters, a queued or finished one's stored ones (throughput 0). Nullopt when
    /// there is no such task.
    std::optional<std::array<u64, 3>> ProgressOf(u64 task_id, u8 filter) const;

    void AddStartEvent(std::shared_ptr<OpenPak::HostEvent> event);
    void AddEndEvent(std::shared_ptr<OpenPak::HostEvent> event);

private:
    using Clock = std::chrono::steady_clock;

    static std::shared_ptr<TransferQueue> Existing(Core::System& system);
    void Run(std::stop_token stop);
    /// The autonomy agent's work for this turn: queued application exits, due users.
    void Autonomy();
    std::deque<TransferTask>::iterator NextLocked(Clock::time_point now);
    Clock::time_point NextWakeLocked() const;
    void LogWaitingLocked();
    void Execute(TransferTask& task);
    void Raise(std::vector<std::weak_ptr<OpenPak::HostEvent>>& events);

    Core::System& system;
    mutable std::mutex mutex;
    std::condition_variable_any wake;
    std::deque<TransferTask> tasks;
    u64 next_id{1};
    std::vector<std::weak_ptr<OpenPak::HostEvent>> start_events;
    std::vector<std::weak_ptr<OpenPak::HostEvent>> end_events;
    std::shared_ptr<OpenPak::Olsc::DaemonSettings> settings;
    std::set<u64> running_applications;
    std::deque<u64> exited_applications;
    std::map<std::string, Clock::time_point> next_sweep; ///< per user (uid string)
    bool users_known{};
    Clock::time_point postponed_gate{};
    Clock::time_point retry_at{Clock::time_point::max()};
    /// The running task's counters, written by the transfer as it goes, and when it started.
    OpenPak::Olsc::Transfer::Progress live_progress;
    Clock::time_point live_started{};
    std::string waiting_logged;
    int execution_holds{};
    int autonomy_holds{};
    std::jthread thread;
};

} // namespace Service::OLSC

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>

#include "common/redact.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/olsc/native_handle_holder.h"
#include "core/hle/service/olsc/stopper_object.h"
#include "core/hle/service/olsc/transfer_task_list_controller.h"
#include "core/hle/service/olsc/transfer_runner.h"
#include "core/openpak/async_worker.h"

namespace Service::OLSC {

namespace {

// The 23.0.1 sysmodule's answers when the task asked for does not exist (0x1a96b0, 0x1aa1b0,
// 0x1ab020) and when no task is running (0x1a9850).
constexpr Result ResultTransferTaskNotFound{ErrorModule::OLSC, 1102};
constexpr Result ResultNoCurrentTransferTask{ErrorModule::OLSC, 1103};

} // namespace

ITransferTaskListController::ITransferTaskListController(
    Core::System& system_, std::shared_ptr<OpenPak::AsyncWorker> worker_,
    std::shared_ptr<TransferQueue> queue_)
    : ServiceFramework{system_, "ITransferTaskListController"}, worker{std::move(worker_)},
      queue{std::move(queue_)} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, nullptr, "GetTransferTaskCountForOcean"},
        {1, nullptr, "GetTransferTaskInfoForOcean"},
        {2, nullptr, "ListTransferTaskInfoForOcean"},
        {3, nullptr, "DeleteTransferTaskForOcean"},
        {4, nullptr, "RaiseTransferTaskPriorityForOcean"},
        {5, D<&ITransferTaskListController::GetTransferTaskEndEventNativeHandleHolder>, "GetTransferTaskEndEventNativeHandleHolder"},
        {6, nullptr, "GetTransferTaskProgressForOcean"},
        {7, nullptr, "GetTransferTaskLastResultForOcean"},
        {8, D<&ITransferTaskListController::StopNextTransferTaskExecution>, "StopNextTransferTaskExecution"},
        {9, D<&ITransferTaskListController::GetTransferTaskStartEventNativeHandleHolder>, "GetTransferTaskStartEventNativeHandleHolder"},
        {10, nullptr, "SuspendTransferTaskForOcean"},
        {11, nullptr, "GetCurrentTransferTaskInfoForOcean"},
        {12, nullptr, "FindTransferTaskInfoForOcean"},
        {13, nullptr, "CancelCurrentRepairTransferTask"},
        {14, nullptr, "GetRepairTransferTaskProgress"},
        {15, nullptr, "EnsureExecutableForRepairTransferTask"},
        {16, D<&ITransferTaskListController::GetTransferTaskCount>, "GetTransferTaskCount"},
        {17, D<&ITransferTaskListController::GetTransferTaskInfo>, "GetTransferTaskInfo"},
        {18, D<&ITransferTaskListController::ListTransferTaskInfo>, "ListTransferTaskInfo"},
        {19, D<&ITransferTaskListController::DeleteTransferTask>, "DeleteTransferTask"},
        {20, D<&ITransferTaskListController::RaiseTransferTaskPriority>, "RaiseTransferTaskPriority"},
        {21, D<&ITransferTaskListController::GetTransferTaskProgress>, "GetTransferTaskProgress"}, //10.1.0+
        {22, D<&ITransferTaskListController::GetTransferTaskLastResult>, "GetTransferTaskLastResult"},
        {23, D<&ITransferTaskListController::SuspendTransferTask>, "SuspendTransferTask"},
        {24, D<&ITransferTaskListController::GetCurrentTransferTaskInfo>, "GetCurrentTransferTaskInfo"},
        {25, D<&ITransferTaskListController::FindTransferTaskInfo>, "FindTransferTaskInfo"},
        {26, nullptr, "Unknown26"}, //20.1.0+
        {27, nullptr, "Unknown27"}, //20.1.0+
        {28, nullptr, "Unknown28"}, //20.1.0+
        {29, nullptr, "Unknown29"}, //20.1.0+
        {30, nullptr, "Unknown30"}, //20.1.0+
    };
    // clang-format on

    RegisterHandlers(functions);
}

ITransferTaskListController::~ITransferTaskListController() = default;

Result ITransferTaskListController::GetTransferTaskEndEventNativeHandleHolder(
    Out<SharedPointer<INativeHandleHolder>> out_holder) {
    // Signalled each time a transfer task ends.
    LOG_DEBUG(Service_OLSC, "called");
    *out_holder = std::make_shared<INativeHandleHolder>(
        system, worker, [queue = queue](auto event) { queue->AddEndEvent(std::move(event)); });
    R_SUCCEED();
}

Result ITransferTaskListController::StopNextTransferTaskExecution(
    Out<SharedPointer<IStopperObject>> out_stopper) {
    // 0x1abb44: while the stopper is open no task starts (a running one goes on).
    LOG_DEBUG(Service_OLSC, "called");
    queue->HoldExecution(true);
    *out_stopper = std::make_shared<IStopperObject>(
        system, [queue = queue] { queue->HoldExecution(false); });
    R_SUCCEED();
}

Result ITransferTaskListController::GetTransferTaskStartEventNativeHandleHolder(
    Out<SharedPointer<INativeHandleHolder>> out_holder) {
    // Signalled each time a transfer task starts.
    LOG_DEBUG(Service_OLSC, "called");
    *out_holder = std::make_shared<INativeHandleHolder>(
        system, worker, [queue = queue](auto event) { queue->AddStartEvent(std::move(event)); });
    R_SUCCEED();
}

Result ITransferTaskListController::GetCurrentTransferTaskInfo(Out<TransferTaskInfo> out_info,
                                                              u8 filter) {
    // 0x223010 -> 0x1a9850: the running task's info, or 2179-1103 when none runs.
    *out_info = {};
    const auto current = queue->Current();
    if (!current || !MatchesFilter(*current, filter)) {
        R_RETURN(ResultNoCurrentTransferTask);
    }
    *out_info = PackTaskInfo(*current);
    R_SUCCEED();
}

Result ITransferTaskListController::GetTransferTaskInfo(Out<TransferTaskInfo> out_info, u8 filter,
                                                       u64 task_id) {
    // 0x222b10 -> 0x1a96b0: {u8 filter, u64 task id} in, the info or 2179-1102.
    *out_info = {};
    for (const auto& task : queue->Tasks()) {
        if (task.id == task_id && MatchesFilter(task, filter)) {
            *out_info = PackTaskInfo(task);
            R_SUCCEED();
        }
    }
    R_RETURN(ResultTransferTaskNotFound);
}

Result ITransferTaskListController::ListTransferTaskInfo(
    Out<s32> out_count, OutArray<TransferTaskInfo, BufferAttr_HipcMapAlias> out_infos, u8 filter,
    s32 offset) {
    // 0x222ca0 -> 0x1a9b60: {u8 filter, s32 offset} in, TransferTaskInfo[] and an s32 count out.
    std::vector<TransferTask> matching;
    for (const auto& task : queue->Tasks()) {
        if (MatchesFilter(task, filter)) {
            matching.push_back(task);
        }
    }
    s32 written = 0;
    for (std::size_t i = static_cast<std::size_t>(std::max(offset, 0));
         i < matching.size() && static_cast<std::size_t>(written) < out_infos.size(); ++i) {
        out_infos[written++] = PackTaskInfo(matching[i]);
    }
    *out_count = written;
    R_SUCCEED();
}

Result ITransferTaskListController::DeleteTransferTask(u8 filter, u64 task_id) {
    // 0x1a9f20: {u8 filter, u64 task id}, nothing out, success even for an unknown task.
    for (const auto& task : queue->Tasks()) {
        if (task.id == task_id && MatchesFilter(task, filter)) {
            queue->Delete(task_id);
        }
    }
    R_SUCCEED();
}

Result ITransferTaskListController::RaiseTransferTaskPriority(u8 filter, u64 task_id) {
    // 0x1aa0a0: {u8 filter, u64 task id}, nothing out, success.
    for (const auto& task : queue->Tasks()) {
        if (task.id == task_id && MatchesFilter(task, filter)) {
            queue->Raise(task_id);
        }
    }
    R_SUCCEED();
}

Result ITransferTaskListController::GetTransferTaskLastResult(u8 filter, u64 task_id) {
    // 0x1aa350: the task's last result is the command's result; 0 for an unknown task.
    for (const auto& task : queue->Tasks()) {
        if (task.id == task_id && MatchesFilter(task, filter) && task.last_result != 0) {
            Result result{};
            result.raw = task.last_result;
            R_RETURN(result);
        }
    }
    R_SUCCEED();
}

Result ITransferTaskListController::SuspendTransferTask(u8 filter, u64 task_id) {
    // 0x1a9ff0: success; Harbor's tasks are not suspended (a running one runs to its end).
    LOG_DEBUG(Service_OLSC, "called, filter={} task={:#x}", filter, task_id);
    R_SUCCEED();
}

Result ITransferTaskListController::GetTransferTaskProgress(Out<std::array<u8, 0x18>> out_progress,
                                                           u8 filter, u64 task_id) {
    // 0x222e80 -> 0x1aa1b0: {total bytes, bytes done, throughput in bits/s}, or 2179-1102.
    out_progress->fill(0);
    for (const auto& task : queue->Tasks()) {
        if (task.id == task_id && MatchesFilter(task, filter)) {
            std::memcpy(out_progress->data(), &task.progress.total, 8);
            std::memcpy(out_progress->data() + 8, &task.progress.done, 8);
            R_SUCCEED();
        }
    }
    R_RETURN(ResultTransferTaskNotFound);
}

Result ITransferTaskListController::FindTransferTaskInfo(Out<TransferTaskInfo> out_info,
                                                        std::array<u64, 4> query) {
    // 0x2231c0 -> 0x1ab020 FindTransferTaskInfo(const Uid&, ApplicationId, TransferTaskFilter),
    // raw {u8 filter @0, Uid @8, ApplicationId @0x18} (qlaunch's packer 0x8926bf0): the user's
    // first task of the title whose type the filter takes (predicate 0x1ab5c0: 0 types 0-1,
    // 1 types 2-3, 2 any), running or queued; none is 2179-1102.
    const u8 filter = static_cast<u8>(query[0]);
    Common::UUID user{};
    std::memcpy(user.uuid.data(), &query[1], sizeof(user.uuid));
    const u64 application_id = query[3];
    for (const auto& task : queue->Tasks()) {
        if (task.user == user && task.application_id == application_id &&
            MatchesFilter(task, filter)) {
            *out_info = PackTaskInfo(task);
            R_SUCCEED();
        }
    }
    *out_info = {};
    R_RETURN(ResultTransferTaskNotFound);
}

Result ITransferTaskListController::GetTransferTaskCount(Out<u32> out_count, u8 filter) {
    // 0x222980 -> 0x1a9400: tasks of a kind (filter 0: types 0-1, 1: types 2-3, 2: all).
    u32 count = 0;
    for (const auto& task : queue->Tasks()) {
        count += MatchesFilter(task, filter) ? 1 : 0;
    }
    *out_count = count;
    R_SUCCEED();
}

} // namespace Service::OLSC

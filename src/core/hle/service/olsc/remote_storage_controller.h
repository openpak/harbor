// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <memory>
#include <string>

#include "common/uuid.h"
#include "core/hle/service/cmif_types.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/service.h"
#include "core/openpak/async_worker.h"

namespace Kernel {
class KEvent;
class KReadableEvent;
class KTransferMemory;
} // namespace Kernel

namespace OpenPak {
class AsyncTask;
class AsyncWorker;
} // namespace OpenPak

namespace Service::OLSC {

class TransferQueue;

/// The sysmodule's IAsyncResult (0x21df40): 0 GetResult, 1 Cancel, 2 HasDone, 3 GetSystemEvent,
/// answered by work on an OpenPak::AsyncWorker.
class IAsyncResult final : public ServiceFramework<IAsyncResult> {
public:
    IAsyncResult(Core::System& system_, std::shared_ptr<OpenPak::AsyncWorker> worker_,
                 std::function<Result()> work);
    IAsyncResult(Core::System& system_, std::shared_ptr<OpenPak::AsyncWorker> worker_,
                 OpenPak::AsyncWorker::Work work, std::function<std::array<u8, 0x30>()> progress,
                 Kernel::KTransferMemory* list_memory = nullptr);
    ~IAsyncResult() override;

private:
    Result GetResult();
    Result Cancel();
    Result HasDone(Out<bool> out_done);
    Result GetSystemEvent(OutCopyHandle<Kernel::KReadableEvent> out_event);
    Result GetProgress(Out<std::array<u8, 0x30>> out_progress);
    Result GetListSize(Out<u32> out_size);
    Result GetListData(Out<u32> out_count, s32 offset, OutBuffer<BufferAttr_HipcMapAlias> out_data);

    KernelHelpers::ServiceContext service_context;
    Kernel::KEvent* event{};
    std::shared_ptr<OpenPak::AsyncWorker> worker;
    std::shared_ptr<OpenPak::AsyncTask> task;
    std::function<std::array<u8, 0x30>()> progress;
    Kernel::KTransferMemory* list_memory{};
};

class IRemoteStorageController final : public ServiceFramework<IRemoteStorageController> {
public:
    IRemoteStorageController(Core::System& system_, Common::UUID user_id_,
                             std::shared_ptr<OpenPak::AsyncWorker> worker_,
                             std::shared_ptr<TransferQueue> queue_);
    ~IRemoteStorageController() override;

private:
    Result GetSecondarySave(Out<bool> out_has_secondary_save, Out<std::array<u64, 3>> out_unknown,
                            u64 application_id);
    Result GetDataNewnessByApplicationId(Out<u8> out_newness, u64 application_id);
    Result RegisterDownloadTask(Out<u64> out_task_id, u64 archive_id);
    Result RegisterUploadTaskForApplication(Out<u64> out_task_id, u64 application_id);
    Result RegisterUploadTask(Out<u64> out_task_id, u8 rank, bool flag, u64 application_id);
    Result CreateSaveDataArchiveCacheUpdationTask(Out<SharedPointer<IAsyncResult>> out_task);
    Result CreateSaveDataArchiveCacheUpdationForSpecifiedApplicationTask(
        Out<SharedPointer<IAsyncResult>> out_task, u64 application_id);
    Result CreateRegisterNotificationTokenTask(Out<SharedPointer<IAsyncResult>> out_task);
    Result ListDataInfo(Out<s32> out_count,
                        OutArray<std::array<u8, 0x38>, BufferAttr_HipcMapAlias> out_infos,
                        s32 count, s32 offset);
    Result ListDataInfoV2(Out<s32> out_count,
                          OutArray<std::array<u8, 0x38>, BufferAttr_HipcMapAlias> out_infos,
                          s32 count, s32 offset);
    Result GetDataInfoV1(Out<std::array<u8, 0x30>> out_data, u64 application_id);
    Result GetDataInfo(Out<std::array<u8, 0x38>> out_data, u64 application_id);

    /// The user this controller speaks for, as the 32 hex digits the OpenPak layer names it by.
    std::string uid;
    std::shared_ptr<OpenPak::AsyncWorker> worker;
    std::shared_ptr<TransferQueue> queue;
    Common::UUID user;
};

} // namespace Service::OLSC

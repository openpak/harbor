// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>

#include "core/hle/service/cmif_serialization.h"
#include "common/redact.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_readable_event.h"
#include "core/hle/service/olsc/remote_storage_controller.h"
#include "core/openpak/async_worker.h"
#include "core/openpak/olsc.h"

namespace Service::OLSC {

namespace {

// 2179-1200: the sysmodule's answer when its archive cache has nothing for the title
// (GetDataInfo 0x1ff010; GetDataInfoV1 0x1fefe0 always answers it on 23.x).
constexpr Result ResultDataInfoNotFound{ErrorModule::OLSC, 1200};

// What GetResult answers after Cancel. The sysmodule's own value was not read; this is the
// generic "cancelled" of the olsc module as the guest sees it (logged when it happens).
constexpr Result ResultCancelled{ErrorModule::OLSC, 1};

} // namespace

IAsyncResult::IAsyncResult(Core::System& system_, std::shared_ptr<OpenPak::AsyncWorker> worker_,
                           std::function<Result()> work)
    : ServiceFramework{system_, "olsc:IAsyncResult"}, service_context{system_, "OLSC"},
      worker{std::move(worker_)} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, D<&IAsyncResult::GetResult>, "GetResult"},
        {1, D<&IAsyncResult::Cancel>, "Cancel"},
        {2, D<&IAsyncResult::HasDone>, "HasDone"},
        {3, D<&IAsyncResult::GetSystemEvent>, "GetSystemEvent"},
    };
    // clang-format on
    RegisterHandlers(functions);

    event = service_context.CreateEvent("OLSC::IAsyncResult");
    task = worker->Run(event, ResultCancelled,
                       [work = std::move(work)](std::stop_token, std::vector<u8>&) {
                           return work();
                       });
}

IAsyncResult::~IAsyncResult() {
    task->Detach();
    service_context.CloseEvent(event);
}

Result IAsyncResult::GetResult() {
    if (!task->IsDone()) {
        // Not established what the sysmodule answers before completion; qlaunch waits first.
        LOG_UNKNOWN_CONTRACT(Service_OLSC,
                             "olsc IAsyncResult 0 asked before done, answered 2179-1");
        R_RETURN(ResultCancelled);
    }
    R_RETURN(task->GetResult());
}

Result IAsyncResult::Cancel() {
    LOG_DEBUG(Service_OLSC, "called");
    task->Cancel();
    R_SUCCEED();
}

Result IAsyncResult::HasDone(Out<bool> out_done) {
    *out_done = task->IsDone();
    R_SUCCEED();
}

Result IAsyncResult::GetSystemEvent(OutCopyHandle<Kernel::KReadableEvent> out_event) {
    *out_event = std::addressof(event->GetReadableEvent());
    R_SUCCEED();
}

IRemoteStorageController::IRemoteStorageController(Core::System& system_, Common::UUID user_id_,
                                                   std::shared_ptr<OpenPak::AsyncWorker> worker_)
    : ServiceFramework{system_, "IRemoteStorageController"},
      uid{user_id_.IsInvalid() ? std::string{} : user_id_.RawString()},
      worker{std::move(worker_)} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, nullptr, "GetSaveDataArchiveInfoBySaveDataId"},
        {1, nullptr, "GetSaveDataArchiveInfoByApplicationId"},
        {3, nullptr, "GetSaveDataArchiveCount"},
        {6, nullptr, "CleanupSaveDataArchives"},
        {7, D<&IRemoteStorageController::CreateSaveDataArchiveCacheUpdationTask>, "CreateSaveDataArchiveCacheUpdationTask"},
        {8, D<&IRemoteStorageController::CreateSaveDataArchiveCacheUpdationForSpecifiedApplicationTask>, "CreateSaveDataArchiveCacheUpdationForSpecifiedApplicationTask"},
        {9, nullptr, "Delete"},
        {10, nullptr, "GetSeriesInfo"},
        {11, nullptr, "CreateDeleteDataTask"},
        {12, nullptr, "DeleteSeriesInfo"},
        {13, D<&IRemoteStorageController::CreateRegisterNotificationTokenTask>, "CreateRegisterNotificationTokenTask"},
        {14, D<&IRemoteStorageController::GetDataNewnessByApplicationId>, "GetDataNewnessByApplicationId"},
        {15, nullptr, "RegisterUploadSaveDataTransferTaskForAutonomyRegistration"},
        {16, nullptr, "CreateCleanupToDeleteSaveDataArchiveInfoTask"},
        {17, D<&IRemoteStorageController::ListDataInfo>, "ListDataInfo"},
        {18, D<&IRemoteStorageController::GetDataInfoV1>, "GetDataInfoV1"},
        {19, nullptr, "GetDataInfoCacheUpdateNativeHandleHolder"},
        {20, nullptr, "CreateSaveDataArchiveInfoCacheForSaveDataBackupUpdationTask"},
        {21, nullptr, "ListSecondarySaves"},
        {22, D<&IRemoteStorageController::GetSecondarySave>, "GetSecondarySave"},
        {23, nullptr, "TouchSecondarySave"},
        {24, nullptr, "GetSecondarySaveDataInfo"},
        {25, nullptr, "RegisterDownloadSaveDataTransferTaskForAutonomyRegistration"},
        {26, D<&IRemoteStorageController::ListDataInfoV2>, "ListDataInfoV2"}, //20.0.0+
        {27, D<&IRemoteStorageController::GetDataInfo>, "GetDataInfoV2"}, //20.0.0+
        {28, nullptr, "Unknown28"}, //20.0.0+
        {29, nullptr, "Unknown29"}, //21.0.0+
        {800, nullptr, "Unknown800"}, //20.0.0+
        {900, nullptr, "SetLoadedDataMissing"},
        {901, nullptr, "Unknown901"}, //20.2.0+
    };
    // clang-format on

    RegisterHandlers(functions);

    // What the Save Data Cloud pages read comes from the user's archive list on the storage host;
    // it is fetched in the background, never on an IPC thread. No server: nothing is cached and
    // every title reads as having no cloud data.
    OpenPak::Olsc::ArchiveCache::Instance().Refresh(uid);
}

IRemoteStorageController::~IRemoteStorageController() = default;

Result IRemoteStorageController::GetSecondarySave(Out<bool> out_has_secondary_save,
                                                  Out<std::array<u64, 3>> out_unknown,
                                                  u64 application_id) {
    LOG_ERROR(Service_OLSC, "(STUBBED) called, application_id={:016X}", application_id);
    *out_has_secondary_save = false;
    *out_unknown = {};
    R_SUCCEED();
}

Result IRemoteStorageController::GetDataNewnessByApplicationId(Out<u8> out_newness,
                                                              u64 application_id) {
    // The sysmodule (0x1d32e0) answers 0 when the cloud and the local save agree and 1 when they
    // differ. Harbor cannot compare digests without the transfer exporter, so an archive in the
    // cloud reads as different.
    const auto archives = OpenPak::Olsc::ArchiveCache::Instance().Get(uid);
    const bool in_cloud =
        archives && OpenPak::Olsc::LatestFor(*archives, application_id).has_value();
    LOG_DEBUG(Service_OLSC, "called, app={:016X} in_cloud={}", application_id, in_cloud);
    *out_newness = in_cloud ? 1 : 0;
    R_SUCCEED();
}

Result IRemoteStorageController::CreateSaveDataArchiveCacheUpdationTask(
    Out<SharedPointer<IAsyncResult>> out_task) {
    // 0x224760: no input, an IAsyncResult out. "Update List" on the Save Data Cloud page.
    LOG_INFO(Service_OLSC, "called");
    *out_task = std::make_shared<IAsyncResult>(system, worker, [who = uid]() -> Result {
        if (!OpenPak::Olsc::ArchiveCache::Instance().RefreshNow(who)) {
            // A console without its server fails here with a network error; which one is not
            // established, so the list stays as it was and the task succeeds.
            LOG_UNKNOWN_CONTRACT(Service_OLSC,
                                 "olsc cache update: no list fetched, answered success");
        }
        return ResultSuccess;
    });
    R_SUCCEED();
}

Result IRemoteStorageController::CreateSaveDataArchiveCacheUpdationForSpecifiedApplicationTask(
    Out<SharedPointer<IAsyncResult>> out_task, u64 application_id) {
    // 0x224900: the application id in, the same IAsyncResult out. The whole list is fetched (the
    // storage host's list takes an application_id filter, but the cache is per user).
    LOG_INFO(Service_OLSC, "called, app={:016X}", application_id);
    return CreateSaveDataArchiveCacheUpdationTask(out_task);
}

Result IRemoteStorageController::CreateRegisterNotificationTokenTask(
    Out<SharedPointer<IAsyncResult>> out_task) {
    // 0x224f90: no input, an IAsyncResult out (the shape of 7). A console mints the profile's
    // npns token and POSTs it to storage.hac
    // .../network_service_accounts/<nsa>/notification_tokens.
    // Harbor's npns mints no tokens (its Penne registration is bound by the push connection
    // itself), so there is nothing to upload and the task succeeds without a request.
    LOG_UNKNOWN_CONTRACT(Service_OLSC,
                         "olsc RSC 13 notification token: no npns token, answered success");
    *out_task =
        std::make_shared<IAsyncResult>(system, worker, []() -> Result { return ResultSuccess; });
    R_SUCCEED();
}

Result IRemoteStorageController::ListDataInfo(
    Out<s32> out_count, OutArray<std::array<u8, 0x38>, BufferAttr_HipcMapAlias> out_infos,
    s32 count, s32 offset) {
    // 0x2255c0 -> 0x1fedb0: the same shape as 26, and the 23.x sysmodule always answers count 0
    // (the list moved to 26).
    LOG_DEBUG(Service_OLSC, "called, count={} offset={} capacity={}", count, offset,
              out_infos.size());
    *out_count = 0;
    R_SUCCEED();
}

Result IRemoteStorageController::ListDataInfoV2(
    Out<s32> out_count, OutArray<std::array<u8, 0x38>, BufferAttr_HipcMapAlias> out_infos,
    s32 count, s32 offset) {
    // The 23.0.1 sysmodule (0x226140 -> 0x1fedf0 -> ISaveDataArchiveInfoCache::ListDataInfo
    // (DataInfo*, Uid, int, int), walk at 0x1d6be0): two s32 in, an s32 count and DataInfo[] out.
    // The first is read as the count and the second as the offset into the cache; qlaunch's
    // Save Data Cloud page asks with a 2000-entry buffer.
    *out_count = 0;
    const auto archives = OpenPak::Olsc::ArchiveCache::Instance().Get(uid);
    if (!archives || offset < 0 || count <= 0) {
        LOG_DEBUG(Service_OLSC, "called, count={} offset={}: nothing listed", count, offset);
        R_SUCCEED();
    }
    const auto listed = OpenPak::Olsc::LatestPerApplication(*archives);
    const std::size_t start = static_cast<std::size_t>(offset);
    const std::size_t wanted =
        std::min<std::size_t>(static_cast<std::size_t>(count), out_infos.size());
    std::size_t written = 0;
    for (std::size_t i = start; i < listed.size() && written < wanted; ++i, ++written) {
        out_infos[written] = OpenPak::Olsc::PackDataInfo(listed[i]);
    }
    LOG_INFO(Service_OLSC, "called, count={} offset={}: {} title(s) in the cloud, {} listed", count,
             offset, listed.size(), written);
    *out_count = static_cast<s32>(written);
    R_SUCCEED();
}

Result IRemoteStorageController::GetDataInfoV1(Out<std::array<u8, 0x30>> out_data,
                                               u64 application_id) {
    LOG_DEBUG(Service_OLSC, "called, app={:016X}", application_id);
    out_data->fill(0);
    R_RETURN(ResultDataInfoNotFound);
}

Result IRemoteStorageController::GetDataInfo(Out<std::array<u8, 0x38>> out_data,
                                             u64 application_id) {
    out_data->fill(0);
    OpenPak::Olsc::ArchiveCache::Instance().Refresh(uid);
    const auto archives = OpenPak::Olsc::ArchiveCache::Instance().Get(uid);
    const auto latest =
        archives ? OpenPak::Olsc::LatestFor(*archives, application_id) : std::nullopt;
    if (!latest) {
        LOG_DEBUG(Service_OLSC, "called, app={:016X}: no cloud data", application_id);
        R_RETURN(ResultDataInfoNotFound);
    }
    LOG_INFO(Service_OLSC, "called, app={:016X}: archive of {} bytes", application_id,
             latest->data_size);
    *out_data = OpenPak::Olsc::PackDataInfo(*latest);
    R_SUCCEED();
}

} // namespace Service::OLSC

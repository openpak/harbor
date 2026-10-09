// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/fs/path_util.h"
#include "common/redact.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_readable_event.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/olsc/daemon_controller.h"
#include "core/hle/service/olsc/olsc_service_for_system_service.h"
#include "core/hle/service/olsc/remote_storage_controller.h"
#include "core/hle/service/olsc/transfer_runner.h"
#include "core/hle/service/olsc/transfer_task_list_controller.h"
#include "core/openpak/async_worker.h"
#include "core/openpak/olsc.h"

namespace Service::OLSC {

// olsc:s 103/105 with nothing recorded (0x7d0b3).
constexpr Result ResultNoErrorInfo{ErrorModule::OLSC, 1000};

IOlscServiceForSystemService::IOlscServiceForSystemService(Core::System& system_)
    : ServiceFramework{system_, "olsc:s"} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, D<&IOlscServiceForSystemService::GetTransferTaskListController>, "GetTransferTaskListController"},
        {1, D<&IOlscServiceForSystemService::GetRemoteStorageController>, "GetRemoteStorageController"},
        {2, D<&IOlscServiceForSystemService::GetDaemonController>, "GetDaemonController"},
        {10, nullptr, "PrepareDeleteUserProperty"},
        {11, nullptr, "DeleteUserSaveDataProperty"},
        {12, nullptr, "InvalidateMountCache"},
        {13, nullptr, "DeleteDeviceSaveDataProperty"},
        {100, nullptr, "ListLastTransferTaskErrorInfo"},
        {101, nullptr, "GetLastErrorInfoCount"},
        {102, D<&IOlscServiceForSystemService::RemoveLastErrorInfo>, "RemoveLastErrorInfoOld"},
        {103, D<&IOlscServiceForSystemService::GetTransferTaskErrorInfo>, "GetLastErrorInfo"},
        {104, nullptr, "GetLastErrorEventHolder"},
        {105, D<&IOlscServiceForSystemService::GetTransferTaskErrorInfo>, "GetTransferTaskErrorInfo"},
        {200, D<&IOlscServiceForSystemService::GetDataTransferPolicy>, "GetDataTransferPolicy"},
        {201, D<&IOlscServiceForSystemService::DeleteDataTransferPolicyCache>, "DeleteDataTransferPolicyCache"},
        {202, nullptr, "Unknown202"},
        {203, nullptr, "RequestUpdateDataTransferPolicyCacheAsync"},
        {204, D<&IOlscServiceForSystemService::ClearDataTransferPolicyCache>, "ClearDataTransferPolicyCache"},
        {205, nullptr, "RequestGetDataTransferPolicyAsync"},
        {206, nullptr, "Unknown206"}, //21.0.0+
        {209, D<&IOlscServiceForSystemService::GetDataTransferPolicy209>, "Unknown209"}, //22.0.0+
        {300, nullptr, "GetUserSaveDataProperty"},
        {301, nullptr, "SetUserSaveDataProperty"},
        {302, nullptr, "Unknown302"}, //21.0.0+
        {400, nullptr, "CleanupSaveDataBackupContextForSpecificApplications"},
        {900, nullptr, "DeleteAllTransferTask"},
        {902, nullptr, "DeleteAllSeriesInfo"},
        {903, nullptr, "DeleteAllSdaInfoCache"},
        {904, nullptr, "DeleteAllApplicationSetting"},
        {905, nullptr, "DeleteAllTransferTaskErrorInfo"},
        {906, nullptr, "RegisterTransferTaskErrorInfo"},
        {907, nullptr, "AddSaveDataArchiveInfoCache"},
        {908, nullptr, "DeleteSeriesInfo"},
        {909, nullptr, "GetSeriesInfo"},
        {910, nullptr, "RemoveTransferTaskErrorInfo"},
        {911, nullptr, "DeleteAllSeriesInfoForSaveDataBackup"},
        {912, nullptr, "DeleteSeriesInfoForSaveDataBackup"},
        {913, nullptr, "GetSeriesInfoForSaveDataBackup"},
        {914, nullptr, "Unknown914"}, //20.2.0+
        {1000, nullptr, "UpdateIssueOld"},
        {1001, D<&IOlscServiceForSystemService::RequestLatestVersionPlayablePlatforms>, "RequestLatestVersionPlayablePlatforms"}, //23.0.0+
        {1010, nullptr, "Unknown1010"},
        {1011, nullptr, "Unknown1011"},
        {1012, nullptr, "Unknown1012"},
        {1013, nullptr, "Unkown1013"},
        {1014, nullptr, "Unknown1014"},
        {1020, nullptr, "Unknown1020"},
        {1021, nullptr, "Unknown1021"},
        {1022, nullptr, "Unknown1022"},
        {1023, nullptr, "Unknown1023"},
        {1024, nullptr, "Unknown1024"},
        {1100, nullptr, "RepairUpdateIssueInfoCacheAync"},
        {1110, nullptr, "RepairGetIssueInfo"},
        {1111, nullptr, "RepairListIssueInfo"},
        {1112, nullptr, "RepairListOperationPermissionInfo"},
        {1113, nullptr, "RepairListDataInfoForRepairedSaveDataDownload"},
        {1114, nullptr, "RepairListDataInfoForOriginalSaveDataDownload"},
        {1120, nullptr, "RepairUploadSaveDataAsync"},
        {1121, nullptr, "RepairUploadSaveDataAsync1"},
        {1122, nullptr, "RepairDownloadRepairedSaveDataAsync"},
        {1123, nullptr, "RepairDownloadOriginalSaveDataAsync"},
        {1124, nullptr, "RepairGetOperationProgressInfo"},
        {10000, D<&IOlscServiceForSystemService::GetOlscServiceForSystemService>, "GetOlscServiceForSystemService"},
    };
    // clang-format on

    RegisterHandlers(functions);

    worker = std::make_shared<OpenPak::AsyncWorker>(system, "olsc:OpenPak");
    queue = TransferQueue::For(system);
}

IOlscServiceForSystemService::~IOlscServiceForSystemService() = default;

Result IOlscServiceForSystemService::GetTransferTaskListController(
    Out<SharedPointer<ITransferTaskListController>> out_interface) {
    LOG_INFO(Service_OLSC, "called");
    *out_interface = std::make_shared<ITransferTaskListController>(system, worker, queue);
    R_SUCCEED();
}

Result IOlscServiceForSystemService::GetRemoteStorageController(
    Out<SharedPointer<IRemoteStorageController>> out_interface, Common::UUID user_id) {
    LOG_INFO(Service_OLSC, "called");
    // The sysmodule keeps its archive cache in its system save; Harbor keeps it beside the NAND
    // saves, so the Save Data Cloud page lists the last known archives before a fetch lands.
    OpenPak::Olsc::ArchiveCache::Instance().SetStore(
        Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir) / "system" / "harbor" /
        "olsc-archives.json");
    // The controller speaks for one user (the sysmodule's takes the Uid here, 0x10 bytes in).
    *out_interface = std::make_shared<IRemoteStorageController>(system, user_id, worker, queue);
    R_SUCCEED();
}

Result IOlscServiceForSystemService::GetDaemonController(
    Out<SharedPointer<IDaemonController>> out_interface) {
    LOG_INFO(Service_OLSC, "called");
    *out_interface = std::make_shared<IDaemonController>(system, queue);
    R_SUCCEED();
}

Result IOlscServiceForSystemService::GetDataTransferPolicy(
    Out<DataTransferPolicy> out_policy, u64 application_id) {
    LOG_WARNING(Service_OLSC, "(STUBBED) called");
    DataTransferPolicy policy{};
    policy.upload_policy = 0;
    policy.download_policy = 0;
    *out_policy = policy;
    R_SUCCEED();
}

Result IOlscServiceForSystemService::DeleteDataTransferPolicyCache(u64 application_id) {
    // 0x21d1e0 case 201: u64 in, nothing out. Harbor keeps no policy cache (200 answers zeros).
    LOG_DEBUG(Service_OLSC, "called, app={:016X}", application_id);
    R_SUCCEED();
}

Result IOlscServiceForSystemService::ClearDataTransferPolicyCache() {
    // 0x21d1e0 case 204: nothing in or out. "Update List" sends it first.
    LOG_DEBUG(Service_OLSC, "called");
    R_SUCCEED();
}

Result IOlscServiceForSystemService::GetDataTransferPolicy209(Out<DataTransferPolicy> out_policy,
                                                              u64 application_id) {
    // Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base): the tile's Options menu
    // calls it right after 200. The 23.0.1 sysmodule unpacks it as 200 (u64 in, 2 bytes out,
    // 0x21fd30 against 0x21f210); what the bytes mean is not established.
    LOG_UNKNOWN_CONTRACT(Service_OLSC, "olsc:s 209 answered like 200 (two zero bytes), app={:016X}",
                         application_id);
    *out_policy = DataTransferPolicy{};
    R_SUCCEED();
}

Result IOlscServiceForSystemService::RequestLatestVersionPlayablePlatforms(
    Out<SharedPointer<IAsyncLatestVersionPlayablePlatformsRequest>> out_request,
    u64 application_id) {
    LOG_DEBUG(Service_OLSC, "called, app={:016X}", application_id);
    *out_request = std::make_shared<IAsyncLatestVersionPlayablePlatformsRequest>(system);
    R_SUCCEED();
}

Result IOlscServiceForSystemService::GetTransferTaskErrorInfo(Out<TransferTaskErrorInfo> out_info,
                                                             Common::UUID uuid, u64 application_id) {
    // 0x18a670 (103 GetLastErrorInfo 0x18a580 is the same lookup): the title's last failed
    // transfer, or 2179-1000 when none is recorded, which qlaunch reads as "no error".
    LOG_DEBUG(Service_OLSC, "called, application_id={:016X}", application_id);
    const auto error = queue->Settings()->LastErrorOf(uuid.RawString(), application_id);
    if (!error) {
        R_THROW(ResultNoErrorInfo);
    }
    TransferTaskErrorInfo info{};
    info.uid = uuid;
    info.application_id = application_id;
    info.unknown_0x18 = error->task_type;
    info.unknown_0x20 = static_cast<u64>(error->time);
    info.error_code = error->result & 0x3fffff;
    *out_info = info;
    R_SUCCEED();
}

Result IOlscServiceForSystemService::RemoveLastErrorInfo(Common::UUID uuid, u64 application_id) {
    // 102 (0x18a760): success whether or not there was one.
    queue->Settings()->RemoveLastError(uuid.RawString(), application_id);
    R_SUCCEED();
}

Result IOlscServiceForSystemService::GetOlscServiceForSystemService(
    Out<SharedPointer<IOlscServiceForSystemService>> out_interface) {
    LOG_INFO(Service_OLSC, "called");
    *out_interface = std::static_pointer_cast<IOlscServiceForSystemService>(shared_from_this());
    R_SUCCEED();
}

// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base). Commands 0-3 are the
// sysmodule's async base (0x21df40: 0 and 1 no data, 2 one byte, 3 a copy handle).
IAsyncLatestVersionPlayablePlatformsRequest::IAsyncLatestVersionPlayablePlatformsRequest(
    Core::System& system_)
    : ServiceFramework{system_, "IAsyncLatestVersionPlayablePlatformsRequest"},
      service_context{system_, "OLSC"} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, D<&IAsyncLatestVersionPlayablePlatformsRequest::GetResult>, "GetResult"},
        {1, D<&IAsyncLatestVersionPlayablePlatformsRequest::Cancel>, "Cancel"},
        {2, D<&IAsyncLatestVersionPlayablePlatformsRequest::HasDone>, "HasDone"},
        {3, D<&IAsyncLatestVersionPlayablePlatformsRequest::GetSystemEvent>, "GetSystemEvent"},
        {100, D<&IAsyncLatestVersionPlayablePlatformsRequest::GetPlayablePlatforms>, "GetPlayablePlatforms"},
    };
    // clang-format on
    RegisterHandlers(functions);

    // Done when made: the event is signalled before the guest first waits on it.
    event = service_context.CreateEvent("OLSC::LatestVersionPlayablePlatforms");
    event->Signal(system.Kernel());
}

IAsyncLatestVersionPlayablePlatformsRequest::~IAsyncLatestVersionPlayablePlatformsRequest() {
    service_context.CloseEvent(event);
}

Result IAsyncLatestVersionPlayablePlatformsRequest::GetResult() {
    R_SUCCEED();
}

Result IAsyncLatestVersionPlayablePlatformsRequest::Cancel() {
    R_SUCCEED();
}

Result IAsyncLatestVersionPlayablePlatformsRequest::HasDone(Out<bool> out_done) {
    *out_done = true;
    R_SUCCEED();
}

Result IAsyncLatestVersionPlayablePlatformsRequest::GetSystemEvent(
    OutCopyHandle<Kernel::KReadableEvent> out_event) {
    *out_event = std::addressof(event->GetReadableEvent());
    R_SUCCEED();
}

Result IAsyncLatestVersionPlayablePlatformsRequest::GetPlayablePlatforms(Out<u32> out_platforms) {
    // Four bytes out (0x2289f0). What pegasus /v1/apps/<id>/latest/platforms -> ["NX"] would
    // give, as the Ryujinx Harbor answers; the bit meaning is not established.
    LOG_UNKNOWN_CONTRACT(Service_OLSC, "olsc 1001 request 100 answered u32 1 (NX only)");
    *out_platforms = 1;
    R_SUCCEED();
}

} // namespace Service::OLSC

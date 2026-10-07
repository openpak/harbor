// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/olsc/remote_storage_controller.h"
#include "core/openpak/olsc.h"

namespace Service::OLSC {

namespace {

// 2179-1200: the sysmodule's answer when its archive cache has nothing for the title
// (GetDataInfo 0x1ff010; GetDataInfoV1 0x1fefe0 always answers it on 23.x).
constexpr Result ResultDataInfoNotFound{ErrorModule::OLSC, 1200};

} // namespace

IRemoteStorageController::IRemoteStorageController(Core::System& system_, Common::UUID user_id_)
    : ServiceFramework{system_, "IRemoteStorageController"},
      uid{user_id_.IsInvalid() ? std::string{} : user_id_.RawString()} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, nullptr, "GetSaveDataArchiveInfoBySaveDataId"},
        {1, nullptr, "GetSaveDataArchiveInfoByApplicationId"},
        {3, nullptr, "GetSaveDataArchiveCount"},
        {6, nullptr, "CleanupSaveDataArchives"},
        {7, nullptr, "CreateSaveDataArchiveCacheUpdationTask"},
        {8, nullptr, "CreateSaveDataArchiveCacheUpdationForSpecifiedApplicationTask"},
        {9, nullptr, "Delete"},
        {10, nullptr, "GetSeriesInfo"},
        {11, nullptr, "CreateDeleteDataTask"},
        {12, nullptr, "DeleteSeriesInfo"},
        {13, nullptr, "CreateRegisterNotificationTokenTask"},
        {14, D<&IRemoteStorageController::GetDataNewnessByApplicationId>, "GetDataNewnessByApplicationId"},
        {15, nullptr, "RegisterUploadSaveDataTransferTaskForAutonomyRegistration"},
        {16, nullptr, "CreateCleanupToDeleteSaveDataArchiveInfoTask"},
        {17, nullptr, "ListDataInfo"},
        {18, D<&IRemoteStorageController::GetDataInfoV1>, "GetDataInfoV1"},
        {19, nullptr, "GetDataInfoCacheUpdateNativeHandleHolder"},
        {20, nullptr, "CreateSaveDataArchiveInfoCacheForSaveDataBackupUpdationTask"},
        {21, nullptr, "ListSecondarySaves"},
        {22, D<&IRemoteStorageController::GetSecondarySave>, "GetSecondarySave"},
        {23, nullptr, "TouchSecondarySave"},
        {24, nullptr, "GetSecondarySaveDataInfo"},
        {25, nullptr, "RegisterDownloadSaveDataTransferTaskForAutonomyRegistration"},
        {26, nullptr, "Unknown26"}, //20.0.0+
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

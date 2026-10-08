// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "common/assert.h"
#include "common/common_types.h"
#include "common/hex_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "core/core.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/errors.h"
#include "core/file_sys/fs_directory.h"
#include "core/file_sys/fs_filesystem.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/program_metadata.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/romfs_factory.h"
#include "core/file_sys/savedata_factory.h"
#include "core/file_sys/system_archive/system_archive.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/hle/result.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/hle/service/filesystem/fsp/fs_i_filesystem.h"
#include "core/hle/service/filesystem/fsp/fs_i_multi_commit_manager.h"
#include "core/hle/service/filesystem/fsp/fs_i_save_data_info_reader.h"
#include "core/hle/service/filesystem/fsp/fs_i_storage.h"
#include "core/hle/service/filesystem/fsp/fsp_srv.h"
#include "core/hle/service/filesystem/fsp/save_data_transfer_prohibiter.h"
#include "core/hle/service/filesystem/romfs_controller.h"
#include "core/hle/service/filesystem/save_data_controller.h"
#include "core/hle/service/hle_ipc.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/loader/loader.h"
#include "core/reporter.h"

namespace Service::FileSystem {

namespace {

// fs access control bits that manage other programs' saves (SaveDataManagement,
// SystemSaveDataManagement, FullPermission).
constexpr u64 SaveDataManagementPermissions = (1ULL << 6) | (1ULL << 14) | (1ULL << 63);

} // namespace

FSP_SRV::FSP_SRV(Core::System& system_)
    : ServiceFramework{system_, "fsp-srv"}, fsc{system.GetFileSystemController()},
      content_provider{system.GetContentProvider()}, reporter{system.GetReporter()} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, nullptr, "OpenFileSystem"},
        {1, D<&FSP_SRV::SetCurrentProcess>, "SetCurrentProcess"},
        {2, D<&FSP_SRV::OpenDataFileSystemByCurrentProcess>, "OpenDataFileSystemByCurrentProcess"},
        {7, D<&FSP_SRV::OpenFileSystemWithPatch>, "OpenFileSystemWithPatch"},
        {8, nullptr, "OpenFileSystemWithId"},
        {9, nullptr, "OpenDataFileSystemByApplicationId"},
        {11, nullptr, "OpenBisFileSystem"},
        {12, nullptr, "OpenBisStorage"},
        {13, nullptr, "InvalidateBisCache"},
        {17, nullptr, "OpenHostFileSystem"},
        {18, D<&FSP_SRV::OpenSdCardFileSystem>, "OpenSdCardFileSystem"},
        {19, nullptr, "FormatSdCardFileSystem"},
        {21, D<&FSP_SRV::DeleteSaveDataFileSystem>, "DeleteSaveDataFileSystem"},
        {22, D<&FSP_SRV::CreateSaveDataFileSystem>, "CreateSaveDataFileSystem"},
        {23, D<&FSP_SRV::CreateSaveDataFileSystemBySystemSaveDataId>, "CreateSaveDataFileSystemBySystemSaveDataId"},
        {24, nullptr, "RegisterSaveDataFileSystemAtomicDeletion"},
        {25, nullptr, "DeleteSaveDataFileSystemBySaveDataSpaceId"},
        {26, nullptr, "FormatSdCardDryRun"},
        {27, D<&FSP_SRV::IsExFatSupported>, "IsExFatSupported"},
        {28, nullptr, "DeleteSaveDataFileSystemBySaveDataAttribute"},
        {30, nullptr, "OpenGameCardStorage"},
        {31, nullptr, "OpenGameCardFileSystem"},
        {32, D<&FSP_SRV::ExtendSaveDataFileSystem>, "ExtendSaveDataFileSystem"},
        {33, nullptr, "DeleteCacheStorage"},
        {34, D<&FSP_SRV::GetCacheStorageSize>, "GetCacheStorageSize"},
        {35, nullptr, "CreateSaveDataFileSystemByHashSalt"},
        {36, nullptr, "OpenHostFileSystemWithOption"},
        {37, &FSP_SRV::CreateSaveDataFileSystemWithCreationInfo2, "CreateSaveDataFileSystemWithCreationInfo2"},
        {51, D<&FSP_SRV::OpenSaveDataFileSystem>, "OpenSaveDataFileSystem"},
        {52, D<&FSP_SRV::OpenSaveDataFileSystemBySystemSaveDataId>, "OpenSaveDataFileSystemBySystemSaveDataId"},
        {53, D<&FSP_SRV::OpenReadOnlySaveDataFileSystem>, "OpenReadOnlySaveDataFileSystem"},
        {57, D<&FSP_SRV::ReadSaveDataFileSystemExtraDataBySaveDataSpaceId>, "ReadSaveDataFileSystemExtraDataBySaveDataSpaceId"},
        {58, D<&FSP_SRV::ReadSaveDataFileSystemExtraData>, "ReadSaveDataFileSystemExtraData"},
        {59, D<&FSP_SRV::WriteSaveDataFileSystemExtraData>, "WriteSaveDataFileSystemExtraData"},
        {60, nullptr, "OpenSaveDataInfoReader"},
        {61, D<&FSP_SRV::OpenSaveDataInfoReaderBySaveDataSpaceId>, "OpenSaveDataInfoReaderBySaveDataSpaceId"},
        {62, D<&FSP_SRV::OpenSaveDataInfoReaderOnlyCacheStorage>, "OpenSaveDataInfoReaderOnlyCacheStorage"},
        {64, nullptr, "OpenSaveDataInternalStorageFileSystem"},
        {65, nullptr, "UpdateSaveDataMacForDebug"},
        {66, nullptr, "WriteSaveDataFileSystemExtraData2"},
        {67, D<&FSP_SRV::FindSaveDataWithFilter>, "FindSaveDataWithFilter"},
        {68, nullptr, "OpenSaveDataInfoReaderBySaveDataFilter"},
        {69, D<&FSP_SRV::ReadSaveDataFileSystemExtraDataBySaveDataAttribute>, "ReadSaveDataFileSystemExtraDataBySaveDataAttribute"},
        {70, D<&FSP_SRV::WriteSaveDataFileSystemExtraDataWithMaskBySaveDataAttribute>, "WriteSaveDataFileSystemExtraDataWithMaskBySaveDataAttribute"},
        {71, D<&FSP_SRV::ReadSaveDataFileSystemExtraDataWithMaskBySaveDataAttribute>, "ReadSaveDataFileSystemExtraDataWithMaskBySaveDataAttribute"},
        {80, nullptr, "OpenSaveDataMetaFile"},
        {81, nullptr, "OpenSaveDataTransferManager"},
        {82, nullptr, "OpenSaveDataTransferManagerVersion2"},
        {83, D<&FSP_SRV::OpenSaveDataTransferProhibiter>, "OpenSaveDataTransferProhibiter"},
        {84, nullptr, "ListApplicationAccessibleSaveDataOwnerId"},
        {85, nullptr, "OpenSaveDataTransferManagerForSaveDataRepair"},
        {86, nullptr, "OpenSaveDataMover"},
        {87, nullptr, "OpenSaveDataTransferManagerForRepair"},
        {100, nullptr, "OpenImageDirectoryFileSystem"},
        {101, nullptr, "OpenBaseFileSystem"},
        {102, nullptr, "FormatBaseFileSystem"},
        {110, nullptr, "OpenContentStorageFileSystem"},
        {120, nullptr, "OpenCloudBackupWorkStorageFileSystem"},
        {130, nullptr, "OpenCustomStorageFileSystem"},
        {200, D<&FSP_SRV::OpenDataStorageByCurrentProcess>, "OpenDataStorageByCurrentProcess"},
        {201, nullptr, "OpenDataStorageByProgramId"},
        {202, D<&FSP_SRV::OpenDataStorageByDataId>, "OpenDataStorageByDataId"},
        {203, D<&FSP_SRV::OpenPatchDataStorageByCurrentProcess>, "OpenPatchDataStorageByCurrentProcess"},
        {204, nullptr, "OpenDataFileSystemByProgramIndex"},
        {205, D<&FSP_SRV::OpenDataStorageWithProgramIndex>, "OpenDataStorageWithProgramIndex"},
        {206, nullptr, "OpenDataStorageByPath"},
        // 23.0.0+: the fs client sends 210-213 with the arguments of 200/201/205/206 and gets
        // back a storage (overlayDisp 23.0.1 +0x13adf0: no input, one out object). The applets
        // mount their RomFs through 210. Behaviour follows the Ryujinx Harbor (MIT-licensed
        // Ryujinx base).
        {210, D<&FSP_SRV::OpenDataStorageByCurrentProcess2>, "OpenDataStorageByCurrentProcess2"},
        {212, D<&FSP_SRV::OpenDataStorageWithProgramIndex>, "OpenDataStorageWithProgramIndex2"},
        {400, nullptr, "OpenDeviceOperator"},
        {500, nullptr, "OpenSdCardDetectionEventNotifier"},
        {501, nullptr, "OpenGameCardDetectionEventNotifier"},
        {510, nullptr, "OpenSystemDataUpdateEventNotifier"},
        {511, nullptr, "NotifySystemDataUpdateEvent"},
        {520, nullptr, "SimulateGameCardDetectionEvent"},
        {600, nullptr, "SetCurrentPosixTime"},
        {601, nullptr, "QuerySaveDataTotalSize"},
        {602, nullptr, "VerifySaveDataFileSystem"},
        {603, nullptr, "CorruptSaveDataFileSystem"},
        {604, nullptr, "CreatePaddingFile"},
        {605, nullptr, "DeleteAllPaddingFiles"},
        {606, nullptr, "GetRightsId"},
        {607, nullptr, "RegisterExternalKey"},
        {608, nullptr, "UnregisterAllExternalKey"},
        {609, nullptr, "GetRightsIdByPath"},
        {610, nullptr, "GetRightsIdAndKeyGenerationByPath"},
        {611, nullptr, "SetCurrentPosixTimeWithTimeDifference"},
        {612, nullptr, "GetFreeSpaceSizeForSaveData"},
        {613, nullptr, "VerifySaveDataFileSystemBySaveDataSpaceId"},
        {614, nullptr, "CorruptSaveDataFileSystemBySaveDataSpaceId"},
        {615, nullptr, "QuerySaveDataInternalStorageTotalSize"},
        {616, nullptr, "GetSaveDataCommitId"},
        {617, nullptr, "UnregisterExternalKey"},
        {620, nullptr, "SetSdCardEncryptionSeed"},
        {630, nullptr, "SetSdCardAccessibility"},
        {631, D<&FSP_SRV::IsSdCardAccessible>, "IsSdCardAccessible"},
        {640, nullptr, "IsSignedSystemPartitionOnSdCardValid"},
        {700, nullptr, "OpenAccessFailureResolver"},
        {701, nullptr, "GetAccessFailureDetectionEvent"},
        {702, nullptr, "IsAccessFailureDetected"},
        {710, nullptr, "ResolveAccessFailure"},
        {720, nullptr, "AbandonAccessFailure"},
        {800, nullptr, "GetAndClearFileSystemProxyErrorInfo"},
        {810, nullptr, "RegisterProgramIndexMapInfo"},
        {820, nullptr, "GetContentStorageInfoIndex"},
        {830, nullptr, "EncryptStreamPlaySaveData"},
        {831, nullptr, "DecryptStreamPlaySaveData"},
        {1000, nullptr, "SetBisRootForHost"},
        {1001, nullptr, "SetSaveDataSize"},
        {1002, nullptr, "SetSaveDataRootPath"},
        {1003, D<&FSP_SRV::DisableAutoSaveDataCreation>, "DisableAutoSaveDataCreation"},
        {1004, D<&FSP_SRV::SetGlobalAccessLogMode>, "SetGlobalAccessLogMode"},
        {1005, D<&FSP_SRV::GetGlobalAccessLogMode>, "GetGlobalAccessLogMode"},
        {1006, D<&FSP_SRV::OutputAccessLogToSdCard>, "OutputAccessLogToSdCard"},
        {1007, nullptr, "RegisterUpdatePartition"},
        {1008, nullptr, "OpenRegisteredUpdatePartition"},
        {1009, nullptr, "GetAndClearMemoryReportInfo"},
        {1010, nullptr, "SetDataStorageRedirectTarget"},
        {1011, D<&FSP_SRV::GetProgramIndexForAccessLog>, "GetProgramIndexForAccessLog"},
        {1012, nullptr, "GetFsStackUsage"},
        {1013, nullptr, "UnsetSaveDataRootPath"},
        {1014, nullptr, "OutputMultiProgramTagAccessLog"},
        {1016, D<&FSP_SRV::FlushAccessLogOnSdCard>, "FlushAccessLogOnSdCard"},
        {1017, nullptr, "OutputApplicationInfoAccessLog"},
        {1018, nullptr, "SetDebugOption"},
        {1019, nullptr, "UnsetDebugOption"},
        {1100, nullptr, "OverrideSaveDataTransferTokenSignVerificationKey"},
        {1110, nullptr, "CorruptSaveDataFileSystemBySaveDataSpaceId2"},
        {1200, D<&FSP_SRV::OpenMultiCommitManager>, "OpenMultiCommitManager"},
        {1300, nullptr, "OpenBisWiper"},
    };
    // clang-format on
    RegisterHandlers(functions);

    if (Settings::values.enable_fs_access_log) {
        access_log_mode = AccessLogMode::SdCard;
    }
}

FSP_SRV::~FSP_SRV() = default;

Result FSP_SRV::SetCurrentProcess(ClientProcessId pid) {
    current_process_id = *pid;

    LOG_DEBUG(Service_FS, "called. current_process_id={:#016x}", current_process_id);

    R_RETURN(
        fsc.OpenProcess(&program_id, &save_data_controller, &romfs_controller, current_process_id));
}

Result FSP_SRV::OpenFileSystemWithPatch(OutInterface<IFileSystem> out_interface,
                                        FileSystemProxyType type, u64 open_program_id) {
    LOG_ERROR(Service_FS, "(STUBBED) called with type={}, program_id={:016X}", type,
              open_program_id);

    // FIXME: many issues with this
    ASSERT(type == FileSystemProxyType::Manual);
    const auto manual_romfs = romfs_controller->OpenPatchedRomFS(
        open_program_id, FileSys::ContentRecordType::HtmlDocument);

    ASSERT(manual_romfs != nullptr);

    const auto extracted_romfs = FileSys::ExtractRomFS(manual_romfs);
    ASSERT(extracted_romfs != nullptr);

    *out_interface = std::make_shared<IFileSystem>(
        system, extracted_romfs, SizeGetter::FromStorageId(fsc, FileSys::StorageId::NandUser));

    R_SUCCEED();
}

Result FSP_SRV::OpenSdCardFileSystem(OutInterface<IFileSystem> out_interface) {
    LOG_DEBUG(Service_FS, "called");

    FileSys::VirtualDir sdmc_dir{};
    fsc.OpenSDMC(&sdmc_dir);

    *out_interface = std::make_shared<IFileSystem>(
        system, sdmc_dir, SizeGetter::FromStorageId(fsc, FileSys::StorageId::SdCard));

    R_SUCCEED();
}

Result FSP_SRV::CreateSaveDataFileSystem(FileSys::SaveDataCreationInfo save_create_struct,
                                         FileSys::SaveDataAttribute save_struct,
                                         [[maybe_unused]] u128 uid) {
    LOG_DEBUG(Service_FS, "called save_struct = {}", save_struct.DebugInfo());

    R_RETURN(CreateSaveDataUnlessExists(FileSys::SaveDataSpaceId::User, save_struct,
                                        save_create_struct.owner_id, save_create_struct.flags,
                                        save_create_struct.size, save_create_struct.journal_size,
                                        FileSys::SaveDataFormatType::Normal));
}

Result FSP_SRV::CreateSaveDataFileSystemBySystemSaveDataId(
    FileSys::SaveDataAttribute save_struct, FileSys::SaveDataCreationInfo save_create_struct) {
    LOG_INFO(Service_FS, "program {:016X} creates system save {:016X}, owner {:016X}", program_id,
             save_struct.system_save_data_id, save_create_struct.owner_id);

    R_TRY(CreateSaveDataUnlessExists(FileSys::SaveDataSpaceId::System, save_struct,
                                     save_create_struct.owner_id, save_create_struct.flags,
                                     save_create_struct.size, save_create_struct.journal_size,
                                     FileSys::SaveDataFormatType::Normal));
    RecordSystemSaveDataOwner(save_struct, save_create_struct.owner_id);
    R_SUCCEED();
}

void FSP_SRV::CreateSaveDataFileSystemWithCreationInfo2(HLERequestContext& ctx) {
    // 14.0.0+: buffer<nn::fs::SaveDataCreationInfo2, 0x19>, 0x200 bytes. The Ryujinx Harbor also
    // takes the struct from the raw data when no buffer comes (it saw qlaunch 22.5.0 do that).
    // Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).
    FileSys::SaveDataCreationInfo2 info{};
    const bool from_buffer = ctx.CanReadBuffer(0) && ctx.GetReadBufferSize(0) > 0;
    if (from_buffer) {
        const auto buffer = ctx.ReadBuffer(0);
        std::memcpy(static_cast<void*>(&info), buffer.data(),
                    (std::min)(sizeof(info), buffer.size()));
    } else {
        const std::size_t offset =
            (std::min)(std::size_t{ctx.GetDataPayloadOffset() + 2}, IPC::COMMAND_BUFFER_LENGTH);
        const std::size_t available = (IPC::COMMAND_BUFFER_LENGTH - offset) * sizeof(u32);
        std::memcpy(static_cast<void*>(&info), ctx.CommandBuffer() + offset,
                    (std::min)(sizeof(info), available));
    }

    LOG_INFO(Service_FS,
             "program {:016X} creates save {} in space {}, owner {:016X}, size {:#x}/{:#x}, "
             "version {:#x}{}",
             program_id, info.attribute.DebugInfo(), info.space_id, info.owner_id, info.size,
             info.journal_size, info.version, from_buffer ? "" : " (inline)");

    Result result =
        CreateSaveDataUnlessExists(info.space_id, info.attribute, info.owner_id, info.flags,
                                   info.size, info.journal_size, info.format_type);
    if (result.IsSuccess() && info.space_id == FileSys::SaveDataSpaceId::System) {
        RecordSystemSaveDataOwner(info.attribute, info.owner_id);
    }

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(result);
}

Result FSP_SRV::CreateSaveDataUnlessExists(FileSys::SaveDataSpaceId space,
                                           const FileSys::SaveDataAttribute& attribute,
                                           u64 owner_id, u32 flags, s64 size, s64 journal_size,
                                           FileSys::SaveDataFormatType format_type) {
    R_UNLESS(save_data_controller != nullptr, FileSys::ResultPermissionDenied);

    // A console refuses to create a save that is already there; callers such as the web applets
    // create their saves on that answer's absence.
    if (save_data_controller->SaveDataExists(space, attribute)) {
        LOG_DEBUG(Service_FS, "save {:016X} in space {} already exists",
                  attribute.system_save_data_id, space);
        R_THROW(FileSys::ResultPathAlreadyExists);
    }

    FileSys::VirtualDir save_data_dir{};
    R_TRY(save_data_controller->CreateSaveData(&save_data_dir, space, attribute));
    save_data_controller->InitializeSaveDataExtraData(space, attribute,
                                                      owner_id != 0 ? owner_id : program_id, flags,
                                                      size, journal_size, format_type);
    R_SUCCEED();
}

void FSP_SRV::RecordSystemSaveDataOwner(const FileSys::SaveDataAttribute& attribute,
                                        u64 owner_id) {
    if (attribute.type != FileSys::SaveDataType::System ||
        attribute.system_save_data_id == FileSys::InvalidSystemSaveDataId) {
        return;
    }
    save_data_controller->WriteSystemSaveDataOwner(attribute.system_save_data_id,
                                                   owner_id != 0 ? owner_id : program_id);
}

Result FSP_SRV::DeleteSaveDataFileSystem(u64 save_data_id) {
    // The form without a space names the system save space. A console lets a program delete a
    // system save it owns, or one whose owner its FAC lists with write access; the web applets
    // hold no management right and clear their browser storage (0x8000000000001093) at start.
    // Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).
    R_UNLESS(save_data_controller != nullptr, FileSys::ResultPermissionDenied);

    // fs finds the save in its index before it checks the caller's rights, so an id that names
    // no save answers TargetNotFound whoever asks.
    if (!save_data_controller->SystemSaveDataExists(save_data_id)) {
        LOG_INFO(Service_FS, "program {:016X} deletes system save {:016X}: not found", program_id,
                 save_data_id);
        R_THROW(FileSys::ResultTargetNotFound);
    }

    const auto owner_id = save_data_controller->ReadSystemSaveDataOwner(save_data_id);
    const auto metadata = FileSys::ProgramMetadata::LoadInstalled(content_provider, program_id);

    const bool is_owner = owner_id && *owner_id == program_id;
    const bool lists_owner = owner_id && metadata && metadata->CanWriteSaveDataOwnedBy(*owner_id);
    const bool manages = metadata &&
                         (metadata->GetFilesystemPermissions() & SaveDataManagementPermissions) != 0;

    if (!is_owner && !lists_owner && !manages) {
        LOG_WARNING(Service_FS,
                    "program {:016X} may not delete system save {:016X} (owner {})", program_id,
                    save_data_id, owner_id ? fmt::format("{:016X}", *owner_id) : "unknown");
        R_THROW(FileSys::ResultPermissionDenied);
    }

    const Result result = save_data_controller->DeleteSystemSaveData(save_data_id);
    LOG_INFO(Service_FS, "program {:016X} deleted system save {:016X}: {:#x}", program_id,
             save_data_id, result.raw);
    R_RETURN(result);
}

Result FSP_SRV::IsExFatSupported(Out<bool> out_is_supported) {
    LOG_WARNING(Service_FS, "(STUBBED) called");

    *out_is_supported = true;

    R_SUCCEED();
}

Result FSP_SRV::OpenSaveDataFileSystem(OutInterface<IFileSystem> out_interface,
                                       FileSys::SaveDataSpaceId space_id,
                                       FileSys::SaveDataAttribute attribute) {
    LOG_INFO(Service_FS, "called.");

    FileSys::VirtualDir dir{};
    R_TRY(save_data_controller->OpenSaveData(&dir, space_id, attribute));

    FileSys::StorageId id{};
    switch (space_id) {
    case FileSys::SaveDataSpaceId::User:
        id = FileSys::StorageId::NandUser;
        break;
    case FileSys::SaveDataSpaceId::SdSystem:
    case FileSys::SaveDataSpaceId::SdUser:
        id = FileSys::StorageId::SdCard;
        break;
    case FileSys::SaveDataSpaceId::System:
        id = FileSys::StorageId::NandSystem;
        break;
    case FileSys::SaveDataSpaceId::Temporary:
        // ok this is definitely wrong. ASSERT(false) here just kills the whole game the first
        // time it opens cache storage, and plenty of games do that (TOTK for one). there is
        // user-space scratch storage so it belongs on user nand. map it, do not crash.
        id = FileSys::StorageId::NandUser;
        break;
    case FileSys::SaveDataSpaceId::ProperSystem:
    case FileSys::SaveDataSpaceId::SafeMode:
        // same deal for these two. they are system-level spaces so they go on system nand.
        // way better than nuking the title over a save-space id we just did not list out.
        id = FileSys::StorageId::NandSystem;
        break;
    }

    *out_interface =
        std::make_shared<IFileSystem>(system, std::move(dir), SizeGetter::FromStorageId(fsc, id));

    R_SUCCEED();
}

Result FSP_SRV::OpenSaveDataFileSystemBySystemSaveDataId(OutInterface<IFileSystem> out_interface,
                                                         FileSys::SaveDataSpaceId space_id,
                                                         FileSys::SaveDataAttribute attribute) {
    LOG_INFO(Service_FS, "called, space_id={}, {}",
             space_id, attribute.DebugInfo());

    R_UNLESS(attribute.system_save_data_id != FileSys::InvalidSystemSaveDataId,
             FileSys::ResultInvalidArgument);

    if (attribute.program_id == 0) {
        attribute.program_id = program_id;
    }

    FileSys::VirtualDir dir{};
    R_TRY(save_data_controller->OpenSaveData(&dir, space_id, attribute));

    FileSys::StorageId id{};
    switch (space_id) {
    case FileSys::SaveDataSpaceId::User:
        id = FileSys::StorageId::NandUser;
        break;
    case FileSys::SaveDataSpaceId::SdSystem:
    case FileSys::SaveDataSpaceId::SdUser:
        id = FileSys::StorageId::SdCard;
        break;
    case FileSys::SaveDataSpaceId::System:
        id = FileSys::StorageId::NandSystem;
        break;
    case FileSys::SaveDataSpaceId::Temporary:
        // same broken switch as OpenSaveDataFileSystem above. do not ASSERT(false) and kill the
        // game over a save-space id, just map Temporary to user nand like it should be.
        id = FileSys::StorageId::NandUser;
        break;
    case FileSys::SaveDataSpaceId::ProperSystem:
    case FileSys::SaveDataSpaceId::SafeMode:
        // system spaces -> system nand. handled, not crashed.
        id = FileSys::StorageId::NandSystem;
        break;
    }

    *out_interface =
        std::make_shared<IFileSystem>(system, std::move(dir), SizeGetter::FromStorageId(fsc, id));

    R_SUCCEED();
}

Result FSP_SRV::OpenReadOnlySaveDataFileSystem(OutInterface<IFileSystem> out_interface,
                                               FileSys::SaveDataSpaceId space_id,
                                               FileSys::SaveDataAttribute attribute) {
    LOG_WARNING(Service_FS, "(STUBBED) called, delegating to 51 OpenSaveDataFilesystem");
    R_RETURN(OpenSaveDataFileSystem(out_interface, space_id, attribute));
}

Result FSP_SRV::OpenSaveDataInfoReaderBySaveDataSpaceId(
    OutInterface<ISaveDataInfoReader> out_interface, FileSys::SaveDataSpaceId space) {
    LOG_INFO(Service_FS, "called, space={}", space);

    *out_interface = std::make_shared<ISaveDataInfoReader>(system, save_data_controller, space);

    R_SUCCEED();
}

Result FSP_SRV::OpenSaveDataInfoReaderOnlyCacheStorage(
    OutInterface<ISaveDataInfoReader> out_interface) {
    LOG_WARNING(Service_FS, "(STUBBED) called");

    *out_interface = std::make_shared<ISaveDataInfoReader>(system, save_data_controller,
                                                           FileSys::SaveDataSpaceId::Temporary);

    R_SUCCEED();
}

Result FSP_SRV::FindSaveDataWithFilter(Out<s64> out_count,
                                       OutBuffer<BufferAttr_HipcMapAlias> out_buffer,
                                       FileSys::SaveDataSpaceId space_id,
                                       FileSys::SaveDataFilter filter) {
    // fs answers success with the number of matches written (0 when none); the client turns
    // 0 into TargetNotFound.
    using SaveDataInfo = ISaveDataInfoReader::SaveDataInfo;
    *out_count = 0;
    R_UNLESS(save_data_controller != nullptr, FileSys::ResultPermissionDenied);

    const auto matches = [&filter](const SaveDataInfo& info) {
        const auto& attribute = filter.attribute;
        if (filter.use_program_id && info.title_id != attribute.program_id) {
            return false;
        }
        if (filter.use_save_data_type && info.type != attribute.type) {
            return false;
        }
        if (filter.use_user_id &&
            std::memcmp(info.user_id.data(), attribute.user_id.data(), info.user_id.size()) != 0) {
            return false;
        }
        if (filter.use_save_data_id && info.save_id != attribute.system_save_data_id) {
            return false;
        }
        if (filter.use_index && info.index != attribute.index) {
            return false;
        }
        return info.rank == filter.rank;
    };

    const std::size_t capacity = out_buffer.size() / sizeof(SaveDataInfo);
    s64 count = 0;
    for (const auto& info : ISaveDataInfoReader::FindAllSaves(*save_data_controller, space_id)) {
        if (static_cast<std::size_t>(count) >= capacity) {
            break;
        }
        if (matches(info)) {
            std::memcpy(out_buffer.data() + count * sizeof(SaveDataInfo), &info,
                        sizeof(SaveDataInfo));
            ++count;
        }
    }
    *out_count = count;

    LOG_DEBUG(Service_FS,
              "space {}, filter program={} type={} user={} id={} index={} rank={} {}: {} found",
              space_id, filter.use_program_id, filter.use_save_data_type, filter.use_user_id,
              filter.use_save_data_id, filter.use_index, static_cast<u8>(filter.rank),
              filter.attribute.DebugInfo(), count);
    R_SUCCEED();
}

// Extra data (attribute, owner, timestamp, flags, sizes, commit id) is kept per save, outside
// the save, from its creation on. Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx
// base), whose LibHac keeps it in the save image.

FileSys::SaveDataAttribute FSP_SRV::ResolveExtraDataAttribute(
    FileSys::SaveDataAttribute attribute) const {
    // fs resolves the "caller" program id to the calling program.
    constexpr u64 AutoResolveCallerProgramId = ~u64{0} - 1;
    if (attribute.program_id == AutoResolveCallerProgramId) {
        attribute.program_id = program_id;
    }
    return attribute;
}

Result FSP_SRV::FindSaveDataById(FileSys::SaveDataAttribute* out_attribute,
                                 FileSys::SaveDataSpaceId space_id, u64 save_data_id) const {
    R_UNLESS(save_data_controller != nullptr, FileSys::ResultPermissionDenied);
    if (save_data_controller->FindSaveDataById(out_attribute, space_id, save_data_id).IsSuccess()) {
        R_SUCCEED();
    }

    // Saves without a static id go by the id the save list gives them.
    for (const auto& info : ISaveDataInfoReader::FindAllSaves(*save_data_controller, space_id)) {
        if (info.save_id_unknown != save_data_id || info.type == FileSys::SaveDataType::System) {
            continue;
        }
        FileSys::UserId user_id{};
        std::memcpy(user_id.data(), info.user_id.data(), sizeof(user_id));
        *out_attribute = FileSys::SaveDataAttribute::Make(info.title_id, info.type, user_id,
                                                          info.save_id, info.index, info.rank);
        R_SUCCEED();
    }
    R_THROW(FileSys::ResultTargetNotFound);
}

Result FSP_SRV::ReadExtraDataById(OutBuffer<BufferAttr_HipcMapAlias> out_buffer,
                                  FileSys::SaveDataSpaceId space_id, u64 save_data_id) {
    R_UNLESS(save_data_controller != nullptr, FileSys::ResultPermissionDenied);
    FileSys::SaveDataAttribute attribute{};
    R_TRY(FindSaveDataById(&attribute, space_id, save_data_id));
    R_RETURN(ReadExtraData(out_buffer, space_id, attribute, {}));
}

Result FSP_SRV::ReadExtraData(OutBuffer<BufferAttr_HipcMapAlias> out_buffer,
                              FileSys::SaveDataSpaceId space_id,
                              const FileSys::SaveDataAttribute& attribute,
                              std::span<const u8> mask) {
    R_UNLESS(save_data_controller != nullptr, FileSys::ResultPermissionDenied);
    FileSys::SaveDataExtraData extra_data{};
    R_TRY(save_data_controller->ReadSaveDataExtraData(&extra_data, space_id, attribute));

    // A mask keeps only the fields it covers; the rest read as zero.
    std::array<u8, sizeof(extra_data)> bytes{};
    std::memcpy(bytes.data(), &extra_data, bytes.size());
    if (!mask.empty()) {
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            bytes[i] &= i < mask.size() ? mask[i] : u8{0};
        }
    }

    std::memset(out_buffer.data(), 0, out_buffer.size());
    std::memcpy(out_buffer.data(), bytes.data(), (std::min)(out_buffer.size(), bytes.size()));
    LOG_DEBUG(Service_FS, "save {:016X} (type {}): owner {:016X}, sizes {:#x}/{:#x}{}",
              attribute.system_save_data_id, static_cast<u8>(attribute.type), extra_data.owner_id,
              extra_data.available_size, extra_data.journal_size, mask.empty() ? "" : " (masked)");
    R_SUCCEED();
}

Result FSP_SRV::WriteExtraData(FileSys::SaveDataSpaceId space_id,
                               const FileSys::SaveDataAttribute& attribute,
                               std::span<const u8> data, std::span<const u8> mask) {
    R_UNLESS(save_data_controller != nullptr, FileSys::ResultPermissionDenied);
    FileSys::SaveDataExtraData extra_data{};
    R_TRY(save_data_controller->ReadSaveDataExtraData(&extra_data, space_id, attribute));

    // Without a mask the whole buffer is written; with one only the bytes it covers.
    std::array<u8, sizeof(extra_data)> bytes{};
    std::memcpy(bytes.data(), &extra_data, bytes.size());
    for (std::size_t i = 0; i < bytes.size() && i < data.size(); ++i) {
        const u8 keep = mask.empty() ? u8{0xFF} : (i < mask.size() ? mask[i] : u8{0});
        bytes[i] = static_cast<u8>((bytes[i] & ~keep) | (data[i] & keep));
    }
    std::memcpy(&extra_data, bytes.data(), bytes.size());

    LOG_INFO(Service_FS,
             "program {:016X} writes the extra data of save {:016X} (type {}){}: flags {:#x}, "
             "sizes {:#x}/{:#x}",
             program_id, attribute.system_save_data_id, static_cast<u8>(attribute.type),
             mask.empty() ? "" : " with a mask", extra_data.flags, extra_data.available_size,
             extra_data.journal_size);
    R_RETURN(save_data_controller->WriteSaveDataExtraData(space_id, attribute, extra_data));
}

Result FSP_SRV::WriteSaveDataFileSystemExtraData(InBuffer<BufferAttr_HipcMapAlias> buffer,
                                                 FileSys::SaveDataSpaceId space_id,
                                                 u64 save_data_id) {
    R_UNLESS(save_data_controller != nullptr, FileSys::ResultPermissionDenied);
    FileSys::SaveDataAttribute attribute{};
    R_TRY(FindSaveDataById(&attribute, space_id, save_data_id));
    R_RETURN(WriteExtraData(space_id, attribute, buffer, {}));
}

Result FSP_SRV::WriteSaveDataFileSystemExtraDataWithMaskBySaveDataAttribute(
    InBuffer<BufferAttr_HipcMapAlias> buffer, InBuffer<BufferAttr_HipcMapAlias> mask_buffer,
    FileSys::SaveDataSpaceId space_id, FileSys::SaveDataAttribute attribute) {
    R_RETURN(WriteExtraData(space_id, ResolveExtraDataAttribute(attribute), buffer, mask_buffer));
}

Result FSP_SRV::ReadSaveDataFileSystemExtraDataWithMaskBySaveDataAttribute(
    FileSys::SaveDataSpaceId space_id, FileSys::SaveDataAttribute attribute,
    InBuffer<BufferAttr_HipcMapAlias> mask_buffer, OutBuffer<BufferAttr_HipcMapAlias> out_buffer) {
    R_RETURN(
        ReadExtraData(out_buffer, space_id, ResolveExtraDataAttribute(attribute), mask_buffer));
}

Result FSP_SRV::ReadSaveDataFileSystemExtraData(OutBuffer<BufferAttr_HipcMapAlias> out_buffer,
                                                u64 save_data_id) {
    R_RETURN(ReadExtraDataById(out_buffer, FileSys::SaveDataSpaceId::System, save_data_id));
}

Result FSP_SRV::ReadSaveDataFileSystemExtraDataBySaveDataAttribute(
    OutBuffer<BufferAttr_HipcMapAlias> out_buffer, FileSys::SaveDataSpaceId space_id,
    FileSys::SaveDataAttribute attribute) {
    R_RETURN(ReadExtraData(out_buffer, space_id, ResolveExtraDataAttribute(attribute), {}));
}

Result FSP_SRV::ReadSaveDataFileSystemExtraDataBySaveDataSpaceId(
    OutBuffer<BufferAttr_HipcMapAlias> out_buffer, FileSys::SaveDataSpaceId space_id,
    u64 save_data_id) {
    R_RETURN(ReadExtraDataById(out_buffer, space_id, save_data_id));
}

Result FSP_SRV::OpenSaveDataTransferProhibiter(
    OutInterface<ISaveDataTransferProhibiter> out_prohibiter, u64 id) {
    LOG_WARNING(Service_FS, "(STUBBED) called, id={:016X}", id);
    *out_prohibiter = std::make_shared<ISaveDataTransferProhibiter>(system);
    R_SUCCEED();
}

Result FSP_SRV::OpenDataFileSystemByCurrentProcess(OutInterface<IFileSystem> out_interface) {
    LOG_DEBUG(Service_FS, "called");

    if (!romfs) {
        auto current_romfs = romfs_controller->OpenRomFSCurrentProcess();
        if (!current_romfs) {
            LOG_CRITICAL(Service_FS, "No file system interface available!");
            R_RETURN(ResultUnknown);
        }

        romfs = current_romfs;
    }

    auto extracted_romfs = FileSys::ExtractRomFS(romfs);
    if (!extracted_romfs) {
        LOG_CRITICAL(Service_FS, "Failed to extract RomFS for the current process!");
        R_RETURN(ResultUnknown);
    }

    *out_interface = std::make_shared<IFileSystem>(
        system, extracted_romfs, SizeGetter::FromStorageId(fsc, FileSys::StorageId::NandUser));

    R_SUCCEED();
}

Result FSP_SRV::OpenDataStorageByCurrentProcess(OutInterface<IStorage> out_interface) {
    LOG_DEBUG(Service_FS, "called");

    if (romfs_controller == nullptr) {
        LOG_ERROR(Service_FS, "No process set on this session (SetCurrentProcess not called)");
        R_RETURN(ResultUnknown);
    }

    if (!romfs) {
        auto current_romfs = romfs_controller->OpenRomFSCurrentProcess();
        if (!current_romfs) {
            // TODO (bunnei): Find the right error code to use here
            LOG_CRITICAL(Service_FS, "No file system interface available!");
            R_RETURN(ResultUnknown);
        }

        romfs = current_romfs;
    }

    *out_interface = std::make_shared<IStorage>(system, romfs);

    R_SUCCEED();
}

Result FSP_SRV::OpenDataStorageByCurrentProcess2(OutInterface<IStorage> out_interface) {
    LOG_DEBUG(Service_FS, "called (23.0.0+ command 210), program_id={:016X}", program_id);
    R_RETURN(OpenDataStorageByCurrentProcess(out_interface));
}

Result FSP_SRV::OpenDataStorageByDataId(OutInterface<IStorage> out_interface,
                                        FileSys::StorageId storage_id, u32 unknown, u64 title_id) {
    LOG_DEBUG(Service_FS, "called with storage_id={:02X}, unknown={:08X}, title_id={:016X}",
              storage_id, unknown, title_id);

    auto data = romfs_controller->OpenRomFS(title_id, storage_id, FileSys::ContentRecordType::Data);

    if (!data) {
        const auto archive = FileSys::SystemArchive::SynthesizeSystemArchive(title_id);

        if (archive != nullptr) {
            *out_interface = std::make_shared<IStorage>(system, archive);
            R_SUCCEED();
        }

        // TODO(DarkLordZach): Find the right error code to use here
        LOG_ERROR(Service_FS,
                  "Could not open data storage with title_id={:016X}, storage_id={:02X}", title_id,
                  storage_id);
        R_RETURN(ResultUnknown);
    }

    const FileSys::PatchManager pm{title_id, fsc, content_provider};

    auto base =
        romfs_controller->OpenBaseNca(title_id, storage_id, FileSys::ContentRecordType::Data);
    auto storage = std::make_shared<IStorage>(
        system, pm.PatchRomFS(base.get(), std::move(data), FileSys::ContentRecordType::Data));

    *out_interface = std::move(storage);
    R_SUCCEED();
}

Result FSP_SRV::OpenPatchDataStorageByCurrentProcess(OutInterface<IStorage> out_interface,
                                                     FileSys::StorageId storage_id, u64 title_id) {
    LOG_WARNING(Service_FS, "(STUBBED) called with storage_id={:02X}, title_id={:016X}", storage_id,
                title_id);

    R_RETURN(FileSys::ResultTargetNotFound);
}

Result FSP_SRV::OpenDataStorageWithProgramIndex(OutInterface<IStorage> out_interface,
                                                u8 program_index) {
    LOG_DEBUG(Service_FS, "called, program_index={}", program_index);

    auto patched_romfs = romfs_controller->OpenPatchedRomFSWithProgramIndex(
        program_id, program_index, FileSys::ContentRecordType::Program);

    if (!patched_romfs) {
        // TODO: Find the right error code to use here
        LOG_ERROR(Service_FS, "Could not open storage with program_index={}", program_index);
        R_RETURN(ResultUnknown);
    }

    *out_interface = std::make_shared<IStorage>(system, std::move(patched_romfs));

    R_SUCCEED();
}

Result FSP_SRV::IsSdCardAccessible(Out<bool> out_is_accessible) {
    LOG_DEBUG(Service_FS, "(STUBBED) called");

    *out_is_accessible = true;

    R_SUCCEED();
}

Result FSP_SRV::DisableAutoSaveDataCreation() {
    LOG_DEBUG(Service_FS, "called");

    save_data_controller->SetAutoCreate(false);

    R_SUCCEED();
}

Result FSP_SRV::SetGlobalAccessLogMode(AccessLogMode access_log_mode_) {
    LOG_DEBUG(Service_FS, "called, access_log_mode={}", access_log_mode_);

    access_log_mode = access_log_mode_;

    R_SUCCEED();
}

Result FSP_SRV::GetGlobalAccessLogMode(Out<AccessLogMode> out_access_log_mode) {
    LOG_DEBUG(Service_FS, "called");

    *out_access_log_mode = access_log_mode;

    R_SUCCEED();
}

Result FSP_SRV::OutputAccessLogToSdCard(InBuffer<BufferAttr_HipcMapAlias> log_message_buffer) {
    LOG_DEBUG(Service_FS, "called");

    auto log = Common::StringFromFixedZeroTerminatedBuffer(
        reinterpret_cast<const char*>(log_message_buffer.data()), log_message_buffer.size());
    reporter.SaveFSAccessLog(log);

    R_SUCCEED();
}

Result FSP_SRV::GetProgramIndexForAccessLog(Out<AccessLogVersion> out_access_log_version,
                                            Out<u32> out_access_log_program_index) {
    LOG_DEBUG(Service_FS, "(STUBBED) called");

    *out_access_log_version = AccessLogVersion::Latest;
    *out_access_log_program_index = access_log_program_index;

    R_SUCCEED();
}

Result FSP_SRV::FlushAccessLogOnSdCard() {
    LOG_DEBUG(Service_FS, "(STUBBED) called");

    R_SUCCEED();
}

Result FSP_SRV::ExtendSaveDataFileSystem(FileSys::SaveDataSpaceId space_id, u64 save_data_id,
                                         s64 available_size, s64 journal_size) {
    // Saves here are host directories with no size limit, so extending a save only records its
    // new sizes in the extra data, where its owner reads them back.
    R_UNLESS(save_data_controller != nullptr, FileSys::ResultPermissionDenied);
    FileSys::SaveDataAttribute attribute{};
    R_TRY(FindSaveDataById(&attribute, space_id, save_data_id));

    FileSys::SaveDataExtraData extra_data{};
    R_TRY(save_data_controller->ReadSaveDataExtraData(&extra_data, space_id, attribute));
    LOG_INFO(Service_FS, "program {:016X} extends save {:016X}: {:#x}/{:#x} -> {:#x}/{:#x}",
             program_id, save_data_id, extra_data.available_size, extra_data.journal_size,
             available_size, journal_size);
    extra_data.available_size = available_size;
    extra_data.journal_size = journal_size;
    R_RETURN(save_data_controller->WriteSaveDataExtraData(space_id, attribute, extra_data));
}

Result FSP_SRV::GetCacheStorageSize(s32 index, Out<s64> out_data_size, Out<s64> out_journal_size) {
    LOG_WARNING(Service_FS, "(STUBBED) called with index={}", index);

    *out_data_size = 0;
    *out_journal_size = 0;

    R_SUCCEED();
}

Result FSP_SRV::OpenMultiCommitManager(OutInterface<IMultiCommitManager> out_interface) {
    LOG_DEBUG(Service_FS, "called");

    *out_interface = std::make_shared<IMultiCommitManager>(system);

    R_SUCCEED();
}

} // namespace Service::FileSystem

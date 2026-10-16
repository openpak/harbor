// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <span>
#include <string>
#include "core/file_sys/fs_save_data_types.h"
#include "core/hle/service/cmif_types.h"
#include "core/hle/service/filesystem/fsp/fsp_types.h"
#include "core/hle/service/service.h"

namespace Core {
class Reporter;
}

namespace FileSys {
class ContentProvider;
class FileSystemBackend;
} // namespace FileSys

namespace Service::FileSystem {

class RomFsController;
class SaveDataController;

class IFileSystem;
class ISaveDataInfoReader;
class ISaveDataTransferProhibiter;
namespace SaveTransfer {
class ISaveDataTransferManagerVersion2;
}
class IStorage;
class IMultiCommitManager;

enum class AccessLogVersion : u32 {
    V7_0_0 = 2,

    Latest = V7_0_0,
};

enum class AccessLogMode : u32 {
    None,
    Log,
    SdCard,
};

class IEventNotifier;

class FSP_SRV final : public ServiceFramework<FSP_SRV> {
public:
    explicit FSP_SRV(Core::System& system_);
    ~FSP_SRV() override;

private:
    Result SetCurrentProcess(ClientProcessId pid);
    Result OpenDataFileSystemByCurrentProcess(OutInterface<IFileSystem> out_interface);
    Result OpenFileSystemWithPatch(OutInterface<IFileSystem> out_interface,
                                   FileSystemProxyType type, u64 open_program_id);
    Result OpenSdCardFileSystem(OutInterface<IFileSystem> out_interface);
    Result OpenEventNotifier(OutInterface<IEventNotifier> out_interface);
    Result CreateSaveDataFileSystem(FileSys::SaveDataCreationInfo save_create_struct,
                                    FileSys::SaveDataAttribute save_struct, u128 uid);
    Result CreateSaveDataFileSystemBySystemSaveDataId(
        FileSys::SaveDataAttribute save_struct, FileSys::SaveDataCreationInfo save_create_struct);
    void CreateSaveDataFileSystemWithCreationInfo2(HLERequestContext& ctx);
    Result CreateSaveDataUnlessExists(FileSys::SaveDataSpaceId space,
                                      const FileSys::SaveDataAttribute& attribute, u64 owner_id,
                                      u32 flags, s64 size, s64 journal_size,
                                      FileSys::SaveDataFormatType format_type);
    void RecordSystemSaveDataOwner(const FileSys::SaveDataAttribute& attribute, u64 owner_id);
    Result DeleteSaveDataFileSystem(u64 save_data_id);
    Result IsExFatSupported(Out<bool> out_is_supported);
    Result OpenSaveDataFileSystem(OutInterface<IFileSystem> out_interface,
                                  FileSys::SaveDataSpaceId space_id,
                                  FileSys::SaveDataAttribute attribute);
    Result OpenSaveDataFileSystemBySystemSaveDataId(OutInterface<IFileSystem> out_interface,
                                                    FileSys::SaveDataSpaceId space_id,
                                                    FileSys::SaveDataAttribute attribute);
    Result OpenReadOnlySaveDataFileSystem(OutInterface<IFileSystem> out_interface,
                                          FileSys::SaveDataSpaceId space_id,
                                          FileSys::SaveDataAttribute attribute);
    Result OpenSaveDataInfoReaderBySaveDataSpaceId(OutInterface<ISaveDataInfoReader> out_interface,
                                                   FileSys::SaveDataSpaceId space);
    Result OpenSaveDataInfoReaderOnlyCacheStorage(OutInterface<ISaveDataInfoReader> out_interface);
    Result FindSaveDataWithFilter(Out<s64> out_count, OutBuffer<BufferAttr_HipcMapAlias> out_buffer,
                                  FileSys::SaveDataSpaceId space_id,
                                  FileSys::SaveDataFilter filter);
    Result WriteSaveDataFileSystemExtraData(InBuffer<BufferAttr_HipcMapAlias> buffer,
                                            FileSys::SaveDataSpaceId space_id, u64 save_data_id);
    Result WriteSaveDataFileSystemExtraDataWithMaskBySaveDataAttribute(
        InBuffer<BufferAttr_HipcMapAlias> buffer, InBuffer<BufferAttr_HipcMapAlias> mask_buffer,
        FileSys::SaveDataSpaceId space_id, FileSys::SaveDataAttribute attribute);
    Result ReadSaveDataFileSystemExtraData(OutBuffer<BufferAttr_HipcMapAlias> out_buffer,
                                           u64 save_data_id);
    Result ReadSaveDataFileSystemExtraDataBySaveDataAttribute(
        OutBuffer<BufferAttr_HipcMapAlias> out_buffer, FileSys::SaveDataSpaceId space_id,
        FileSys::SaveDataAttribute attribute);
    Result ReadSaveDataFileSystemExtraDataBySaveDataSpaceId(
        OutBuffer<BufferAttr_HipcMapAlias> out_buffer, FileSys::SaveDataSpaceId space_id,
        u64 save_data_id);
    Result ReadSaveDataFileSystemExtraDataWithMaskBySaveDataAttribute(
        FileSys::SaveDataSpaceId space_id, FileSys::SaveDataAttribute attribute,
        InBuffer<BufferAttr_HipcMapAlias> mask_buffer,
        OutBuffer<BufferAttr_HipcMapAlias> out_buffer);
    FileSys::SaveDataAttribute ResolveExtraDataAttribute(
        FileSys::SaveDataAttribute attribute) const;
    std::string DescribeUser(const FileSys::UserId& user_id) const;
    Result FindSaveDataById(FileSys::SaveDataAttribute* out_attribute,
                            FileSys::SaveDataSpaceId space_id, u64 save_data_id) const;
    Result QuerySaveDataTotalSize(Out<s64> out_size, s64 data_size, s64 journal_size);
    Result GetSaveDataCommitId(Out<s64> out_commit_id, FileSys::SaveDataSpaceId space_id,
                               u64 save_data_id);
    Result ReadExtraDataById(OutBuffer<BufferAttr_HipcMapAlias> out_buffer,
                             FileSys::SaveDataSpaceId space_id, u64 save_data_id);
    Result ReadExtraData(OutBuffer<BufferAttr_HipcMapAlias> out_buffer,
                         FileSys::SaveDataSpaceId space_id,
                         const FileSys::SaveDataAttribute& attribute, std::span<const u8> mask);
    Result WriteExtraData(FileSys::SaveDataSpaceId space_id,
                          const FileSys::SaveDataAttribute& attribute, std::span<const u8> data,
                          std::span<const u8> mask);
    Result OpenSaveDataTransferManagerVersion2(
        OutInterface<SaveTransfer::ISaveDataTransferManagerVersion2> out_manager);
    Result OpenSaveDataTransferProhibiter(OutInterface<ISaveDataTransferProhibiter> out_prohibiter,
                                          u64 id);
    Result OpenDataStorageByCurrentProcess(OutInterface<IStorage> out_interface);
    Result OpenDataStorageByCurrentProcess2(OutInterface<IStorage> out_interface);
    Result OpenDataStorageByDataId(OutInterface<IStorage> out_interface,
                                   FileSys::StorageId storage_id, u32 unknown, u64 title_id);
    Result OpenPatchDataStorageByCurrentProcess(OutInterface<IStorage> out_interface,
                                                FileSys::StorageId storage_id, u64 title_id);
    Result OpenDataStorageWithProgramIndex(OutInterface<IStorage> out_interface, u8 program_index);
    Result IsSdCardAccessible(Out<bool> out_is_accessible);
    Result DisableAutoSaveDataCreation();
    Result SetGlobalAccessLogMode(AccessLogMode access_log_mode_);
    Result GetGlobalAccessLogMode(Out<AccessLogMode> out_access_log_mode);
    Result OutputAccessLogToSdCard(InBuffer<BufferAttr_HipcMapAlias> log_message_buffer);
    Result FlushAccessLogOnSdCard();
    Result GetProgramIndexForAccessLog(Out<AccessLogVersion> out_access_log_version,
                                       Out<u32> out_access_log_program_index);
    Result OpenMultiCommitManager(OutInterface<IMultiCommitManager> out_interface);
    Result ExtendSaveDataFileSystem(FileSys::SaveDataSpaceId space_id, u64 save_data_id,
                                    s64 available_size, s64 journal_size);
    Result GetCacheStorageSize(s32 index, Out<s64> out_data_size, Out<s64> out_journal_size);

    FileSystemController& fsc;
    const FileSys::ContentProvider& content_provider;
    const Core::Reporter& reporter;

    FileSys::VirtualFile romfs;
    u64 current_process_id = 0;
    u32 access_log_program_index = 0;
    AccessLogMode access_log_mode = AccessLogMode::None;
    u64 program_id = 0;
    std::shared_ptr<SaveDataController> save_data_controller;
    std::shared_ptr<RomFsController> romfs_controller;
};

} // namespace Service::FileSystem

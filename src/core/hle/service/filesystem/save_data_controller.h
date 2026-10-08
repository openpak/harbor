// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>

#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/savedata_factory.h"
#include "core/file_sys/vfs/vfs_types.h"

namespace Service::FileSystem {

class SaveDataController {
public:
    explicit SaveDataController(Core::System& system,
                                std::shared_ptr<FileSys::SaveDataFactory> factory_);
    ~SaveDataController();

    Result CreateSaveData(FileSys::VirtualDir* out_save_data, FileSys::SaveDataSpaceId space,
                          const FileSys::SaveDataAttribute& attribute);
    Result OpenSaveData(FileSys::VirtualDir* out_save_data, FileSys::SaveDataSpaceId space,
                        const FileSys::SaveDataAttribute& attribute);
    bool SaveDataExists(FileSys::SaveDataSpaceId space,
                        const FileSys::SaveDataAttribute& attribute) const;
    Result OpenSaveDataSpace(FileSys::VirtualDir* out_save_data_space,
                             FileSys::SaveDataSpaceId space);

    FileSys::SaveDataSize ReadSaveDataSize(FileSys::SaveDataType type, u64 title_id, u128 user_id);
    void WriteSaveDataSize(FileSys::SaveDataType type, u64 title_id, u128 user_id,
                           FileSys::SaveDataSize new_value);
    void SetAutoCreate(bool state);

    std::optional<u64> ReadSystemSaveDataOwner(u64 save_id) const;
    void WriteSystemSaveDataOwner(u64 save_id, u64 owner_id);
    Result DeleteSystemSaveData(u64 save_id);
    bool SystemSaveDataExists(u64 save_id) const;

    /// The attribute of the save that save_id names in the space. fs indexes system saves under
    /// their static id; saves in the other spaces have no id here.
    Result FindSaveDataById(FileSys::SaveDataAttribute* out_attribute,
                            FileSys::SaveDataSpaceId space, u64 save_id) const;
    /// The save's extra data. A save made before extra data was kept gets one built from what
    /// is known of it (attribute, owner); its sizes read 0 until it is extended.
    Result ReadSaveDataExtraData(FileSys::SaveDataExtraData* out_extra_data,
                                 FileSys::SaveDataSpaceId space,
                                 const FileSys::SaveDataAttribute& attribute) const;
    Result WriteSaveDataExtraData(FileSys::SaveDataSpaceId space,
                                  const FileSys::SaveDataAttribute& attribute,
                                  const FileSys::SaveDataExtraData& extra_data);
    /// The extra data fs writes when it creates a save.
    void InitializeSaveDataExtraData(FileSys::SaveDataSpaceId space,
                                     const FileSys::SaveDataAttribute& attribute, u64 owner_id,
                                     u32 flags, s64 size, s64 journal_size,
                                     FileSys::SaveDataFormatType format_type);

private:
    Core::System& system;
    const std::shared_ptr<FileSys::SaveDataFactory> factory;
};

} // namespace Service::FileSystem

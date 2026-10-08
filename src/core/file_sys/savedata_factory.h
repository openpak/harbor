// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <optional>
#include <string>
#include "common/common_funcs.h"
#include "common/common_types.h"
#include "core/file_sys/fs_save_data_types.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/hle/result.h"

namespace Core {
class System;
}

namespace FileSys {

constexpr const char* GetSaveDataSizeFileName() {
    return ".yuzu_save_size";
}

using ProgramId = u64;

/// File system interface to the SaveData archive
class SaveDataFactory {
public:
    explicit SaveDataFactory(Core::System& system_, ProgramId program_id_,
                             VirtualDir save_directory_);
    ~SaveDataFactory();

    VirtualDir Create(SaveDataSpaceId space, const SaveDataAttribute& meta) const;
    VirtualDir Open(SaveDataSpaceId space, const SaveDataAttribute& meta) const;
    /// Whether the save exists, without creating it the way Open may.
    bool Exists(SaveDataSpaceId space, const SaveDataAttribute& meta) const;

    VirtualDir GetSaveDataSpaceDirectory(SaveDataSpaceId space) const;

    static std::string GetSaveDataSpaceIdPath(SaveDataSpaceId space);
    static std::string GetFullPath(ProgramId program_id, VirtualDir dir, SaveDataSpaceId space,
                                   SaveDataType type, u64 title_id, u128 user_id, u64 save_id);
    static std::string GetUserGameSaveDataRoot(u128 user_id, bool future);

    SaveDataSize ReadSaveDataSize(SaveDataType type, u64 title_id, u128 user_id) const;
    void WriteSaveDataSize(SaveDataType type, u64 title_id, u128 user_id,
                           SaveDataSize new_value) const;

    void SetAutoCreate(bool state);

    /// The owner recorded when system save save_id was created here, if any. Kept under
    /// /system/save/.harbor, outside every save.
    std::optional<u64> ReadSystemSaveDataOwner(u64 save_id) const;
    void WriteSystemSaveDataOwner(u64 save_id, u64 owner_id) const;
    /// Removes system save save_id for every user, with its owner record.
    Result DeleteSystemSaveData(u64 save_id) const;
    /// Whether system save save_id exists for any user.
    bool SystemSaveDataExists(u64 save_id) const;
    /// The attribute of system save save_id (its first user directory), if it exists.
    std::optional<SaveDataAttribute> FindSystemSaveData(u64 save_id) const;

    /// The save's extra data, kept under /system/save/.harbor/extra, outside every save, if
    /// any was written.
    std::optional<SaveDataExtraData> ReadExtraData(SaveDataSpaceId space,
                                                   const SaveDataAttribute& meta) const;
    bool WriteExtraData(SaveDataSpaceId space, const SaveDataAttribute& meta,
                        const SaveDataExtraData& extra_data) const;

private:
    Core::System& system;
    ProgramId program_id;
    VirtualDir dir;
    bool auto_create{true};
};

} // namespace FileSys

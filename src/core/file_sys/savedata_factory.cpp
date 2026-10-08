// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <charconv>

#include "common/assert.h"
#include "common/common_types.h"
#include "common/logging.h"
#include "common/uuid.h"
#include "core/core.h"
#include "core/file_sys/errors.h"
#include "core/file_sys/savedata_factory.h"
#include "core/file_sys/vfs/vfs.h"

namespace FileSys {

namespace {

bool ShouldSaveDataBeAutomaticallyCreated(SaveDataSpaceId space, const SaveDataAttribute& attr) {
    // [OpenPak] A console provisions a title's BCAT delivery cache itself. Splatoon 3 opens it
    // at startup and, refused, retried every ~500 ms forever.
    return attr.type == SaveDataType::Cache || attr.type == SaveDataType::Temporary ||
           attr.type == SaveDataType::Bcat ||
           (space == SaveDataSpaceId::User && ///< Normal Save Data -- Current Title & User
            (attr.type == SaveDataType::Account || attr.type == SaveDataType::Device) &&
            attr.program_id == 0 && attr.system_save_data_id == 0);
}

// Owner records of system saves, one u64 file per save id. They live outside every save so no
// program mounting or copying a save sees them; save enumeration skips dot directories.
constexpr const char* SystemSaveDataOwnersPath = "/system/save/.harbor/owners";

// Extra data of every save, one 0x200-byte file per save, named after the save's path with '/'
// turned into '-' (for example system-save-8000000000001063-000...0).
constexpr const char* SaveDataExtraDataPath = "/system/save/.harbor/extra";

std::string GetExtraDataFileName(std::string_view save_path) {
    while (!save_path.empty() && save_path.front() == '/') {
        save_path.remove_prefix(1);
    }
    std::string name{save_path};
    std::replace(name.begin(), name.end(), '/', '-');
    return name;
}

std::optional<u64> ParseHex64(std::string_view text) {
    if (text.size() != 16) {
        return std::nullopt;
    }
    u64 value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::string GetFutureSaveDataPath(SaveDataSpaceId space_id, SaveDataType type, u64 title_id,
                                  u128 user_id) {
    // Only detect nand user saves.
    const auto space_id_path = [space_id]() -> std::string_view {
        switch (space_id) {
        case SaveDataSpaceId::User:
            return "/user/save";
        default:
            return "";
        }
    }();

    if (space_id_path.empty()) {
        return "";
    }

    Common::UUID uuid;
    std::memcpy(uuid.uuid.data(), user_id.data(), sizeof(Common::UUID));

    // Only detect account/device saves from the future location.
    switch (type) {
    case SaveDataType::Account:
        return fmt::format("{}/account/{}/{:016X}/0", space_id_path, uuid.RawString(), title_id);
    case SaveDataType::Device:
        return fmt::format("{}/device/{:016X}/0", space_id_path, title_id);
    default:
        return "";
    }
}

} // Anonymous namespace

SaveDataFactory::SaveDataFactory(Core::System& system_, ProgramId program_id_,
                                 VirtualDir save_directory_)
    : system{system_}, program_id{program_id_}, dir{std::move(save_directory_)} {}

SaveDataFactory::~SaveDataFactory() = default;

VirtualDir SaveDataFactory::Create(SaveDataSpaceId space, const SaveDataAttribute& meta) const {
    const auto save_directory = GetFullPath(program_id, dir, space, meta.type, meta.program_id,
                                            meta.user_id, meta.system_save_data_id);

    return dir->CreateDirectoryRelative(save_directory);
}

VirtualDir SaveDataFactory::Open(SaveDataSpaceId space, const SaveDataAttribute& meta) const {

    const auto save_directory = GetFullPath(program_id, dir, space, meta.type, meta.program_id,
                                            meta.user_id, meta.system_save_data_id);

    auto out = dir->GetDirectoryRelative(save_directory);

    if (out == nullptr && (ShouldSaveDataBeAutomaticallyCreated(space, meta) && auto_create)) {
        return Create(space, meta);
    }

    return out;
}

bool SaveDataFactory::Exists(SaveDataSpaceId space, const SaveDataAttribute& meta) const {
    const auto save_directory = GetFullPath(program_id, dir, space, meta.type, meta.program_id,
                                            meta.user_id, meta.system_save_data_id);
    return dir->GetDirectoryRelative(save_directory) != nullptr;
}

VirtualDir SaveDataFactory::GetSaveDataSpaceDirectory(SaveDataSpaceId space) const {
    return dir->GetDirectoryRelative(GetSaveDataSpaceIdPath(space));
}

std::string SaveDataFactory::GetSaveDataSpaceIdPath(SaveDataSpaceId space) {
    switch (space) {
    case SaveDataSpaceId::System:
        return "/system/";
    case SaveDataSpaceId::User:
    case SaveDataSpaceId::SdUser:
        return "/user/";
    case SaveDataSpaceId::Temporary:
        return "/temp/";
    default:
        ASSERT_MSG(false, "Unrecognized SaveDataSpaceId: {:02X}", static_cast<u8>(space));
        return "/unrecognized/"; ///< To prevent corruption when ignoring asserts.
    }
}

std::string SaveDataFactory::GetFullPath(ProgramId program_id, VirtualDir dir,
                                         SaveDataSpaceId space, SaveDataType type, u64 title_id,
                                         u128 user_id, u64 save_id) {
    // According to switchbrew, if a save is of type SaveData and the title id field is 0, it should
    // be interpreted as the title id of the current process.
    if (type == SaveDataType::Account || type == SaveDataType::Device) {
        if (title_id == 0) {
            title_id = program_id;
        }
    }

    // For compat with a future impl.
    if (std::string future_path =
            GetFutureSaveDataPath(space, type, title_id & ~(0xFFULL), user_id);
        !future_path.empty()) {
        // Check if this location exists, and prefer it over the old.
        if (const auto future_dir = dir->GetDirectoryRelative(future_path); future_dir != nullptr) {
            LOG_INFO(Service_FS, "Using save at new location for title {:016X}", title_id);
            return future_path;
        }
    }

    std::string out = GetSaveDataSpaceIdPath(space);

    switch (type) {
    case SaveDataType::System:
        return fmt::format("{}save/{:016X}/{:016X}{:016X}", out, save_id, user_id[1], user_id[0]);
    case SaveDataType::Account:
    case SaveDataType::Device:
        return fmt::format("{}save/{:016X}/{:016X}{:016X}/{:016X}", out, 0, user_id[1], user_id[0],
                           title_id);
    case SaveDataType::Temporary:
        return fmt::format("{}{:016X}/{:016X}{:016X}/{:016X}", out, 0, user_id[1], user_id[0],
                           title_id);
    case SaveDataType::Cache:
        return fmt::format("{}save/cache/{:016X}", out, title_id);
    case SaveDataType::Bcat:
        // BCAT delivery caches live in the system save space, where BISFactory::GetBCATDirectory
        // reads them, whichever space the request names.
        return fmt::format("/system/save/bcat/{:016X}", title_id);
    default:
        ASSERT_MSG(false, "Unrecognized SaveDataType: {:02X}", static_cast<u8>(type));
        return fmt::format("{}save/unknown_{:X}/{:016X}", out, static_cast<u8>(type), title_id);
    }
}

std::string SaveDataFactory::GetUserGameSaveDataRoot(u128 user_id, bool future) {
    if (future) {
        Common::UUID uuid;
        std::memcpy(uuid.uuid.data(), user_id.data(), sizeof(Common::UUID));
        return fmt::format("/user/save/account/{}", uuid.RawString());
    }
    return fmt::format("/user/save/{:016X}/{:016X}{:016X}", 0, user_id[1], user_id[0]);
}

SaveDataSize SaveDataFactory::ReadSaveDataSize(SaveDataType type, u64 title_id,
                                               u128 user_id) const {
    const auto path =
        GetFullPath(program_id, dir, SaveDataSpaceId::User, type, title_id, user_id, 0);
    const auto relative_dir = GetOrCreateDirectoryRelative(dir, path);

    const auto size_file = relative_dir->GetFile(GetSaveDataSizeFileName());
    if (size_file == nullptr || size_file->GetSize() < sizeof(SaveDataSize)) {
        return {0, 0};
    }

    SaveDataSize out;
    if (size_file->ReadObject(&out) != sizeof(SaveDataSize)) {
        return {0, 0};
    }

    return out;
}

void SaveDataFactory::WriteSaveDataSize(SaveDataType type, u64 title_id, u128 user_id,
                                        SaveDataSize new_value) const {
    const auto path =
        GetFullPath(program_id, dir, SaveDataSpaceId::User, type, title_id, user_id, 0);
    const auto relative_dir = GetOrCreateDirectoryRelative(dir, path);

    const auto size_file = relative_dir->CreateFile(GetSaveDataSizeFileName());
    if (size_file == nullptr) {
        return;
    }

    size_file->Resize(sizeof(SaveDataSize));
    size_file->WriteObject(new_value);
}

void SaveDataFactory::SetAutoCreate(bool state) {
    auto_create = state;
}

std::optional<u64> SaveDataFactory::ReadSystemSaveDataOwner(u64 save_id) const {
    const auto owners = dir->GetDirectoryRelative(SystemSaveDataOwnersPath);
    const auto file = owners != nullptr ? owners->GetFile(fmt::format("{:016X}", save_id)) : nullptr;
    u64 owner_id{};
    if (file == nullptr || file->ReadObject(&owner_id) != sizeof(owner_id)) {
        return std::nullopt;
    }
    return owner_id;
}

void SaveDataFactory::WriteSystemSaveDataOwner(u64 save_id, u64 owner_id) const {
    const auto owners = GetOrCreateDirectoryRelative(dir, SystemSaveDataOwnersPath);
    const auto file =
        owners != nullptr ? owners->CreateFile(fmt::format("{:016X}", save_id)) : nullptr;
    if (file == nullptr || !file->Resize(sizeof(owner_id))) {
        LOG_WARNING(Service_FS, "Could not record the owner of system save {:016X}", save_id);
        return;
    }
    file->WriteObject(owner_id);
}

bool SaveDataFactory::SystemSaveDataExists(u64 save_id) const {
    const auto save_root = dir->GetDirectoryRelative("/system/save");
    return save_root != nullptr &&
           save_root->GetSubdirectory(fmt::format("{:016X}", save_id)) != nullptr;
}

std::optional<SaveDataAttribute> SaveDataFactory::FindSystemSaveData(u64 save_id) const {
    const auto save_dir = dir->GetDirectoryRelative(fmt::format("/system/save/{:016X}", save_id));
    if (save_dir == nullptr) {
        return std::nullopt;
    }
    for (const auto& user_dir : save_dir->GetSubdirectories()) {
        const std::string name = user_dir->GetName();
        const auto high =
            name.size() == 32 ? ParseHex64(std::string_view{name}.substr(0, 16)) : std::nullopt;
        const auto low =
            name.size() == 32 ? ParseHex64(std::string_view{name}.substr(16)) : std::nullopt;
        if (high && low) {
            return SaveDataAttribute::Make(0, SaveDataType::System, UserId{*low, *high}, save_id);
        }
    }
    return std::nullopt;
}

std::optional<SaveDataExtraData> SaveDataFactory::ReadExtraData(
    SaveDataSpaceId space, const SaveDataAttribute& meta) const {
    const auto extra_dir = dir->GetDirectoryRelative(SaveDataExtraDataPath);
    const auto file = extra_dir != nullptr ? extra_dir->GetFile(GetExtraDataFileName(GetFullPath(
                                                 program_id, dir, space, meta.type, meta.program_id,
                                                 meta.user_id, meta.system_save_data_id)))
                                           : nullptr;
    SaveDataExtraData extra_data{};
    if (file == nullptr || file->ReadObject(&extra_data) != sizeof(extra_data)) {
        return std::nullopt;
    }
    return extra_data;
}

bool SaveDataFactory::WriteExtraData(SaveDataSpaceId space, const SaveDataAttribute& meta,
                                     const SaveDataExtraData& extra_data) const {
    const auto extra_dir = GetOrCreateDirectoryRelative(dir, SaveDataExtraDataPath);
    const auto name =
        GetExtraDataFileName(GetFullPath(program_id, dir, space, meta.type, meta.program_id,
                                         meta.user_id, meta.system_save_data_id));
    const auto file = extra_dir != nullptr ? extra_dir->CreateFile(name) : nullptr;
    if (file == nullptr || !file->Resize(sizeof(extra_data)) ||
        file->WriteObject(extra_data) != sizeof(extra_data)) {
        LOG_WARNING(Service_FS, "Could not write the extra data of save {:016X} (type {})",
                    meta.system_save_data_id, static_cast<u8>(meta.type));
        return false;
    }
    return true;
}

Result SaveDataFactory::DeleteSystemSaveData(u64 save_id) const {
    const auto save_root = dir->GetDirectoryRelative("/system/save");
    const auto name = fmt::format("{:016X}", save_id);
    if (save_root == nullptr || save_root->GetSubdirectory(name) == nullptr) {
        return ResultTargetNotFound;
    }
    if (!save_root->DeleteSubdirectoryRecursive(name)) {
        return ResultPermissionDenied;
    }
    if (const auto owners = dir->GetDirectoryRelative(SystemSaveDataOwnersPath);
        owners != nullptr) {
        owners->DeleteFile(name);
    }
    if (const auto extra_dir = dir->GetDirectoryRelative(SaveDataExtraDataPath);
        extra_dir != nullptr) {
        const auto prefix = fmt::format("system-save-{}-", name);
        for (const auto& file : extra_dir->GetFiles()) {
            if (file->GetName().starts_with(prefix)) {
                extra_dir->DeleteFile(file->GetName());
            }
        }
    }
    return ResultSuccess;
}

} // namespace FileSys

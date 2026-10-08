// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <ctime>
#include <random>

#include "core/core.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/errors.h"
#include "core/file_sys/patch_manager.h"
#include "core/hle/service/filesystem/save_data_controller.h"
#include "core/loader/loader.h"

namespace Service::FileSystem {

namespace {

// A default size for normal/journal save data size if application control metadata cannot be found.
// This should be large enough to satisfy even the most extreme requirements (~4.2GB)
constexpr u64 SufficientSaveDataSize = 0xF0000000;

// The extra-data store and the save paths know the system spaces as System.
FileSys::SaveDataSpaceId ToStoredSpace(FileSys::SaveDataSpaceId space) {
    switch (space) {
    case FileSys::SaveDataSpaceId::ProperSystem:
    case FileSys::SaveDataSpaceId::SafeMode:
        return FileSys::SaveDataSpaceId::System;
    default:
        return space;
    }
}

// The sizes fs gives a title's save when ns creates it: the NACP's account or device sizes.
// Zero for other saves or when the title's NACP is not found.
std::pair<s64, s64> GetTitleSaveDataSize(Core::System& system,
                                         const FileSys::SaveDataAttribute& attribute) {
    const bool account = attribute.type == FileSys::SaveDataType::Account;
    if ((!account && attribute.type != FileSys::SaveDataType::Device) ||
        attribute.program_id == 0) {
        return {0, 0};
    }
    const FileSys::PatchManager pm{attribute.program_id, system.GetFileSystemController(),
                                   system.GetContentProvider()};
    const auto nacp = pm.GetControlMetadata().first;
    if (nacp == nullptr) {
        return {0, 0};
    }
    const auto& raw = nacp->raw;
    return account ? std::pair<s64, s64>{static_cast<s64>(raw.user_account_save_data_size),
                                         static_cast<s64>(raw.user_account_save_data_journal_size)}
                   : std::pair<s64, s64>{static_cast<s64>(raw.device_save_data_size),
                                         static_cast<s64>(raw.device_save_data_journal_size)};
}

FileSys::SaveDataSize GetDefaultSaveDataSize(Core::System& system, u64 program_id) {
    const FileSys::PatchManager pm{program_id, system.GetFileSystemController(),
                                   system.GetContentProvider()};
    const auto metadata = pm.GetControlMetadata();
    const auto& nacp = metadata.first;

    if (nacp != nullptr) {
        return {nacp->GetDefaultNormalSaveSize(), nacp->GetDefaultJournalSaveSize()};
    }

    return {SufficientSaveDataSize, SufficientSaveDataSize};
}

} // namespace

SaveDataController::SaveDataController(Core::System& system_,
                                       std::shared_ptr<FileSys::SaveDataFactory> factory_)
    : system{system_}, factory{std::move(factory_)} {}
SaveDataController::~SaveDataController() = default;

Result SaveDataController::CreateSaveData(FileSys::VirtualDir* out_save_data,
                                          FileSys::SaveDataSpaceId space,
                                          const FileSys::SaveDataAttribute& attribute) {
    LOG_TRACE(Service_FS, "Creating Save Data for space_id={:01X}, save_struct={}", space,
              attribute.DebugInfo());

    auto save_data = factory->Create(space, attribute);
    if (save_data == nullptr) {
        return FileSys::ResultTargetNotFound;
    }

    *out_save_data = save_data;
    return ResultSuccess;
}

Result SaveDataController::OpenSaveData(FileSys::VirtualDir* out_save_data,
                                        FileSys::SaveDataSpaceId space,
                                        const FileSys::SaveDataAttribute& attribute) {
    auto save_data = factory->Open(space, attribute);
    if (save_data == nullptr) {
        return FileSys::ResultTargetNotFound;
    }

    *out_save_data = save_data;
    return ResultSuccess;
}

bool SaveDataController::SaveDataExists(FileSys::SaveDataSpaceId space,
                                        const FileSys::SaveDataAttribute& attribute) const {
    return factory->Exists(space, attribute);
}

Result SaveDataController::OpenSaveDataSpace(FileSys::VirtualDir* out_save_data_space,
                                             FileSys::SaveDataSpaceId space) {
    auto save_data_space = factory->GetSaveDataSpaceDirectory(space);
    if (save_data_space == nullptr) {
        return FileSys::ResultTargetNotFound;
    }

    *out_save_data_space = save_data_space;
    return ResultSuccess;
}

FileSys::SaveDataSize SaveDataController::ReadSaveDataSize(FileSys::SaveDataType type, u64 title_id,
                                                           u128 user_id) {
    const auto value = factory->ReadSaveDataSize(type, title_id, user_id);

    if (value.normal == 0 && value.journal == 0) {
        const auto size = GetDefaultSaveDataSize(system, title_id);
        factory->WriteSaveDataSize(type, title_id, user_id, size);
        return size;
    }

    return value;
}

void SaveDataController::WriteSaveDataSize(FileSys::SaveDataType type, u64 title_id, u128 user_id,
                                           FileSys::SaveDataSize new_value) {
    factory->WriteSaveDataSize(type, title_id, user_id, new_value);
}

void SaveDataController::SetAutoCreate(bool state) {
    factory->SetAutoCreate(state);
}

std::optional<u64> SaveDataController::ReadSystemSaveDataOwner(u64 save_id) const {
    return factory->ReadSystemSaveDataOwner(save_id);
}

void SaveDataController::WriteSystemSaveDataOwner(u64 save_id, u64 owner_id) {
    factory->WriteSystemSaveDataOwner(save_id, owner_id);
}

Result SaveDataController::DeleteSystemSaveData(u64 save_id) {
    return factory->DeleteSystemSaveData(save_id);
}

bool SaveDataController::SystemSaveDataExists(u64 save_id) const {
    return factory->SystemSaveDataExists(save_id);
}

Result SaveDataController::FindSaveDataById(FileSys::SaveDataAttribute* out_attribute,
                                            FileSys::SaveDataSpaceId space, u64 save_id) const {
    if (ToStoredSpace(space) != FileSys::SaveDataSpaceId::System) {
        return FileSys::ResultTargetNotFound;
    }
    const auto attribute = factory->FindSystemSaveData(save_id);
    if (!attribute) {
        return FileSys::ResultTargetNotFound;
    }
    *out_attribute = *attribute;
    return ResultSuccess;
}

Result SaveDataController::ReadSaveDataExtraData(
    FileSys::SaveDataExtraData* out_extra_data, FileSys::SaveDataSpaceId space,
    const FileSys::SaveDataAttribute& attribute) const {
    space = ToStoredSpace(space);
    if (!factory->Exists(space, attribute)) {
        return FileSys::ResultTargetNotFound;
    }
    FileSys::SaveDataExtraData extra_data{};
    if (const auto stored = factory->ReadExtraData(space, attribute)) {
        extra_data = *stored;
    } else {
        extra_data.attr = attribute;
        if (attribute.type == FileSys::SaveDataType::System) {
            extra_data.owner_id =
                factory->ReadSystemSaveDataOwner(attribute.system_save_data_id).value_or(0);
        } else {
            extra_data.owner_id = attribute.program_id;
        }
    }

    // A console's save always carries the sizes it was made with, and qlaunch extends a title's
    // save to its NACP sizes when they are larger, which fails with ExtensionSizeInvalid
    // (2002-6067) unless those sizes are whole MiB. Title saves made here without a size (by
    // Eden, or before extra data was kept) read the NACP sizes fs would have made them with.
    if (extra_data.available_size == 0 && extra_data.journal_size == 0) {
        const auto [size, journal_size] = GetTitleSaveDataSize(system, attribute);
        extra_data.available_size = size;
        extra_data.journal_size = journal_size;
    }
    *out_extra_data = extra_data;
    return ResultSuccess;
}

Result SaveDataController::WriteSaveDataExtraData(FileSys::SaveDataSpaceId space,
                                                  const FileSys::SaveDataAttribute& attribute,
                                                  const FileSys::SaveDataExtraData& extra_data) {
    space = ToStoredSpace(space);
    if (!factory->Exists(space, attribute)) {
        return FileSys::ResultTargetNotFound;
    }
    if (!factory->WriteExtraData(space, attribute, extra_data)) {
        return FileSys::ResultPermissionDenied;
    }
    return ResultSuccess;
}

void SaveDataController::InitializeSaveDataExtraData(FileSys::SaveDataSpaceId space,
                                                     const FileSys::SaveDataAttribute& attribute,
                                                     u64 owner_id, u32 flags, s64 size,
                                                     s64 journal_size,
                                                     FileSys::SaveDataFormatType format_type) {
    FileSys::SaveDataExtraData extra_data{};
    extra_data.attr = attribute;
    extra_data.owner_id = owner_id;
    extra_data.timestamp = static_cast<s64>(std::time(nullptr));
    extra_data.flags = flags;
    extra_data.format_type = format_type;
    extra_data.available_size = size;
    extra_data.journal_size = journal_size;
    std::random_device random;
    extra_data.commit_id = static_cast<s64>((u64{random()} << 32) | random());
    factory->WriteExtraData(ToStoredSpace(space), attribute, extra_data);
}

void SaveDataController::RecordSaveDataCommit(FileSys::SaveDataSpaceId space,
                                              const FileSys::SaveDataAttribute& attribute) {
    FileSys::SaveDataExtraData extra_data{};
    if (ReadSaveDataExtraData(&extra_data, space, attribute).IsError()) {
        return;
    }
    extra_data.timestamp = static_cast<s64>(std::time(nullptr));
    std::random_device random;
    extra_data.commit_id = static_cast<s64>((u64{random()} << 32) | random());
    factory->WriteExtraData(ToStoredSpace(space), attribute, extra_data);
}

} // namespace Service::FileSystem

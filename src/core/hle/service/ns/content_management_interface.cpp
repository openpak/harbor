// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/common_funcs.h"
#include "core/core.h"
#include "core/file_sys/common_funcs.h"
#include "core/file_sys/registered_cache.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/hle/service/ns/content_management_interface.h"
#include "core/hle/service/ns/ns_types.h"

namespace Service::NS {

IContentManagementInterface::IContentManagementInterface(Core::System& system_)
    : ServiceFramework{system_, "IContentManagementInterface"} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {11, D<&IContentManagementInterface::CalculateApplicationOccupiedSize>, "CalculateApplicationOccupiedSize"},
        {43, D<&IContentManagementInterface::CheckSdCardMountStatus>, "CheckSdCardMountStatus"},
        {47, D<&IContentManagementInterface::GetTotalSpaceSize>, "GetTotalSpaceSize"},
        {48, D<&IContentManagementInterface::GetFreeSpaceSize>, "GetFreeSpaceSize"},
        {71, D<&IContentManagementInterface::GetUnknown71>, "Unknown71"},
        {600, nullptr, "CountApplicationContentMeta"},
        {601, nullptr, "ListApplicationContentMetaStatus"},
        {605, nullptr, "ListApplicationContentMetaStatusWithRightsCheck"},
        {607, D<&IContentManagementInterface::IsAnyApplicationRunning>, "IsAnyApplicationRunning"},
        {608, nullptr, "Unknown608"}, //21.0.0+
    };
    // clang-format on

    RegisterHandlers(functions);
}

IContentManagementInterface::~IContentManagementInterface() = default;

Result IContentManagementInterface::CalculateApplicationOccupiedSize(
    Out<ApplicationOccupiedSize> out_size, u64 application_id) {
    // What "Data Management" shows for a title: the sizes of the content the user's folders (and
    // the NAND/SD card) hold for it, base, update and DLC, as the HOME menu counts them.
    const auto& content = system.GetContentProviderUnion();
    const auto update_id = FileSys::GetUpdateTitleID(application_id);

    const auto size_of = [&content](u64 title_id) {
        u64 total = 0;
        for (u8 type = 0; type < static_cast<u8>(FileSys::ContentRecordType::Count); ++type) {
            if (const auto file =
                    content.GetEntryRaw(title_id, static_cast<FileSys::ContentRecordType>(type))) {
                total += file->GetSize();
            }
        }
        return total;
    };

    u64 aoc_size = 0;
    for (const auto& entry : content.ListEntriesFilter(FileSys::TitleType::AOC, std::nullopt, std::nullopt)) {
        if (FileSys::GetBaseTitleID(entry.title_id) == FileSys::GetBaseTitleID(application_id)) {
            if (const auto file = content.GetEntryRaw(entry.title_id, entry.type)) {
                aoc_size += file->GetSize();
            }
        }
    }

    // Folders and the SD card count as the SD card, the NAND as the user storage.
    auto storage_id = FileSys::StorageId::SdCard;
    if (const auto slot = content.GetSlotForEntry(application_id, FileSys::ContentRecordType::Program);
        slot && (*slot == FileSys::ContentProviderUnionSlot::UserNAND ||
                 *slot == FileSys::ContentProviderUnionSlot::SysNAND)) {
        storage_id = FileSys::StorageId::NandUser;
    }

    out_size->entities = {};
    out_size->entities[0] = {
        .storage_id = storage_id,
        .app_size = size_of(application_id),
        .patch_size = size_of(update_id),
        .aoc_size = aoc_size,
    };
    LOG_DEBUG(Service_NS, "called, application_id={:016X}, app={:#x} patch={:#x} aoc={:#x}",
              application_id, out_size->entities[0].app_size, out_size->entities[0].patch_size,
              out_size->entities[0].aoc_size);

    R_SUCCEED();
}

Result IContentManagementInterface::CheckSdCardMountStatus() {
    LOG_WARNING(Service_NS, "(STUBBED) called");
    R_SUCCEED();
}

Result IContentManagementInterface::GetTotalSpaceSize(Out<s64> out_total_space_size,
                                                      FileSys::StorageId storage_id) {
    LOG_INFO(Service_NS, "(STUBBED) called, storage_id={}", storage_id);
    *out_total_space_size = system.GetFileSystemController().GetTotalSpaceSize(storage_id);
    R_SUCCEED();
}

Result IContentManagementInterface::GetFreeSpaceSize(Out<s64> out_free_space_size,
                                                     FileSys::StorageId storage_id) {
    LOG_INFO(Service_NS, "(STUBBED) called, storage_id={}", storage_id);
    *out_free_space_size = system.GetFileSystemController().GetFreeSpaceSize(storage_id);
    R_SUCCEED();
}

Result IContentManagementInterface::IsAnyApplicationRunning(Out<bool> out_is_running) {
    // Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base): a title, not the HOME menu
    // or another system program, is running.
    const auto* const process = system.ApplicationProcess();
    *out_is_running = process != nullptr && process->GetProgramId() >= 0x0100000000001FFFull;
    LOG_DEBUG(Service_NS, "called, is_running={}", *out_is_running);
    R_SUCCEED();
}

Result IContentManagementInterface::GetUnknown71(Out<u64> out_value_a, Out<u64> out_value_b,
                                                 u8 flag) {
    LOG_INFO(Service_NS, "(STUBBED) called, flag={:02X}", flag);
    *out_value_a = 0;
    *out_value_b = 0;
    R_SUCCEED();
}

} // namespace Service::NS

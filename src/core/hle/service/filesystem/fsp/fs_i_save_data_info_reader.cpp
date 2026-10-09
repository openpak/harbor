// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include <string>
#include <string_view>

#include "common/hex_util.h"
#include "core/file_sys/savedata_factory.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/filesystem/fsp/fs_i_save_data_info_reader.h"
#include "core/hle/service/filesystem/fsp/save_data_size.h"
#include "core/hle/service/filesystem/save_data_controller.h"

namespace Service::FileSystem {

ISaveDataInfoReader::ISaveDataInfoReader(Core::System& system_,
                                         std::shared_ptr<SaveDataController> save_data_controller_,
                                         FileSys::SaveDataSpaceId space)
    : ServiceFramework{system_, "ISaveDataInfoReader"}, save_data_controller{
                                                            save_data_controller_} {
    static const FunctionInfo functions[] = {
        {0, D<&ISaveDataInfoReader::ReadSaveDataInfo>, "ReadSaveDataInfo"},
    };
    RegisterHandlers(functions);

    if (save_data_controller != nullptr) {
        info = FindAllSaves(*save_data_controller, space);
    }
}

ISaveDataInfoReader::~ISaveDataInfoReader() = default;

static u64 stoull_be(std::string_view str) {
    if (str.size() != 16) {
        return 0;
    }

    const auto bytes = Common::HexStringToArray<0x8>(str);
    u64 out{};
    std::memcpy(&out, bytes.data(), sizeof(u64));

    return Common::swap64(out);
}

// fs gives every save that has no static id an id of its own in its index. Saves here have no
// index, so the id is derived from the save's location: stable across restarts, below the
// system save range (top bit clear) and never 0.
static u64 MakeSaveDataId(FileSys::SaveDataSpaceId space, std::string_view user_id,
                          std::string_view title_id) {
    u64 hash = 0xCBF29CE484222325ULL;
    const auto mix = [&hash](std::string_view text) {
        for (const char c : text) {
            hash = (hash ^ static_cast<u8>(c)) * 0x100000001B3ULL;
        }
        hash = (hash ^ u8{'/'}) * 0x100000001B3ULL;
    };
    mix(std::to_string(static_cast<u32>(space)));
    mix(user_id);
    mix(title_id);
    const u64 id = hash & 0x7FFFFFFFFFFFFFFFULL;
    return id != 0 ? id : 1;
}

Result ISaveDataInfoReader::ReadSaveDataInfo(
    Out<u64> out_count, OutArray<SaveDataInfo, BufferAttr_HipcMapAlias> out_entries) {
    LOG_DEBUG(Service_FS, "called");

    // Calculate how many entries we can fit in the output buffer
    const u64 count_entries = out_entries.size();

    // Cap at total number of entries.
    const u64 actual_entries = (std::min)(count_entries, info.size() - next_entry_index);

    // Determine data start and end
    const auto* begin = reinterpret_cast<u8*>(info.data() + next_entry_index);
    const auto* end = reinterpret_cast<u8*>(info.data() + next_entry_index + actual_entries);
    const auto range_size = static_cast<std::size_t>(std::distance(begin, end));

    next_entry_index += actual_entries;

    // Write the data to memory
    std::memcpy(out_entries.data(), begin, range_size);
    *out_count = actual_entries;

    R_SUCCEED();
}

std::vector<ISaveDataInfoReader::SaveDataInfo> ISaveDataInfoReader::FindAllSaves(
    SaveDataController& save_data_controller, FileSys::SaveDataSpaceId space) {
    std::vector<SaveDataInfo> info;
    FileSys::VirtualDir save_root{};
    const auto result = save_data_controller.OpenSaveDataSpace(&save_root, space);

    if (result != ResultSuccess || save_root == nullptr) {
        LOG_ERROR(Service_FS, "The save root for the space_id={:02X} was invalid!", space);
        return info;
    }

    for (const auto& type : save_root->GetSubdirectories()) {
        if (type->GetName() == "save") {
            FindNormalSaves(info, space, type);
        } else if (space == FileSys::SaveDataSpaceId::Temporary) {
            FindTemporaryStorageSaves(info, space, type);
        }
    }

    // SaveDataInfo.size is the save image's total size for the sizes the save was made with, as
    // fs stores it at creation (fs-save-size.md), not what the files take on the host. A save
    // whose sizes are unknown keeps the host size.
    for (auto& entry : info) {
        if (entry.type == FileSys::SaveDataType::Temporary) {
            continue;
        }
        const bool system = entry.type == FileSys::SaveDataType::System;
        FileSys::UserId user{};
        std::memcpy(user.data(), entry.user_id.data(), sizeof(user));
        const auto attribute = FileSys::SaveDataAttribute::Make(
            system ? 0 : entry.title_id, entry.type, user,
            system ? entry.save_id : FileSys::InvalidSystemSaveDataId, 0,
            FileSys::SaveDataRank::Primary);
        FileSys::SaveDataExtraData extra{};
        if (save_data_controller.ReadSaveDataExtraData(&extra, space, attribute).IsError()) {
            continue;
        }
        const auto size = QuerySaveDataTotalSize(extra.available_size, extra.journal_size);
        if (size) {
            entry.save_image_size = static_cast<u64>(*size);
        }
    }
    return info;
}

void ISaveDataInfoReader::FindNormalSaves(std::vector<SaveDataInfo>& info,
                                          FileSys::SaveDataSpaceId space,
                                          const FileSys::VirtualDir& type) {
    for (const auto& save_id : type->GetSubdirectories()) {
        // Emulator bookkeeping (/system/save/.harbor) is not a save.
        if (save_id->GetName().starts_with('.')) {
            continue;
        }
        // Eden's newer layout for user saves (SaveDataFactory::GetFullPath prefers it when it
        // exists): account/<user>/<title>/0 and device/<title>/0.
        if (space == FileSys::SaveDataSpaceId::User &&
            (save_id->GetName() == "account" || save_id->GetName() == "device")) {
            FindNewLayoutSaves(info, space, save_id);
            continue;
        }
        for (const auto& user_id : save_id->GetSubdirectories()) {
            // Skip non user id subdirectories
            if (user_id->GetName().size() != 0x20) {
                continue;
            }

            const auto save_id_numeric = stoull_be(save_id->GetName());
            auto user_id_numeric = Common::HexStringToArray<0x10>(user_id->GetName());
            std::reverse(user_id_numeric.begin(), user_id_numeric.end());

            if (save_id_numeric != 0) {
                // System Save Data. fs indexes a system save under its static id.
                info.emplace_back(SaveDataInfo{
                    save_id_numeric,
                    space,
                    FileSys::SaveDataType::System,
                    {},
                    user_id_numeric,
                    save_id_numeric,
                    0,
                    user_id->GetSize(),
                    {},
                    {},
                });

                continue;
            }

            for (const auto& title_id : user_id->GetSubdirectories()) {
                const auto device = std::all_of(user_id_numeric.begin(), user_id_numeric.end(),
                                                [](u8 val) { return val == 0; });
                info.emplace_back(SaveDataInfo{
                    MakeSaveDataId(space, user_id->GetName(), title_id->GetName()),
                    space,
                    device ? FileSys::SaveDataType::Device : FileSys::SaveDataType::Account,
                    {},
                    user_id_numeric,
                    save_id_numeric,
                    stoull_be(title_id->GetName()),
                    title_id->GetSize(),
                    {},
                    {},
                });
            }
        }
    }
}

void ISaveDataInfoReader::FindNewLayoutSaves(std::vector<SaveDataInfo>& info,
                                             FileSys::SaveDataSpaceId space,
                                             const FileSys::VirtualDir& layout) {
    const bool device = layout->GetName() == "device";
    const auto add_title_saves = [&](const FileSys::VirtualDir& titles, std::string_view user_name,
                                     const std::array<u8, 0x10>& user_id) {
        for (const auto& title_id : titles->GetSubdirectories()) {
            if (title_id->GetName().size() != 0x10 || title_id->GetSubdirectory("0") == nullptr) {
                continue;
            }
            // The layout tag keeps these ids apart from a save of the same title and user in
            // the older layout.
            info.emplace_back(SaveDataInfo{
                MakeSaveDataId(space, fmt::format("{}/{}", layout->GetName(), user_name),
                               title_id->GetName()),
                space,
                device ? FileSys::SaveDataType::Device : FileSys::SaveDataType::Account,
                {},
                user_id,
                0,
                stoull_be(title_id->GetName()),
                title_id->GetSize(),
                {},
                {},
            });
        }
    };

    if (device) {
        add_title_saves(layout, {}, {});
        return;
    }
    for (const auto& user_dir : layout->GetSubdirectories()) {
        // The user directory is Common::UUID::RawString: the id's bytes in memory order.
        if (user_dir->GetName().size() != 0x20) {
            continue;
        }
        add_title_saves(user_dir, user_dir->GetName(),
                        Common::HexStringToArray<0x10>(user_dir->GetName()));
    }
}

void ISaveDataInfoReader::FindTemporaryStorageSaves(std::vector<SaveDataInfo>& info,
                                                    FileSys::SaveDataSpaceId space,
                                                    const FileSys::VirtualDir& type) {
    for (const auto& user_id : type->GetSubdirectories()) {
        // Skip non user id subdirectories
        if (user_id->GetName().size() != 0x20) {
            continue;
        }
        for (const auto& title_id : user_id->GetSubdirectories()) {
            if (!title_id->GetFiles().empty() || !title_id->GetSubdirectories().empty()) {
                auto user_id_numeric = Common::HexStringToArray<0x10>(user_id->GetName());
                std::reverse(user_id_numeric.begin(), user_id_numeric.end());

                info.emplace_back(SaveDataInfo{
                    MakeSaveDataId(space, user_id->GetName(), title_id->GetName()),
                    space,
                    FileSys::SaveDataType::Temporary,
                    {},
                    user_id_numeric,
                    stoull_be(type->GetName()),
                    stoull_be(title_id->GetName()),
                    title_id->GetSize(),
                    {},
                    {},
                });
            }
        }
    }
}

} // namespace Service::FileSystem

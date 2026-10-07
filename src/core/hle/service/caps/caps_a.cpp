// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>

#include "common/logging.h"
#include "core/hle/kernel/k_readable_event.h"
#include "core/hle/service/caps/caps_a.h"
#include "core/hle/service/caps/caps_manager.h"
#include "core/hle/service/caps/caps_result.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/ipc_helpers.h"

namespace Service::Capture {

IAlbumAccessorService::IAlbumAccessorService(Core::System& system_,
                                             std::shared_ptr<AlbumManager> album_manager)
    : ServiceFramework{system_, "caps:a"}, manager{album_manager} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, C<&IAlbumAccessorService::GetAlbumFileCount>, "GetAlbumFileCount"},
        {1, C<&IAlbumAccessorService::GetAlbumFileList>, "GetAlbumFileList"},
        {2, nullptr, "LoadAlbumFile"},
        {3, C<&IAlbumAccessorService::DeleteAlbumFile>, "DeleteAlbumFile"},
        {4, nullptr, "StorageCopyAlbumFile"},
        {5, C<&IAlbumAccessorService::IsAlbumMounted>, "IsAlbumMounted"},
        {6, C<&IAlbumAccessorService::GetAlbumUsage>, "GetAlbumUsage"},
        {7, nullptr, "GetAlbumFileSize"},
        {8, nullptr, "LoadAlbumFileThumbnail"},
        {9, nullptr, "LoadAlbumScreenShotImage"},
        {10, nullptr, "LoadAlbumScreenShotThumbnailImage"},
        {11, nullptr, "GetAlbumEntryFromApplicationAlbumEntry"},
        {12, nullptr, "LoadAlbumScreenShotImageEx"},
        {13, nullptr, "LoadAlbumScreenShotThumbnailImageEx"},
        {14, nullptr, "LoadAlbumScreenShotImageEx0"},
        {15, C<&IAlbumAccessorService::GetAlbumUsage3>, "GetAlbumUsage3"},
        {16, C<&IAlbumAccessorService::GetAlbumMountResult>, "GetAlbumMountResult"},
        {17, C<&IAlbumAccessorService::GetAlbumUsage16>, "GetAlbumUsage16"},
        {18, C<&IAlbumAccessorService::Unknown18>, "Unknown18"},
        {19, nullptr, "Unknown19"},
        {100, C<&IAlbumAccessorService::GetAlbumFileCountEx0>, "GetAlbumFileCountEx0"},
        {101, C<&IAlbumAccessorService::GetAlbumFileListEx0>, "GetAlbumFileListEx0"},
        {110, C<&IAlbumAccessorService::Unknown110>, "GetAlbumFileListEx1"},
        {120, C<&IAlbumAccessorService::Unknown120>, "GetAlbumFileListEx2"},
        {140, C<&IAlbumAccessorService::GetAlbumFileCreatedEvent>, "GetAlbumFileCreatedEvent"},
        {202, nullptr, "SaveEditedScreenShot"},
        {301, C<&IAlbumAccessorService::GetLastOverlayScreenShotThumbnail>, "GetLastOverlayScreenShotThumbnail"},
        {302, C<&IAlbumAccessorService::GetLastOverlayMovieThumbnail>, "GetLastOverlayMovieThumbnail"},
        {401,  C<&IAlbumAccessorService::GetAutoSavingStorage>, "GetAutoSavingStorage"},
        {501, C<&IAlbumAccessorService::GetRequiredStorageSpaceSizeToCopyAll>, "GetRequiredStorageSpaceSizeToCopyAll"},
        {1001, nullptr, "LoadAlbumScreenShotThumbnailImageEx0"},
        {1002, C<&IAlbumAccessorService::LoadAlbumScreenShotImageEx1>, "LoadAlbumScreenShotImageEx1"},
        {1003, C<&IAlbumAccessorService::LoadAlbumScreenShotThumbnailImageEx1>, "LoadAlbumScreenShotThumbnailImageEx1"},
        {8001, nullptr, "ForceAlbumUnmounted"},
        {8002, nullptr, "ResetAlbumMountStatus"},
        {8011, nullptr, "RefreshAlbumCache"},
        {8012, nullptr, "GetAlbumCache"},
        {8013, nullptr, "GetAlbumCacheEx"},
        {8021, nullptr, "GetAlbumEntryFromApplicationAlbumEntryAruid"},
        {10011, nullptr, "SetInternalErrorConversionEnabled"},
        {50000, nullptr, "LoadMakerNoteInfoForDebug"},
        {50011, C<&IAlbumAccessorService::GetAlbumAccessResultForDebug>, "GetAlbumAccessResultForDebug"},
        {50012, C<&IAlbumAccessorService::SetAlbumAccessResultForDebug>, "SetAlbumAccessResultForDebug"},
        {60002, nullptr, "OpenAccessorSession"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

IAlbumAccessorService::~IAlbumAccessorService() = default;

Result IAlbumAccessorService::GetAlbumFileCount(Out<u64> out_count, AlbumStorage storage) {
    const Result result = manager->GetAlbumFileCount(*out_count, storage, 0);
    LOG_DEBUG(Service_Capture, "called, storage={}: {}", storage, *out_count);
    R_RETURN(TranslateResult(result));
}

Result IAlbumAccessorService::GetAlbumFileList(
    Out<u64> out_count, AlbumStorage storage,
    OutArray<AlbumEntry, BufferAttr_HipcMapAlias> out_entries) {
    LOG_INFO(Service_Capture, "called, storage={}", storage);

    const Result result = manager->GetAlbumFileList(out_entries, *out_count, storage, 0);
    R_RETURN(TranslateResult(result));
}

Result IAlbumAccessorService::DeleteAlbumFile(AlbumFileId file_id) {
    LOG_INFO(Service_Capture, "called, application_id={:#0x}, storage={}, type={}",
             file_id.application_id, file_id.storage, file_id.type);

    const Result result = manager->DeleteAlbumFile(file_id);
    R_RETURN(TranslateResult(result));
}

Result IAlbumAccessorService::IsAlbumMounted(Out<bool> out_is_mounted, AlbumStorage storage) {
    LOG_INFO(Service_Capture, "called, storage={}", storage);

    const Result result = manager->IsAlbumMounted(storage);
    if (result.IsSuccess() && !manager->IsStorageAvailable(storage)) {
        // A storage the console does not present is "not mounted", not an error.
        *out_is_mounted = false;
        R_SUCCEED();
    }
    *out_is_mounted = result.IsSuccess();
    R_RETURN(TranslateResult(result));
}

Result IAlbumAccessorService::GetAlbumUsage(Out<std::array<AlbumUsage, 2>> out_usage,
                                            AlbumStorage storage) {
    // unknown contract: the layout is libnx's (two 0x18-byte usages); which content type each
    // one is, is a guess (screenshot, then movie).
    LOG_WARNING(Service_Capture, "unknown contract: caps:a 6 GetAlbumUsage, storage={}", storage);
    *out_usage = {};
    R_RETURN(TranslateResult(manager->GetAlbumUsage(*out_usage, storage)));
}

Result IAlbumAccessorService::GetAlbumUsage3(Out<std::array<AlbumUsage, 3>> out_usage,
                                             AlbumStorage storage) {
    LOG_WARNING(Service_Capture, "unknown contract: caps:a 15 GetAlbumUsage3, storage={}", storage);
    *out_usage = {};
    R_RETURN(TranslateResult(manager->GetAlbumUsage(*out_usage, storage)));
}

Result IAlbumAccessorService::GetAlbumMountResult(AlbumStorage storage) {
    LOG_DEBUG(Service_Capture, "called, storage={}", storage);
    R_RETURN(TranslateResult(manager->GetAlbumMountResult(storage)));
}

Result IAlbumAccessorService::GetAlbumUsage16(AlbumStorage storage, u8 flags,
                                              OutBuffer<BufferAttr_HipcAutoSelect> out_buffer) {
    // unknown contract: the buffer's element layout is not established; it is answered empty.
    LOG_WARNING(Service_Capture,
                "unknown contract: caps:a 17 GetAlbumUsage16, storage={}, flags={}, buffer {:#x} "
                "bytes; zero-filled",
                storage, flags, out_buffer.size());
    std::ranges::fill(out_buffer, u8{0});
    R_SUCCEED();
}

Result IAlbumAccessorService::GetAlbumFileCountEx0(Out<u64> out_count, AlbumStorage storage,
                                                   u8 flags) {
    const Result result = manager->GetAlbumFileCount(*out_count, storage, flags);
    LOG_DEBUG(Service_Capture, "called, storage={}, flags={}: {}", storage, flags, *out_count);
    R_RETURN(TranslateResult(result));
}

Result IAlbumAccessorService::Unknown110(Out<u64> out_count,
                                         OutBuffer<BufferAttr_HipcMapAlias> out_buffer) {
    // unknown contract: the inputs are undocumented; the Ryujinx Harbor answers an empty list.
    LOG_WARNING(Service_Capture,
                "unknown contract: caps:a 110, out buffer {:#x} bytes; returning 0 entries",
                out_buffer.size());
    std::ranges::fill(out_buffer, u8{0});
    *out_count = 0;
    R_SUCCEED();
}

Result IAlbumAccessorService::Unknown120(Out<u64> out_count,
                                         OutBuffer<BufferAttr_HipcMapAlias> out_buffer) {
    LOG_WARNING(Service_Capture,
                "unknown contract: caps:a 120, out buffer {:#x} bytes; returning 0 entries",
                out_buffer.size());
    std::ranges::fill(out_buffer, u8{0});
    *out_count = 0;
    R_SUCCEED();
}

Result IAlbumAccessorService::GetAlbumFileCreatedEvent(
    OutCopyHandle<Kernel::KReadableEvent> out_event) {
    LOG_DEBUG(Service_Capture, "called");
    *out_event = manager->GetAlbumFileCreatedEvent().GetHandle();
    R_SUCCEED();
}

Result IAlbumAccessorService::GetLastOverlayScreenShotThumbnail(
    Out<AlbumFileId> out_file_id, Out<u64> out_size,
    OutBuffer<BufferAttr_HipcMapAlias> out_thumbnail) {
    // The overlay's "Captured" pop-up shows this thumbnail only when the id is the one ovln
    // announced; before any capture the answer is an empty id and size 0.
    std::ranges::fill(out_thumbnail, u8{0});
    *out_file_id = {};
    *out_size = 0;

    std::vector<u8> thumbnail;
    if (manager->GetLastOverlayScreenShot(*out_file_id, thumbnail)) {
        *out_size = std::min<u64>(thumbnail.size(), out_thumbnail.size());
        std::memcpy(out_thumbnail.data(), thumbnail.data(), *out_size);
    }
    LOG_DEBUG(Service_Capture, "called: {:#x} bytes", *out_size);
    R_SUCCEED();
}

Result IAlbumAccessorService::GetLastOverlayMovieThumbnail(
    Out<AlbumFileId> out_file_id, Out<u64> out_size,
    OutBuffer<BufferAttr_HipcMapAlias> out_thumbnail) {
    // No video is ever recorded here.
    std::ranges::fill(out_thumbnail, u8{0});
    *out_file_id = {};
    *out_size = 0;
    R_SUCCEED();
}

Result IAlbumAccessorService::GetRequiredStorageSpaceSizeToCopyAll(Out<u64> out_size) {
    // Nothing is copied between the storages (only the NAND album is presented).
    *out_size = 0;
    R_SUCCEED();
}

Result IAlbumAccessorService::SetAlbumAccessResultForDebug(Result in_result) {
    LOG_DEBUG(Service_Capture, "called, result={:#x}: ignored", in_result.raw);
    R_SUCCEED();
}

Result IAlbumAccessorService::Unknown18(
    Out<u32> out_buffer_size,
    OutArray<u8, BufferAttr_HipcMapAlias | BufferAttr_HipcMapTransferAllowsNonSecure> out_buffer) {
    LOG_WARNING(Service_Capture, "(STUBBED) called");
    *out_buffer_size = 0;
    R_SUCCEED();
}

Result IAlbumAccessorService::GetAlbumFileListEx0(
    Out<u64> out_entries_size, AlbumStorage storage, u8 flags,
    OutArray<AlbumEntry, BufferAttr_HipcMapAlias> out_entries) {
    LOG_INFO(Service_Capture, "called, storage={}, flags={}", storage, flags);

    const Result result = manager->GetAlbumFileList(out_entries, *out_entries_size, storage, flags);
    R_RETURN(TranslateResult(result));
}

Result IAlbumAccessorService::GetAutoSavingStorage(Out<bool> out_is_autosaving) {
    LOG_WARNING(Service_Capture, "(STUBBED) called");

    const Result result = manager->GetAutoSavingStorage(*out_is_autosaving);
    R_RETURN(TranslateResult(result));
}

Result IAlbumAccessorService::LoadAlbumScreenShotImageEx1(
    const AlbumFileId& file_id, const ScreenShotDecodeOption& decoder_options,
    OutLargeData<LoadAlbumScreenShotImageOutput, BufferAttr_HipcMapAlias> out_image_output,
    OutArray<u8, BufferAttr_HipcMapAlias | BufferAttr_HipcMapTransferAllowsNonSecure> out_image,
    OutArray<u8, BufferAttr_HipcMapAlias> out_buffer) {
    LOG_INFO(Service_Capture, "called, application_id={:#0x}, storage={}, type={}, flags={}",
             file_id.application_id, file_id.storage, file_id.type, decoder_options.flags);

    const Result result =
        manager->LoadAlbumScreenShotImage(*out_image_output, out_image, file_id, decoder_options);
    R_RETURN(TranslateResult(result));
}

Result IAlbumAccessorService::LoadAlbumScreenShotThumbnailImageEx1(
    const AlbumFileId& file_id, const ScreenShotDecodeOption& decoder_options,
    OutLargeData<LoadAlbumScreenShotImageOutput, BufferAttr_HipcMapAlias> out_image_output,
    OutArray<u8, BufferAttr_HipcMapAlias | BufferAttr_HipcMapTransferAllowsNonSecure> out_image,
    OutArray<u8, BufferAttr_HipcMapAlias> out_buffer) {
    LOG_INFO(Service_Capture, "called, application_id={:#0x}, storage={}, type={}, flags={}",
             file_id.application_id, file_id.storage, file_id.type, decoder_options.flags);

    const Result result = manager->LoadAlbumScreenShotThumbnail(*out_image_output, out_image,
                                                                file_id, decoder_options);
    R_RETURN(TranslateResult(result));
}

Result IAlbumAccessorService::TranslateResult(Result in_result) {
    if (in_result.IsSuccess()) {
        return in_result;
    }

    if ((in_result.raw & 0x3801ff) == ResultUnknown1024.raw) {
        if (in_result.GetDescription() - 0x514 < 100) {
            return ResultInvalidFileData;
        }
        if (in_result.GetDescription() - 0x5dc < 100) {
            return ResultInvalidFileData;
        }

        if (in_result.GetDescription() - 0x578 < 100) {
            if (in_result == ResultFileCountLimit) {
                return ResultUnknown22;
            }
            return ResultUnknown25;
        }

        if (in_result.raw < ResultUnknown1801.raw) {
            if (in_result == ResultUnknown1202) {
                return ResultUnknown810;
            }
            if (in_result == ResultUnknown1203) {
                return ResultUnknown810;
            }
            if (in_result == ResultUnknown1701) {
                return ResultUnknown5;
            }
        } else if (in_result.raw < ResultUnknown1803.raw) {
            if (in_result == ResultUnknown1801) {
                return ResultUnknown5;
            }
            if (in_result == ResultUnknown1802) {
                return ResultUnknown6;
            }
        } else {
            if (in_result == ResultUnknown1803) {
                return ResultUnknown7;
            }
            if (in_result == ResultUnknown1804) {
                return ResultOutOfRange;
            }
        }
        return ResultUnknown1024;
    }

    if (in_result.GetModule() == ErrorModule::FS) {
        if ((in_result.GetDescription() >> 0xc < 0x7d) ||
            (in_result.GetDescription() - 1000 < 2000) ||
            (((in_result.GetDescription() - 3000) >> 3) < 0x271)) {
            // TODO: Translate FS error
            return in_result;
        }
    }

    return in_result;
}


Result IAlbumAccessorService::GetAlbumAccessResultForDebug(Out<Result> out_result) {
    LOG_WARNING(Service_Capture, "(STUBBED) called");
    *out_result = ResultSuccess;
    R_SUCCEED();
}

} // namespace Service::Capture

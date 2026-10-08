// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/hle/service/caps/caps_types.h"
#include "core/hle/service/cmif_types.h"
#include "core/hle/service/service.h"

namespace Core {
class System;
}

namespace Kernel {
class KReadableEvent;
}

namespace Service::Capture {
class AlbumManager;

class IAlbumAccessorService final : public ServiceFramework<IAlbumAccessorService> {
public:
    explicit IAlbumAccessorService(Core::System& system_,
                                   std::shared_ptr<AlbumManager> album_manager);
    ~IAlbumAccessorService() override;

private:
    Result GetAlbumFileCount(Out<u64> out_count, AlbumStorage storage);

    Result GetAlbumFileList(Out<u64> out_count, AlbumStorage storage,
                            OutArray<AlbumEntry, BufferAttr_HipcMapAlias> out_entries);

    Result DeleteAlbumFile(AlbumFileId file_id);

    Result IsAlbumMounted(Out<bool> out_is_mounted, AlbumStorage storage);

    Result GetAlbumUsage(Out<std::array<AlbumUsage, 2>> out_usage, AlbumStorage storage);

    Result GetAlbumUsage3(Out<std::array<AlbumUsage, 3>> out_usage, AlbumStorage storage);

    Result GetAlbumMountResult(AlbumStorage storage);

    Result GetAlbumUsage16(AlbumStorage storage, u8 flags,
                           OutBuffer<BufferAttr_HipcAutoSelect> out_buffer);

    Result GetAlbumFileCountEx0(Out<u64> out_count, AlbumStorage storage, u8 flags);

    Result Unknown110(Out<u64> out_count, OutBuffer<BufferAttr_HipcMapAlias> out_buffer);

    Result Unknown120(Out<u64> out_count, OutBuffer<BufferAttr_HipcMapAlias> out_buffer);

    Result GetAlbumFileCreatedEvent(OutCopyHandle<Kernel::KReadableEvent> out_event);

    Result GetLastOverlayScreenShotThumbnail(Out<AlbumFileId> out_file_id, Out<u64> out_size,
                                             OutBuffer<BufferAttr_HipcMapAlias> out_thumbnail);

    Result GetLastOverlayMovieThumbnail(Out<AlbumFileId> out_file_id, Out<u64> out_size,
                                        OutBuffer<BufferAttr_HipcMapAlias> out_thumbnail);

    Result GetRequiredStorageSpaceSizeToCopyAll(Out<u64> out_size);

    Result SetAlbumAccessResultForDebug(Result in_result);

    Result Unknown18(
        Out<u32> out_buffer_size,
        OutArray<u8, BufferAttr_HipcMapAlias | BufferAttr_HipcMapTransferAllowsNonSecure>
            out_buffer);

    Result GetAlbumFileListEx0(Out<u64> out_entries_size, AlbumStorage storage, u8 flags,
                               OutArray<AlbumEntry, BufferAttr_HipcMapAlias> out_entries);

    Result GetAutoSavingStorage(Out<bool> out_is_autosaving);

    Result LoadAlbumScreenShotImageEx1(
        const AlbumFileId& file_id, const ScreenShotDecodeOption& decoder_options,
        OutLargeData<LoadAlbumScreenShotImageOutput, BufferAttr_HipcMapAlias> out_image_output,
        OutArray<u8, BufferAttr_HipcMapAlias | BufferAttr_HipcMapTransferAllowsNonSecure> out_image,
        OutArray<u8, BufferAttr_HipcMapAlias> out_buffer);

    Result LoadAlbumScreenShotThumbnailImageEx1(
        const AlbumFileId& file_id, const ScreenShotDecodeOption& decoder_options,
        OutLargeData<LoadAlbumScreenShotImageOutput, BufferAttr_HipcMapAlias> out_image_output,
        OutArray<u8, BufferAttr_HipcMapAlias | BufferAttr_HipcMapTransferAllowsNonSecure> out_image,
        OutArray<u8, BufferAttr_HipcMapAlias> out_buffer);

    Result TranslateResult(Result in_result);

    Result GetAlbumAccessResultForDebug(Out<Result> out_result);

    std::shared_ptr<AlbumManager> manager = nullptr;
};

} // namespace Service::Capture

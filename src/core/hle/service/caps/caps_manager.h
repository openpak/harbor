// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <mutex>
#include <span>
#include <vector>

#include "common/container/unordered_map.h"

#include "common/fs/fs.h"
#include "core/hle/result.h"
#include "core/hle/service/caps/caps_types.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/os/event.h"

namespace Core {
class System;
}

namespace std {
// Hash used to create lists from AlbumFileId data
template <>
struct hash<Service::Capture::AlbumFileId> {
    size_t operator()(const Service::Capture::AlbumFileId& pad_id) const noexcept {
        u64 hash_value = (static_cast<u64>(pad_id.date.year) << 8);
        hash_value ^= (static_cast<u64>(pad_id.date.month) << 7);
        hash_value ^= (static_cast<u64>(pad_id.date.day) << 6);
        hash_value ^= (static_cast<u64>(pad_id.date.hour) << 5);
        hash_value ^= (static_cast<u64>(pad_id.date.minute) << 4);
        hash_value ^= (static_cast<u64>(pad_id.date.second) << 3);
        hash_value ^= (static_cast<u64>(pad_id.date.unique_id) << 2);
        hash_value ^= (static_cast<u64>(pad_id.storage) << 1);
        hash_value ^= static_cast<u64>(pad_id.type);
        return static_cast<size_t>(hash_value);
    }
};

} // namespace std

namespace Service::Capture {

class AlbumManager {
public:
    explicit AlbumManager(Core::System& system_);
    ~AlbumManager();

    Result DeleteAlbumFile(const AlbumFileId& file_id);
    Result IsAlbumMounted(AlbumStorage storage);
    // Whether the album's storage is presented as inserted. A HOME session's console has the
    // NAND album only; every storage is "mounted" otherwise.
    bool IsStorageAvailable(AlbumStorage storage) const;
    Result GetAlbumMountResult(AlbumStorage storage);
    Result GetAlbumFileCount(u64& out_count, AlbumStorage storage, u8 flags);
    // One entry per content type (screenshot, movie, extra movie), as many as `out_usage` holds.
    Result GetAlbumUsage(std::span<AlbumUsage> out_usage, AlbumStorage storage);
    Result GetAlbumFileList(std::span<AlbumEntry> out_entries, u64& out_entries_count,
                            AlbumStorage storage, u8 flags);
    Result GetAlbumFileList(std::span<ApplicationAlbumFileEntry> out_entries,
                            u64& out_entries_count, ContentType content_type, s64 start_posix_time,
                            s64 end_posix_time, u64 aruid);
    Result GetAlbumFileList(std::span<ApplicationAlbumEntry> out_entries, u64& out_entries_count,
                            ContentType content_type, AlbumFileDateTime start_date,
                            AlbumFileDateTime end_date, u64 aruid);
    Result GetAutoSavingStorage(bool& out_is_autosaving) const;
    Result LoadAlbumScreenShotImage(LoadAlbumScreenShotImageOutput& out_image_output,
                                    std::span<u8> out_image, const AlbumFileId& file_id,
                                    const ScreenShotDecodeOption& decoder_options);
    Result LoadAlbumScreenShotThumbnail(LoadAlbumScreenShotImageOutput& out_image_output,
                                        std::span<u8> out_image, const AlbumFileId& file_id,
                                        const ScreenShotDecodeOption& decoder_options);

    Result SaveScreenShot(ApplicationAlbumEntry& out_entry, const ScreenShotAttribute& attribute,
                          AlbumReportOption report_option, std::span<const u8> image_data,
                          u64 aruid);
    Result SaveScreenShot(ApplicationAlbumEntry& out_entry, const ScreenShotAttribute& attribute,
                          AlbumReportOption report_option, const ApplicationData& app_data,
                          std::span<const u8> image_data, u64 aruid);
    Result SaveEditedScreenShot(ApplicationAlbumEntry& out_entry,
                                const ScreenShotAttribute& attribute, const AlbumFileId& file_id,
                                std::span<const u8> image_data);

    void FlipVerticallyOnWrite(bool flip);

    // The capture button's screenshot (a HOME session): `rgba` is the 1280x720 picture, saved to
    // the album under `title_id`. The thumbnail (96x54 RGBA) is kept for caps:a 301, and the
    // album's file-created event is signalled. `out_file_id` names the new file.
    Result SaveOverlayScreenShot(AlbumFileId& out_file_id, u64 title_id, std::span<const u8> rgba,
                                 std::span<const u8> thumbnail);

    // caps:a 301: the last capture-button screenshot's id and thumbnail, false when none was taken.
    bool GetLastOverlayScreenShot(AlbumFileId& out_file_id, std::vector<u8>& out_thumbnail);

    // caps:a 140.
    Event& GetAlbumFileCreatedEvent() {
        return album_file_created_event;
    }

private:
    static constexpr std::size_t NandAlbumFileLimit = 1000;
    static constexpr std::size_t SdAlbumFileLimit = 10000;

    bool IsHomeSession() const;
    // The storage the album's files report: the NAND album in a HOME session (the SD card is not
    // presented), the SD card otherwise (Eden's own screenshots directory).
    AlbumStorage FileStorage() const;
    void EnsureMountedLocked();
    void FindScreenshots();
    void FindScreenshotsLocked();
    Result CurrentAlbumDateTime(AlbumFileDateTime& out_date) const;
    Result GetFile(std::filesystem::path& out_path, const AlbumFileId& file_id) const;
    Result GetAlbumEntry(AlbumEntry& out_entry, const std::filesystem::path& path) const;
    Result LoadImage(std::span<u8> out_image, const std::filesystem::path& path, int width,
                     int height, ScreenShotDecoderFlag flag) const;
    Result SaveImage(ApplicationAlbumEntry& out_entry, std::span<const u8> image, u64 title_id,
                     const AlbumFileDateTime& date, bool flip, AlbumFileId* out_file_id = nullptr);

    AlbumFileDateTime ConvertToAlbumDateTime(u64 posix_time) const;

    // The album is read by the caps services' server threads and written by the capture button's
    // thread.
    mutable std::mutex mutex;
    bool is_mounted{};
    bool flip_on_write{};
    ::Common::unordered_map<AlbumFileId, std::filesystem::path> album_files;

    AlbumFileId last_overlay_file_id{};
    std::vector<u8> last_overlay_thumbnail;

    Core::System& system;
    KernelHelpers::ServiceContext service_context;
    Event album_file_created_event;
};

} // namespace Service::Capture

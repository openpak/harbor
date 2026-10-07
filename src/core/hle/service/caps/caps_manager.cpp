// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <sstream>

#include "common/fs/file.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/stb.h"
#include "core/core.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/caps/caps_manager.h"
#include "core/hle/service/caps/caps_result.h"
#include "core/hle/service/glue/time/static.h"
#include "core/hle/service/psc/time/system_clock.h"
#include "core/hle/service/service.h"
#include "core/hle/service/sm/sm.h"

namespace Service::Capture {

namespace {
// Orders album dates oldest first.
u64 DateKey(const AlbumFileDateTime& date) {
    return (static_cast<u64>(static_cast<u16>(date.year)) << 40) |
           (static_cast<u64>(static_cast<u8>(date.month)) << 32) |
           (static_cast<u64>(static_cast<u8>(date.day)) << 24) |
           (static_cast<u64>(static_cast<u8>(date.hour)) << 16) |
           (static_cast<u64>(static_cast<u8>(date.minute)) << 8) |
           static_cast<u64>(static_cast<u8>(date.second));
}
} // namespace

AlbumManager::AlbumManager(Core::System& system_)
    : system{system_}, service_context{system_, "caps"}, album_file_created_event{service_context} {}

AlbumManager::~AlbumManager() = default;

bool AlbumManager::IsHomeSession() const {
    return system.GetAppletManager().IsHomeSession();
}

AlbumStorage AlbumManager::FileStorage() const {
    return IsHomeSession() ? AlbumStorage::Nand : AlbumStorage::Sd;
}

bool AlbumManager::IsStorageAvailable(AlbumStorage storage) const {
    if (storage > AlbumStorage::Sd) {
        return false;
    }
    return !IsHomeSession() || storage == AlbumStorage::Nand;
}

void AlbumManager::EnsureMountedLocked() {
    // A console mounts its album at boot; Eden's stock behaviour waits for the first
    // IsAlbumMounted.
    if (!is_mounted && IsHomeSession()) {
        FindScreenshotsLocked();
    }
}

Result AlbumManager::DeleteAlbumFile(const AlbumFileId& file_id) {
    if (file_id.storage > AlbumStorage::Sd) {
        return ResultInvalidStorage;
    }

    std::scoped_lock lk{mutex};
    EnsureMountedLocked();

    if (!is_mounted) {
        return ResultIsNotMounted;
    }

    std::filesystem::path path;
    const auto result = GetFile(path, file_id);

    if (result.IsError()) {
        return result;
    }

    if (!Common::FS::RemoveFile(path)) {
        return ResultFileNotFound;
    }

    album_files.erase(file_id);
    return ResultSuccess;
}

Result AlbumManager::IsAlbumMounted(AlbumStorage storage) {
    if (storage > AlbumStorage::Sd) {
        return ResultInvalidStorage;
    }

    std::scoped_lock lk{mutex};
    is_mounted = true;

    // The directory is read again each time the album is mounted, so what other writers (the
    // frontend's own screenshots) added is listed.
    if (storage == FileStorage()) {
        FindScreenshotsLocked();
    }

    return is_mounted ? ResultSuccess : ResultIsNotMounted;
}

Result AlbumManager::GetAlbumMountResult(AlbumStorage storage) {
    if (storage > AlbumStorage::Sd) {
        return ResultInvalidStorage;
    }
    return IsStorageAvailable(storage) ? ResultSuccess : ResultIsNotMounted;
}

Result AlbumManager::GetAlbumFileCount(u64& out_count, AlbumStorage storage, u8 flags) {
    // The list's own bound: the count is what a list of that storage would hold.
    out_count = 0;
    if (storage > AlbumStorage::Sd) {
        return ResultInvalidStorage;
    }

    std::scoped_lock lk{mutex};
    EnsureMountedLocked();
    if (!is_mounted) {
        return ResultIsNotMounted;
    }

    for (const auto& [file_id, path] : album_files) {
        if (file_id.storage == storage) {
            out_count++;
        }
    }
    return ResultSuccess;
}

Result AlbumManager::GetAlbumUsage(std::span<AlbumUsage> out_usage, AlbumStorage storage) {
    if (storage > AlbumStorage::Sd) {
        return ResultInvalidStorage;
    }

    std::scoped_lock lk{mutex};
    EnsureMountedLocked();
    if (!is_mounted) {
        return ResultIsNotMounted;
    }

    constexpr std::array contents{ContentType::Screenshot, ContentType::Movie,
                                  ContentType::ExtraMovie};
    for (std::size_t i = 0; i < out_usage.size(); i++) {
        out_usage[i] = {};
        if (i >= contents.size()) {
            continue;
        }
        out_usage[i].content = contents[i];
        for (const auto& [file_id, path] : album_files) {
            if (file_id.storage == storage && file_id.type == contents[i]) {
                out_usage[i].file_count++;
                out_usage[i].size += Common::FS::GetSize(path);
            }
        }
    }
    return ResultSuccess;
}

Result AlbumManager::GetAlbumFileList(std::span<AlbumEntry> out_entries, u64& out_entries_count,
                                      AlbumStorage storage, u8 flags) {
    if (storage > AlbumStorage::Sd) {
        return ResultInvalidStorage;
    }

    std::scoped_lock lk{mutex};
    EnsureMountedLocked();

    if (!is_mounted) {
        return ResultIsNotMounted;
    }

    // Oldest first, so the list does not depend on the hash map's order.
    std::vector<AlbumEntry> entries;
    for (auto& [file_id, path] : album_files) {
        if (file_id.storage != storage) {
            continue;
        }
        entries.push_back({.entry_size = Common::FS::GetSize(path), .file_id = file_id});
    }
    std::ranges::sort(entries, [](const AlbumEntry& a, const AlbumEntry& b) {
        return DateKey(a.file_id.date) < DateKey(b.file_id.date);
    });

    for (const auto& entry : entries) {
        if (out_entries_count >= SdAlbumFileLimit) {
            break;
        }
        if (out_entries_count >= out_entries.size()) {
            break;
        }
        out_entries[out_entries_count++] = entry;
    }

    return ResultSuccess;
}

Result AlbumManager::GetAlbumFileList(std::span<ApplicationAlbumFileEntry> out_entries,
                                      u64& out_entries_count, ContentType content_type,
                                      s64 start_posix_time, s64 end_posix_time, u64 aruid) {
    {
        std::scoped_lock lk{mutex};
        EnsureMountedLocked();
        if (!is_mounted) {
            return ResultIsNotMounted;
        }
    }

    std::vector<ApplicationAlbumEntry> album_entries(out_entries.size());
    const auto start_date = ConvertToAlbumDateTime(start_posix_time);
    const auto end_date = ConvertToAlbumDateTime(end_posix_time);
    const auto result = GetAlbumFileList(album_entries, out_entries_count, content_type, start_date,
                                         end_date, aruid);

    if (result.IsError()) {
        return result;
    }

    for (std::size_t i = 0; i < out_entries_count; i++) {
        out_entries[i] = {
            .entry = album_entries[i],
            .datetime = album_entries[i].datetime,
            .unknown = {},
        };
    }

    return ResultSuccess;
}

Result AlbumManager::GetAlbumFileList(std::span<ApplicationAlbumEntry> out_entries,
                                      u64& out_entries_count, ContentType content_type,
                                      AlbumFileDateTime start_date, AlbumFileDateTime end_date,
                                      u64 aruid) {
    std::scoped_lock lk{mutex};
    EnsureMountedLocked();
    if (!is_mounted) {
        return ResultIsNotMounted;
    }

    for (auto& [file_id, path] : album_files) {
        if (file_id.type != content_type) {
            continue;
        }
        if (file_id.date > start_date) {
            continue;
        }
        if (file_id.date < end_date) {
            continue;
        }
        if (out_entries_count >= SdAlbumFileLimit) {
            break;
        }
        if (out_entries_count >= out_entries.size()) {
            break;
        }

        const auto entry_size = Common::FS::GetSize(path);
        out_entries[out_entries_count++] = {
            .size = entry_size,
            .hash{},
            .datetime = file_id.date,
            .storage = file_id.storage,
            .content = content_type,
            .unknown = 1,
        };
    }

    return ResultSuccess;
}

Result AlbumManager::GetAutoSavingStorage(bool& out_is_autosaving) const {
    out_is_autosaving = false;
    return ResultSuccess;
}

Result AlbumManager::LoadAlbumScreenShotImage(LoadAlbumScreenShotImageOutput& out_image_output,
                                              std::span<u8> out_image, const AlbumFileId& file_id,
                                              const ScreenShotDecodeOption& decoder_options) {
    if (file_id.storage > AlbumStorage::Sd) {
        return ResultInvalidStorage;
    }

    std::filesystem::path path;
    {
        std::scoped_lock lk{mutex};
        EnsureMountedLocked();
        if (!is_mounted) {
            return ResultIsNotMounted;
        }
        const auto result = GetFile(path, file_id);
        if (result.IsError()) {
            return result;
        }
    }

    out_image_output = {};
    out_image_output.width = 1280;
    out_image_output.height = 720;
    out_image_output.attribute.orientation = AlbumImageOrientation::None;

    return LoadImage(out_image, path, static_cast<int>(out_image_output.width),
                     +static_cast<int>(out_image_output.height), decoder_options.flags);
}

Result AlbumManager::LoadAlbumScreenShotThumbnail(
    LoadAlbumScreenShotImageOutput& out_image_output, std::span<u8> out_image,
    const AlbumFileId& file_id, const ScreenShotDecodeOption& decoder_options) {
    if (file_id.storage > AlbumStorage::Sd) {
        return ResultInvalidStorage;
    }

    std::filesystem::path path;
    {
        std::scoped_lock lk{mutex};
        EnsureMountedLocked();
        if (!is_mounted) {
            return ResultIsNotMounted;
        }
        const auto result = GetFile(path, file_id);
        if (result.IsError()) {
            return result;
        }
    }

    out_image_output = {};
    out_image_output.width = 320;
    out_image_output.height = 180;
    out_image_output.attribute.orientation = AlbumImageOrientation::None;

    return LoadImage(out_image, path, static_cast<int>(out_image_output.width),
                     +static_cast<int>(out_image_output.height), decoder_options.flags);
}

Result AlbumManager::SaveScreenShot(ApplicationAlbumEntry& out_entry,
                                    const ScreenShotAttribute& attribute,
                                    AlbumReportOption report_option, std::span<const u8> image_data,
                                    u64 aruid) {
    return SaveScreenShot(out_entry, attribute, report_option, {}, image_data, aruid);
}

Result AlbumManager::SaveScreenShot(ApplicationAlbumEntry& out_entry,
                                    const ScreenShotAttribute& attribute,
                                    AlbumReportOption report_option,
                                    const ApplicationData& app_data, std::span<const u8> image_data,
                                    u64 aruid) {
    R_UNLESS(!image_data.empty(), ResultUnknown); //TODO: ???

    const u64 title_id = system.ResolveCallerProgramId(aruid);

    auto static_service =
        system.ServiceManager().GetService<Service::Glue::Time::StaticService>("time:u", true);

    std::shared_ptr<Service::PSC::Time::SystemClock> user_clock{};
    static_service->GetStandardUserSystemClock(&user_clock);

    s64 posix_time{};
    auto result = user_clock->GetCurrentTime(&posix_time);

    if (result.IsError()) {
        return result;
    }

    const auto date = ConvertToAlbumDateTime(posix_time);
    return SaveImage(out_entry, image_data, title_id, date, flip_on_write);
}

Result AlbumManager::SaveEditedScreenShot(ApplicationAlbumEntry& out_entry, const ScreenShotAttribute& attribute, const AlbumFileId& file_id, std::span<const u8> image_data) {
    R_UNLESS(!image_data.empty(), ResultUnknown); //TODO: ???

    auto static_service =
        system.ServiceManager().GetService<Service::Glue::Time::StaticService>("time:u", true);

    std::shared_ptr<Service::PSC::Time::SystemClock> user_clock{};
    static_service->GetStandardUserSystemClock(&user_clock);

    s64 posix_time{};
    auto result = user_clock->GetCurrentTime(&posix_time);

    if (result.IsError()) {
        return result;
    }

    const auto date = ConvertToAlbumDateTime(posix_time);
    return SaveImage(out_entry, image_data, file_id.application_id, date, flip_on_write);
}

Result AlbumManager::CurrentAlbumDateTime(AlbumFileDateTime& out_date) const {
    auto static_service =
        system.ServiceManager().GetService<Service::Glue::Time::StaticService>("time:u", true);
    if (!static_service) {
        return ResultUnknown;
    }

    std::shared_ptr<Service::PSC::Time::SystemClock> user_clock{};
    static_service->GetStandardUserSystemClock(&user_clock);

    s64 posix_time{};
    const auto result = user_clock->GetCurrentTime(&posix_time);
    if (result.IsError()) {
        return result;
    }

    out_date = ConvertToAlbumDateTime(posix_time);
    return ResultSuccess;
}

Result AlbumManager::SaveOverlayScreenShot(AlbumFileId& out_file_id, u64 title_id,
                                           std::span<const u8> rgba,
                                           std::span<const u8> thumbnail) {
    R_UNLESS(rgba.size() == 1280 * 720 * STBI_rgb_alpha, ResultUnknown);

    AlbumFileDateTime date{};
    R_TRY(CurrentAlbumDateTime(date));

    // The picture arrives upright: the capture path already flipped it.
    ApplicationAlbumEntry entry{};
    AlbumFileId file_id{};
    R_TRY(SaveImage(entry, rgba, title_id, date, false, &file_id));

    {
        std::scoped_lock lk{mutex};
        last_overlay_file_id = file_id;
        last_overlay_thumbnail.assign(thumbnail.begin(), thumbnail.end());
    }
    out_file_id = file_id;
    return ResultSuccess;
}

bool AlbumManager::GetLastOverlayScreenShot(AlbumFileId& out_file_id,
                                            std::vector<u8>& out_thumbnail) {
    std::scoped_lock lk{mutex};
    out_file_id = last_overlay_file_id;
    out_thumbnail = last_overlay_thumbnail;
    return !last_overlay_thumbnail.empty();
}

Result AlbumManager::GetFile(std::filesystem::path& out_path, const AlbumFileId& file_id) const {
    const auto file = album_files.find(file_id);

    if (file == album_files.end()) {
        return ResultFileNotFound;
    }

    out_path = file->second;
    return ResultSuccess;
}

void AlbumManager::FindScreenshots() {
    std::scoped_lock lk{mutex};
    FindScreenshotsLocked();
}

void AlbumManager::FindScreenshotsLocked() {
    is_mounted = false;
    album_files.clear();

    // TODO: Swap this with a blocking operation.
    const auto screenshots_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::ScreenshotsDir);
    Common::FS::IterateDirEntries(
        screenshots_dir,
        [this](const std::filesystem::path& full_path) {
            AlbumEntry entry;
            if (GetAlbumEntry(entry, full_path).IsError()) {
                return true;
            }
            while (album_files.contains(entry.file_id)) {
                if (++entry.file_id.date.unique_id == 0) {
                    break;
                }
            }
            album_files[entry.file_id] = full_path;
            return true;
        },
        Common::FS::DirEntryFilter::File);

    is_mounted = true;
}

Result AlbumManager::GetAlbumEntry(AlbumEntry& out_entry, const std::filesystem::path& path) const {
    std::istringstream line_stream(path.filename().string());
    std::string date;
    std::string application;
    std::string time;

    // Parse filename to obtain entry properties
    std::getline(line_stream, application, '_');
    std::getline(line_stream, date, '_');
    std::getline(line_stream, time, '_');

    std::istringstream date_stream(date);
    std::istringstream time_stream(time);
    std::string year;
    std::string month;
    std::string day;
    std::string hour;
    std::string minute;
    std::string second;

    std::getline(date_stream, year, '-');
    std::getline(date_stream, month, '-');
    std::getline(date_stream, day, '-');

    std::getline(time_stream, hour, '-');
    std::getline(time_stream, minute, '-');
    std::getline(time_stream, second, '-');
    std::string sub_second;
    std::getline(time_stream, sub_second, '.');

    try {
        out_entry = {
            .entry_size = 1,
            .file_id{
                .application_id = static_cast<u64>(std::stoll(application, 0, 16)),
                .date =
                    {
                        .year = static_cast<s16>(std::stoi(year)),
                        .month = static_cast<s8>(std::stoi(month)),
                        .day = static_cast<s8>(std::stoi(day)),
                        .hour = static_cast<s8>(std::stoi(hour)),
                        .minute = static_cast<s8>(std::stoi(minute)),
                        .second = static_cast<s8>(std::stoi(second)),
                        .unique_id = 0,
                    },
                .storage = FileStorage(),
                .type = ContentType::Screenshot,
                .unknown = 1,
            },
        };

        // Files this album wrote end in a counter ("-000"); the frontend's end in milliseconds,
        // which a signed byte cannot hold, so those count from 0 as they collide.
        if (const int counter = sub_second.empty() ? -1 : std::atoi(sub_second.c_str());
            counter >= 0 && counter < 100) {
            out_entry.file_id.date.unique_id = static_cast<s8>(counter);
        }
    } catch (const std::invalid_argument&) {
        return ResultUnknown;
    } catch (const std::out_of_range&) {
        return ResultUnknown;
    } catch (const std::exception&) {
        return ResultUnknown;
    }

    return ResultSuccess;
}

Result AlbumManager::LoadImage(std::span<u8> out_image, const std::filesystem::path& path,
                               int width, int height, ScreenShotDecoderFlag flag) const {
    if (out_image.size() != static_cast<std::size_t>(width * height * STBI_rgb_alpha)) {
        return ResultUnknown;
    }

    const Common::FS::IOFile db_file{path, Common::FS::FileAccessMode::Read,
                                     Common::FS::FileType::BinaryFile};

    std::vector<u8> raw_file(db_file.GetSize());
    if (db_file.Read(raw_file) != raw_file.size()) {
        return ResultUnknown;
    }

    int filter_flag = STBIR_FILTER_DEFAULT;
    int original_width, original_height, color_channels;
    const auto dbi_image =
        stbi_load_from_memory(raw_file.data(), static_cast<int>(raw_file.size()), &original_width,
                              &original_height, &color_channels, STBI_rgb_alpha);

    if (dbi_image == nullptr) {
        return ResultUnknown;
    }

    switch (flag) {
    case ScreenShotDecoderFlag::EnableFancyUpsampling:
        filter_flag = STBIR_FILTER_TRIANGLE;
        break;
    case ScreenShotDecoderFlag::EnableBlockSmoothing:
        filter_flag = STBIR_FILTER_BOX;
        break;
    default:
        filter_flag = STBIR_FILTER_DEFAULT;
        break;
    }

    stbir_resize_uint8_srgb(dbi_image, original_width, original_height, 0, out_image.data(), width,
                            height, 0, STBI_rgb_alpha, 3, filter_flag);
    stbi_image_free(dbi_image);

    return ResultSuccess;
}

void AlbumManager::FlipVerticallyOnWrite(bool flip) {
    flip_on_write = flip;
}

static void PNGToMemory(void* context, void* data, int len) {
    std::vector<u8>* png_image = static_cast<std::vector<u8>*>(context);
    unsigned char* png = static_cast<unsigned char*>(data);
    png_image->insert(png_image->end(), png, png + len);
}

Result AlbumManager::SaveImage(ApplicationAlbumEntry& out_entry, std::span<const u8> image,
                               u64 title_id, const AlbumFileDateTime& date, bool flip,
                               AlbumFileId* out_file_id) {
    const auto screenshot_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::ScreenshotsDir);
    if (!Common::FS::CreateDir(screenshot_dir)) {
        return ResultFileCountLimit;
    }

    // One writer at a time: the PNG encoder's flip flag is global, and the name below is picked
    // from what is on disk.
    std::scoped_lock lk{mutex};

    AlbumFileDateTime saved_date = date;
    std::filesystem::path file_path;
    for (int counter = 0; counter < 100; counter++) {
        saved_date.unique_id = static_cast<s8>(counter);
        file_path = screenshot_dir / fmt::format("{:016x}_{:04}-{:02}-{:02}_{:02}-{:02}-{:02}-{:03}.png",
                                                 title_id, u16(date.year), u8(date.month),
                                                 u8(date.day), u8(date.hour), u8(date.minute),
                                                 u8(date.second), counter);
        if (!Common::FS::Exists(file_path)) {
            break;
        }
        if (counter == 99) {
            return ResultFileCountLimit;
        }
    }

    std::vector<u8> png_image;
    stbi_flip_vertically_on_write(flip);
    const bool encoded = stbi_write_png_to_func(PNGToMemory, &png_image, 1280, 720, STBI_rgb_alpha,
                                                image.data(), 0) != 0;
    stbi_flip_vertically_on_write(false);
    if (!encoded) {
        return ResultFileCountLimit;
    }

    {
        const Common::FS::IOFile db_file{file_path, Common::FS::FileAccessMode::Write,
                                         Common::FS::FileType::BinaryFile};
        if (!db_file.IsOpen() || db_file.Write(png_image) != png_image.size()) {
            return ResultFileCountLimit;
        }
    }

    const AlbumFileId file_id{
        .application_id = title_id,
        .date = saved_date,
        .storage = FileStorage(),
        .type = ContentType::Screenshot,
        .unknown = 1,
    };
    album_files[file_id] = file_path;
    if (out_file_id != nullptr) {
        *out_file_id = file_id;
    }

    out_entry = {
        .size = png_image.size(),
        .hash = {},
        .datetime = saved_date,
        .storage = file_id.storage,
        .content = ContentType::Screenshot,
        .unknown = 1,
    };

    // A new picture is in the album: caps:a 140's event.
    album_file_created_event.Signal(system.Kernel());

    return ResultSuccess;
}

AlbumFileDateTime AlbumManager::ConvertToAlbumDateTime(u64 posix_time) const {
    auto static_service =
        system.ServiceManager().GetService<Service::Glue::Time::StaticService>("time:u", true);

    std::shared_ptr<Service::Glue::Time::TimeZoneService> timezone_service{};
    static_service->GetTimeZoneService(&timezone_service);

    Service::PSC::Time::CalendarTime calendar_time{};
    Service::PSC::Time::CalendarAdditionalInfo additional_info{};
    timezone_service->ToCalendarTimeWithMyRule(&calendar_time, &additional_info, posix_time);

    return {
        .year = calendar_time.year,
        .month = calendar_time.month,
        .day = calendar_time.day,
        .hour = calendar_time.hour,
        .minute = calendar_time.minute,
        .second = calendar_time.second,
        .unique_id = 0,
    };
}

} // namespace Service::Capture

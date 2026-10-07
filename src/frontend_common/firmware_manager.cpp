// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "firmware_manager.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <span>
#include <common/fs/fs_paths.h>
#include <zlib.h>

#include "common/fs/fs.h"
#include "common/fs/path_util.h"

#include "common/logging.h"

#include "common/fs/file.h"
#include "core/crypto/key_manager.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/loader/loader.h"
#include "frontend_common/content_manager.h"

#ifdef __ANDROID__
#include <jni.h>
#include <common/android/id_cache.h>
#include <common/android/android_common.h>
#include "common/fs/fs_android.h"
#endif

FirmwareManager::KeyInstallResult
FirmwareManager::InstallKeys(std::string location, std::string extension) {
    LOG_INFO(Frontend, "Installing key files from {}", location);

    const auto keys_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::KeysDir);

#ifdef __ANDROID__
    // A plain path (Harbor's setup installs a checked copy of the picked file) is copied like on desktop.
    if (Common::FS::Android::IsContentUri(location)) {
        JNIEnv *env = Common::Android::GetEnvForThread();

        jstring jsrc = Common::Android::ToJString(env, location);

        jclass native = Common::Android::GetNativeLibraryClass();
        jmethodID getExtension = Common::Android::GetFileExtension();

        jstring jext = static_cast<jstring>(env->CallStaticObjectMethod(
            native,
            getExtension,
            jsrc
        ));

        std::string ext = Common::Android::GetJString(env, jext);

        if (ext != extension) {
            return ErrorWrongFilename;
        }

        jmethodID copyToStorage = Common::Android::GetCopyToStorage();
        jstring jdest = Common::Android::ToJString(env, keys_dir.string() + "/");

        jboolean copyResult = env->CallStaticBooleanMethod(
            native,
            copyToStorage,
            jsrc,
            jdest
        );

        if (!copyResult) {
            return ErrorFailedCopy;
        }
    } else
#endif
    {
        if (!location.ends_with(extension)) {
            return ErrorWrongFilename;
        }

        bool prod_keys_found = false;

        const std::filesystem::path prod_key_path = location;
        const std::filesystem::path key_source_path = prod_key_path.parent_path();

        if (!Common::FS::IsDir(key_source_path)) {
            return InvalidDir;
        }

        std::vector<std::filesystem::path> source_key_files;

        if (Common::FS::Exists(prod_key_path)) {
            prod_keys_found = true;
            source_key_files.emplace_back(prod_key_path);
        }

        if (Common::FS::Exists(key_source_path / "title.keys")) {
            source_key_files.emplace_back(key_source_path / "title.keys");
        }

        if (Common::FS::Exists(key_source_path / "key_retail.bin")) {
            source_key_files.emplace_back(key_source_path / "key_retail.bin");
        }

        if (source_key_files.empty() || !prod_keys_found) {
            return ErrorWrongFilename;
        }

        for (const auto &key_file : source_key_files) {
            std::filesystem::path destination_key_file = keys_dir / key_file.filename();
            if (!std::filesystem::copy_file(key_file,
                                            destination_key_file,
                                            std::filesystem::copy_options::overwrite_existing)) {
                LOG_ERROR(Frontend,
                          "Failed to copy file {} to {}",
                          key_file.string(),
                          destination_key_file.string());
                return ErrorFailedCopy;
            }
        }
    }

    // Reinitialize the key manager
    Core::Crypto::KeyManager::Instance().ReloadKeys();

    if (ContentManager::AreKeysPresent()) {
        return Success;
    }

    // Let the frontend handle everything else
    return ErrorFailedInit;
}

FirmwareManager::FirmwareCheckResult FirmwareManager::VerifyFirmware(Core::System &system) {
    if (!CheckFirmwarePresence(system)) {
        return ErrorFirmwareMissing;
    } else {
        const auto pair = GetFirmwareVersion(system);
        const auto result = pair.second;

        if (result.IsError()) {
            LOG_INFO(Frontend, "Unable to read firmware");
            return ErrorFirmwareCorrupted;
        }
    }

    return FirmwareGood;
}

// Firmware installation shared by every frontend: copy into a staging folder beside the NAND, check
// the copy, then swap it in with a rename. The installed firmware is only replaced by one that checked out.

namespace {

namespace fs = std::filesystem;

constexpr u64 SystemVersionDataId = 0x0100000000000809ULL;

fs::path SystemContentsDir() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir) / "system" / "Contents";
}

fs::path StagingDir() {
    return SystemContentsDir() / "harbor-staging";
}

std::string Megabytes(u64 bytes) {
    return fmt::format("{:.0f}", static_cast<double>(bytes) / (1024.0 * 1024.0));
}

// Only plain NCA file names land in the NAND; nothing in an archive chooses where a file goes.
std::optional<std::string> NcaFileName(std::string_view entry_name) {
    const auto slash = entry_name.find_last_of("/\\");
    const auto name = std::string(slash == std::string_view::npos ? entry_name
                                                                   : entry_name.substr(slash + 1));
    if (name.size() < 5 || !name.ends_with(".nca") || name.starts_with(".")) {
        return std::nullopt;
    }
    const bool plain = std::all_of(name.begin(), name.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-';
    });
    return plain ? std::optional{name} : std::nullopt;
}

// A minimal ZIP reader: stored and deflated entries, ZIP64 sizes, CRC checked.
struct ZipEntry {
    std::string name;
    u16 flags = 0;
    u16 method = 0;
    u32 crc = 0;
    u64 compressed = 0;
    u64 size = 0;
    u64 local_offset = 0;
};

u16 Le16(const u8* p) {
    return static_cast<u16>(p[0] | (p[1] << 8));
}

u32 Le32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}

u64 Le64(const u8* p) {
    return static_cast<u64>(Le32(p)) | (static_cast<u64>(Le32(p + 4)) << 32);
}

bool ReadAt(const Common::FS::IOFile& file, u64 offset, std::span<u8> out) {
    return file.Seek(static_cast<s64>(offset)) && file.ReadSpan(out) == out.size();
}

std::optional<std::vector<ZipEntry>> ReadZipDirectory(const Common::FS::IOFile& file, std::string& error) {
    const u64 file_size = file.GetSize();
    if (file_size < 22) {
        error = "This is not a .zip file.";
        return std::nullopt;
    }

    const u64 tail_size = std::min<u64>(file_size, 0xFFFF + 22);
    std::vector<u8> tail(tail_size);
    if (!ReadAt(file, file_size - tail_size, tail)) {
        error = "The file could not be read.";
        return std::nullopt;
    }
    std::optional<size_t> eocd;
    for (size_t i = tail_size - 22 + 1; i-- > 0;) {
        if (Le32(&tail[i]) == 0x06054b50) {
            eocd = i;
            break;
        }
    }
    if (!eocd) {
        error = "This is not a .zip file.";
        return std::nullopt;
    }

    u64 entries = Le16(&tail[*eocd + 10]);
    u64 directory_size = Le32(&tail[*eocd + 12]);
    u64 directory_offset = Le32(&tail[*eocd + 16]);
    if (*eocd >= 20 && Le32(&tail[*eocd - 20]) == 0x07064b50) {
        std::array<u8, 56> zip64{};
        if (!ReadAt(file, Le64(&tail[*eocd - 20 + 8]), zip64) || Le32(zip64.data()) != 0x06064b50) {
            error = "The .zip file is damaged.";
            return std::nullopt;
        }
        entries = Le64(&zip64[32]);
        directory_size = Le64(&zip64[40]);
        directory_offset = Le64(&zip64[48]);
    }
    if (directory_size > 64ULL * 1024 * 1024 || directory_offset + directory_size > file_size) {
        error = "The .zip file is damaged.";
        return std::nullopt;
    }

    std::vector<u8> directory(directory_size);
    if (!ReadAt(file, directory_offset, directory)) {
        error = "The file could not be read.";
        return std::nullopt;
    }

    std::vector<ZipEntry> result;
    size_t pos = 0;
    for (u64 i = 0; i < entries; i++) {
        if (pos + 46 > directory.size() || Le32(&directory[pos]) != 0x02014b50) {
            error = "The .zip file is damaged.";
            return std::nullopt;
        }
        const u8* h = &directory[pos];
        ZipEntry entry;
        entry.flags = Le16(h + 8);
        entry.method = Le16(h + 10);
        entry.crc = Le32(h + 16);
        entry.compressed = Le32(h + 20);
        entry.size = Le32(h + 24);
        const u16 name_length = Le16(h + 28);
        const u16 extra_length = Le16(h + 30);
        const u16 comment_length = Le16(h + 32);
        entry.local_offset = Le32(h + 42);
        if (pos + 46 + name_length + extra_length + comment_length > directory.size()) {
            error = "The .zip file is damaged.";
            return std::nullopt;
        }
        entry.name.assign(reinterpret_cast<const char*>(h + 46), name_length);

        // ZIP64 extra field: the values that did not fit, in this order.
        const u8* extra = h + 46 + name_length;
        for (size_t x = 0; x + 4 <= extra_length;) {
            const u16 id = Le16(extra + x);
            const u16 length = Le16(extra + x + 2);
            if (x + 4 + length > extra_length) {
                break;
            }
            if (id == 0x0001) {
                const u8* field = extra + x + 4;
                size_t used = 0;
                const auto take = [&](u64& value) {
                    if (value == 0xFFFFFFFF && used + 8 <= length) {
                        value = Le64(field + used);
                        used += 8;
                    }
                };
                take(entry.size);
                take(entry.compressed);
                take(entry.local_offset);
            }
            x += 4 + length;
        }
        result.push_back(std::move(entry));
        pos += 46 + name_length + extra_length + comment_length;
    }
    return result;
}

bool ExtractZipEntry(const Common::FS::IOFile& file, const ZipEntry& entry, const fs::path& destination,
                     const std::function<bool(u64)>& advance, std::string& error) {
    std::array<u8, 30> local{};
    if (!ReadAt(file, entry.local_offset, local) || Le32(local.data()) != 0x04034b50) {
        error = "The .zip file is damaged.";
        return false;
    }
    const u64 data_offset = entry.local_offset + 30 + Le16(&local[26]) + Le16(&local[28]);
    if (!file.Seek(static_cast<s64>(data_offset))) {
        error = "The file could not be read.";
        return false;
    }

    Common::FS::IOFile out(destination, Common::FS::FileAccessMode::Write);
    if (!out.IsOpen()) {
        error = "The firmware could not be written to the console's storage.";
        return false;
    }

    constexpr size_t Chunk = 1024 * 1024;
    std::vector<u8> in_buffer(Chunk);
    std::vector<u8> out_buffer(Chunk);
    uLong crc = crc32(0L, Z_NULL, 0);
    u64 written = 0;
    u64 remaining = entry.compressed;

    const auto write = [&](const u8* data, size_t size) {
        if (out.WriteSpan(std::span<const u8>(data, size)) != size) {
            error = "The firmware could not be written to the console's storage.";
            return false;
        }
        crc = crc32(crc, data, static_cast<uInt>(size));
        written += size;
        return true;
    };

    if (entry.method == 0) {
        while (remaining > 0) {
            const size_t n = static_cast<size_t>(std::min<u64>(remaining, Chunk));
            if (file.ReadSpan(std::span<u8>(in_buffer.data(), n)) != n) {
                error = "The file could not be read.";
                return false;
            }
            remaining -= n;
            if (!write(in_buffer.data(), n)) {
                return false;
            }
            if (!advance(n)) {
                return false;
            }
        }
    } else if (entry.method == 8) {
        z_stream stream{};
        if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
            error = "The .zip file could not be unpacked.";
            return false;
        }
        int status = Z_OK;
        while (status != Z_STREAM_END) {
            if (stream.avail_in == 0) {
                const size_t n = static_cast<size_t>(std::min<u64>(remaining, Chunk));
                if (n == 0 || file.ReadSpan(std::span<u8>(in_buffer.data(), n)) != n) {
                    inflateEnd(&stream);
                    error = "The .zip file is damaged.";
                    return false;
                }
                remaining -= n;
                stream.next_in = in_buffer.data();
                stream.avail_in = static_cast<uInt>(n);
            }
            stream.next_out = out_buffer.data();
            stream.avail_out = static_cast<uInt>(out_buffer.size());
            status = inflate(&stream, Z_NO_FLUSH);
            if (status != Z_OK && status != Z_STREAM_END) {
                inflateEnd(&stream);
                error = "The .zip file is damaged.";
                return false;
            }
            const size_t produced = out_buffer.size() - stream.avail_out;
            if (!write(out_buffer.data(), produced) || !advance(produced)) {
                inflateEnd(&stream);
                return false;
            }
        }
        inflateEnd(&stream);
    } else {
        error = "The .zip file uses a compression Harbor cannot unpack. Use a plain .zip.";
        return false;
    }

    if (written != entry.size || crc != entry.crc) {
        error = "The .zip file is damaged (a checksum does not match).";
        return false;
    }
    return true;
}

// The installed or staged firmware, read through a RegisteredCache like the NAND content provider.
FirmwareManager::InstalledFirmware Inspect(const fs::path& dir) {
    FirmwareManager::InstalledFirmware info;
    std::error_code ec;
    if (!fs::is_directory(dir, ec) || fs::is_empty(dir, ec)) {
        return info;
    }
    info.present = true;

    FileSys::RealVfsFilesystem vfs;
    auto vdir = vfs.OpenDirectory(dir.string(), FileSys::OpenMode::Read);
    if (!vdir) {
        return info;
    }
    FileSys::RegisteredCache cache(vdir);

    const auto qlaunch = cache.GetEntry(FirmwareManager::QLaunchProgramId,
                                        FileSys::ContentRecordType::Program);
    info.has_qlaunch = qlaunch && qlaunch->GetStatus() == Loader::ResultStatus::Success;

    const auto version_nca = cache.GetEntry(SystemVersionDataId, FileSys::ContentRecordType::Data);
    if (!version_nca || version_nca->GetStatus() != Loader::ResultStatus::Success) {
        return info;
    }
    const auto romfs = FileSys::ExtractRomFS(version_nca->GetRomFS());
    const auto file = romfs ? romfs->GetFile("file") : nullptr;
    if (!file) {
        return info;
    }
    const auto data = file->ReadAllBytes();
    if (data.size() != sizeof(Service::Set::FirmwareVersionFormat)) {
        return info;
    }
    Service::Set::FirmwareVersionFormat format{};
    std::memcpy(&format, data.data(), sizeof(format));
    info.version = std::string(format.display_version.data(),
                               strnlen(format.display_version.data(), format.display_version.size()));
    return info;
}

} // namespace

FirmwareManager::FirmwareResult FirmwareManager::StageFirmware(const std::string& source,
                                                               const FirmwareProgressCallback& progress) {
    FirmwareResult result;
    LOG_INFO(Frontend, "Staging firmware");

    if (!ContentManager::AreKeysPresent()) {
        result.error = "Install your keys first: the firmware cannot be read without them.";
        return result;
    }

    const auto staging = StagingDir();
    std::error_code ec;
    fs::remove_all(staging, ec);
    if (!fs::create_directories(staging, ec) && ec) {
        result.error = "The console's storage could not be written: " + ec.message();
        return result;
    }

    const auto fail = [&](std::string error) {
        fs::remove_all(staging, ec);
        result.error = std::move(error);
        LOG_WARNING(Frontend, "Firmware not staged: {}", result.error);
        return result;
    };

    bool is_folder = false;
#ifdef __ANDROID__
    if (!Common::FS::Android::IsContentUri(source))
#endif
    {
        is_folder = fs::is_directory(source, ec);
    }

    u64 total = 0;
    u64 done = 0;
    size_t count = 0;
    const auto advance = [&](u64 bytes) {
        done += bytes;
        return !progress || progress({fmt::format("Copying the firmware ({} / {} MB)", Megabytes(done),
                                                  Megabytes(total)),
                                      total ? std::optional{static_cast<double>(done) / total}
                                            : std::nullopt});
    };

    if (is_folder) {
        std::vector<std::pair<fs::path, std::string>> files;
        try {
            for (const auto& entry : fs::recursive_directory_iterator(source)) {
                if (!entry.is_regular_file(ec)) {
                    continue;
                }
                if (const auto name = NcaFileName(entry.path().filename().string())) {
                    files.emplace_back(entry.path(), *name);
                    total += entry.file_size(ec);
                }
            }
        } catch (const fs::filesystem_error& e) {
            return fail(std::string("The folder could not be read: ") + e.code().message());
        }
        if (files.empty()) {
            return fail("This folder has no .nca files. Choose the folder your firmware dump is in.");
        }
        for (const auto& [path, name] : files) {
            fs::copy_file(path, staging / name, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                return fail("The firmware could not be copied: " + ec.message());
            }
            if (!advance(fs::file_size(path, ec))) {
                return fail("Cancelled.");
            }
            count++;
        }
    } else {
        Common::FS::IOFile file(source, Common::FS::FileAccessMode::Read);
        if (!file.IsOpen()) {
            return fail("The file could not be opened.");
        }
        std::string error;
        const auto entries = ReadZipDirectory(file, error);
        if (!entries) {
            return fail(error);
        }
        std::vector<std::pair<const ZipEntry*, std::string>> ncas;
        for (const auto& entry : *entries) {
            if (const auto name = NcaFileName(entry.name); name && !entry.name.ends_with("/")) {
                if (entry.flags & 1) {
                    return fail("The .zip file is encrypted. Use a plain .zip.");
                }
                ncas.emplace_back(&entry, *name);
                total += entry.size;
            }
        }
        if (ncas.empty()) {
            return fail("This .zip file has no .nca files, so it is not a firmware dump.");
        }
        for (const auto& [entry, name] : ncas) {
            if (!ExtractZipEntry(file, *entry, staging / name, advance, error)) {
                return fail(error.empty() ? "Cancelled." : error);
            }
            count++;
        }
    }

    if (progress) {
        progress({"Checking the firmware", std::nullopt});
    }
    const auto staged = Inspect(staging);
    if (staged.version.empty()) {
        return fail("This firmware has no readable system version. Check that it was dumped from "
                    "your console and that your keys are its keys.");
    }
    if (!staged.has_qlaunch) {
        return fail(fmt::format("Firmware {} has no HOME menu (qlaunch), so the console cannot start "
                                "from it.",
                                staged.version));
    }

    LOG_INFO(Frontend, "Firmware {} staged ({} files)", staged.version, count);
    result.ok = true;
    result.version = staged.version;
    return result;
}

bool FirmwareManager::CommitStagedFirmware(std::string* error) {
    const auto contents = SystemContentsDir();
    const auto staging = StagingDir();
    const auto registered = contents / "registered";
    const auto replaced = contents / "harbor-replaced";
    std::error_code ec;

    if (!fs::is_directory(staging, ec)) {
        if (error) {
            *error = "No firmware is waiting to be installed.";
        }
        return false;
    }

    fs::remove_all(replaced, ec);
    const bool had_firmware = fs::exists(registered, ec);
    if (had_firmware) {
        fs::rename(registered, replaced, ec);
        if (ec) {
            if (error) {
                *error = "The installed firmware could not be moved aside: " + ec.message();
            }
            return false;
        }
    }
    fs::rename(staging, registered, ec);
    if (ec) {
        const auto message = ec.message();
        if (had_firmware) {
            fs::rename(replaced, registered, ec);
        }
        if (error) {
            *error = "The firmware could not be installed: " + message;
        }
        return false;
    }
    fs::remove_all(replaced, ec);
    LOG_INFO(Frontend, "Staged firmware installed");
    return true;
}

void FirmwareManager::DiscardStagedFirmware() {
    std::error_code ec;
    fs::remove_all(StagingDir(), ec);
}

FirmwareManager::FirmwareResult FirmwareManager::InstallFirmware(const std::string& source,
                                                                 const FirmwareProgressCallback& progress) {
    auto result = StageFirmware(source, progress);
    if (!result.ok) {
        return result;
    }
    std::string error;
    if (!CommitStagedFirmware(&error)) {
        DiscardStagedFirmware();
        result.ok = false;
        result.error = error;
    }
    return result;
}

FirmwareManager::InstalledFirmware FirmwareManager::InspectInstalledFirmware() {
    return Inspect(SystemContentsDir() / "registered");
}

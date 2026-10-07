// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <sstream>
#include <system_error>

#include "common/fs/file.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "core/crypto/key_manager.h"
#include "frontend_common/content_manager.h"
#include "frontend_common/firmware_manager.h"
#include "harbor_setup/keys.h"

#ifdef __ANDROID__
#include "common/fs/fs_android.h"
#endif

namespace HarborSetup {

namespace {

namespace fs = std::filesystem;

constexpr std::uint64_t MaxKeysFile = 16ULL * 1024 * 1024;

std::string Trim(std::string text) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), not_space));
    text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(), text.end());
    return text;
}

bool IsHex(const std::string& text) {
    return !text.empty() && std::all_of(text.begin(), text.end(),
                                        [](unsigned char c) { return std::isxdigit(c); });
}

fs::path ImportDir() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "harbor-import" / "keys";
}

std::optional<std::string> ReadPicked(const std::string& location, std::string& error) {
    Common::FS::IOFile file(location, Common::FS::FileAccessMode::Read);
    if (!file.IsOpen()) {
        error = "The file could not be opened.";
        return std::nullopt;
    }
    const auto size = file.GetSize();
    if (size == 0 || size > MaxKeysFile) {
        error = size == 0 ? "The file is empty." : "The file is too large to be a keys file.";
        return std::nullopt;
    }
    std::string text(static_cast<size_t>(size), '\0');
    if (file.ReadSpan(std::span<char>(text.data(), text.size())) != text.size()) {
        error = "The file could not be read.";
        return std::nullopt;
    }
    return text;
}

bool WriteStaged(const fs::path& path, const std::string& text) {
    Common::FS::IOFile file(path, Common::FS::FileAccessMode::Write);
    return file.IsOpen() &&
           file.WriteSpan(std::span<const char>(text.data(), text.size())) == text.size();
}

void ClearImport() {
    std::error_code ec;
    fs::remove_all(ImportDir(), ec);
    fs::remove(ImportDir().parent_path(), ec); // only when nothing else is in it
}

std::string InstallMessage(FirmwareManager::KeyInstallResult result) {
    switch (result) {
    case FirmwareManager::Success:
        return {};
    case FirmwareManager::ErrorFailedInit:
        return "These keys cannot read the console's content. Dump prod.keys again from your "
               "console.";
    case FirmwareManager::ErrorFailedCopy:
        return "The keys could not be copied into Harbor's data folder.";
    default:
        return "The keys could not be installed.";
    }
}

} // namespace

std::string KeysFileProblem(const std::string& text, bool title_keys) {
    std::istringstream lines(text);
    bool header_key = false;
    size_t count = 0;
    for (std::string line; std::getline(lines, line);) {
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') {
            continue;
        }
        const auto equals = line.find('=');
        if (equals == std::string::npos) {
            return "This is not a keys file: a line is not \"name = value\".";
        }
        const auto name = Trim(line.substr(0, equals));
        const auto value = Trim(line.substr(equals + 1));
        if (name.empty() || !IsHex(value)) {
            return "This is not a keys file: a key has no hexadecimal value.";
        }
        if (title_keys && (name.size() != 32 || !IsHex(name) || value.size() != 32)) {
            return "This is not a title.keys file.";
        }
        if (name == "header_key") {
            if (value.size() != 64) {
                return "The header_key in this file is not 32 bytes long.";
            }
            header_key = true;
        }
        count++;
    }
    if (count == 0) {
        return "The file has no keys.";
    }
    if (!title_keys && !header_key) {
        return "This file has no header_key, so it cannot be a prod.keys dumped from a console.";
    }
    return {};
}

KeysResult ImportProdKeys(const std::string& location) {
    std::string error;
    const auto text = ReadPicked(location, error);
    if (!text) {
        return {false, error};
    }
    if (auto problem = KeysFileProblem(*text, false); !problem.empty()) {
        return {false, problem + " The installed keys are unchanged."};
    }

    ClearImport();
    std::error_code ec;
    fs::create_directories(ImportDir(), ec);
    const auto staged = ImportDir() / "prod.keys";
    if (!WriteStaged(staged, *text)) {
        ClearImport();
        return {false, "The keys could not be copied into Harbor's data folder."};
    }

    // On desktop, a title.keys beside the picked file comes along, when it is one.
    bool title_keys = false;
#ifdef __ANDROID__
    if (!Common::FS::Android::IsContentUri(location))
#endif
    {
        const auto sibling = fs::path(location).parent_path() / "title.keys";
        if (fs::is_regular_file(sibling, ec)) {
            std::string sibling_error;
            if (const auto titles = ReadPicked(sibling.string(), sibling_error);
                titles && KeysFileProblem(*titles, true).empty()) {
                title_keys = WriteStaged(ImportDir() / "title.keys", *titles);
            }
        }
    }

    // Keep the installed files until the new ones load: a well-formed file can still be the wrong one.
    const auto keys_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::KeysDir);
    const auto backup = fs::path(ImportDir()).parent_path() / "keys-before";
    fs::remove_all(backup, ec);
    fs::create_directories(backup, ec);
    for (const char* name : {"prod.keys", "title.keys"}) {
        if (fs::is_regular_file(keys_dir / name, ec)) {
            fs::copy_file(keys_dir / name, backup / name, fs::copy_options::overwrite_existing, ec);
        }
    }

    const auto result = FirmwareManager::InstallKeys(staged.string(), "keys");
    ClearImport();
    if (result != FirmwareManager::Success) {
        for (const char* name : {"prod.keys", "title.keys"}) {
            if (fs::is_regular_file(backup / name, ec)) {
                fs::copy_file(backup / name, keys_dir / name, fs::copy_options::overwrite_existing, ec);
            } else if (result == FirmwareManager::ErrorFailedInit) {
                fs::remove(keys_dir / name, ec);
            }
        }
        fs::remove_all(backup, ec);
        Core::Crypto::KeyManager::Instance().ReloadKeys();
        LOG_WARNING(Frontend, "harbor: keys not installed (result {})", static_cast<int>(result));
        return {false, InstallMessage(result) + " The installed keys are unchanged."};
    }
    fs::remove_all(backup, ec);
    LOG_INFO(Frontend, "harbor: keys installed{}", title_keys ? " with title.keys" : "");
    return {true, title_keys ? "Your keys and the title.keys beside them are installed."
                             : "Your keys are installed."};
}

KeysResult ImportTitleKeys(const std::string& location) {
    std::string error;
    const auto text = ReadPicked(location, error);
    if (!text) {
        return {false, error};
    }
    if (auto problem = KeysFileProblem(*text, true); !problem.empty()) {
        return {false, problem + " The installed title keys are unchanged."};
    }
    const auto keys_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::KeysDir);
    const auto temporary = keys_dir / "title.keys.tmp";
    std::error_code ec;
    if (!WriteStaged(temporary, *text)) {
        return {false, "The title keys could not be copied into Harbor's data folder."};
    }
    fs::rename(temporary, keys_dir / "title.keys", ec);
    if (ec) {
        fs::remove(temporary, ec);
        return {false, "The title keys could not be installed."};
    }
    Core::Crypto::KeyManager::Instance().ReloadKeys();
    LOG_INFO(Frontend, "harbor: title.keys installed");
    return {true, "Your title keys are installed."};
}

bool KeysUsable() {
    return ContentManager::AreKeysPresent();
}

} // namespace HarborSetup

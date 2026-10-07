// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace HarborSetup {

/// What the first-run setup chose, kept in the data directory as harbor-setup.json so every host builds
/// the same HOME session from it. An empty optional means its page has not been passed yet; an empty list
/// means the page was passed with nothing chosen (the optional DLC and update pages).
/// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).
struct Record {
    bool keys_present = false;
    std::optional<std::string> firmware_version;
    std::optional<std::vector<std::string>> game_dirs;
    std::optional<std::vector<std::string>> dlc_dirs;
    std::optional<std::vector<std::string>> update_dirs;
    /// The graphics driver the console starts with (Android, Adreno): an installed driver's file name, or
    /// empty for the phone's own driver.
    std::optional<std::string> gpu_driver;
    bool completed = false;

    /// The DLC and update folders together, without duplicates: what a HOME session reads add-ons from.
    std::vector<std::string> AutoloadDirs() const;
};

inline constexpr const char* RecordFileName = "harbor-setup.json";

/// <user>/harbor-setup.json in Eden's user directory.
std::filesystem::path RecordPath();

/// The record at `path`, or a new one when it is missing or unreadable.
Record LoadRecord(const std::filesystem::path& path);

/// Writes the record through a temporary file and a rename, so a crash never leaves half a file behind.
bool SaveRecord(const Record& record, const std::filesystem::path& path, std::string* error = nullptr);

std::string ToJson(const Record& record);
std::optional<Record> FromJson(const std::string& text);

} // namespace HarborSetup

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"
#include "core/core.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/registered_cache.h"
#include "core/hle/service/filesystem/filesystem.h"
#include <algorithm>
#include <array>
#include <core/hle/service/am/frontend/applet_mii_edit.h>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "core/hle/result.h"
#include "core/hle/service/set/settings_types.h"
#include "core/hle/service/set/system_settings_server.h"

namespace FirmwareManager {

static constexpr std::array<u64, 1> FIRMWARE_REQUIRED_GAMES = {
    0x0100152000022000ULL, // MK8DX
};

enum KeyInstallResult {
    Success,
    InvalidDir,
    ErrorFailedCopy,
    ErrorWrongFilename,
    ErrorFailedInit,
};

/**
 * @brief Installs any arbitrary set of keys for the emulator.
 * @param location Where the keys are located.
 * @param expected_extension What extension the file should have.
 * @return A result code for the operation.
 */
KeyInstallResult InstallKeys(std::string location, std::string expected_extension);

/**
 * \brief Check if the specified program requires firmware to run properly.
 * It is the responsibility of the frontend to properly expose this to the user.
 * \param program_id The program ID to check.
 * \return Whether or not the program requires firmware to run properly.
 */
inline constexpr bool GameRequiresFirmware(u64 program_id)
{
    return std::find(FIRMWARE_REQUIRED_GAMES.begin(), FIRMWARE_REQUIRED_GAMES.end(), program_id)
           != FIRMWARE_REQUIRED_GAMES.end();
}

enum FirmwareCheckResult {
    FirmwareGood,
    ErrorFirmwareMissing,
    ErrorFirmwareCorrupted,
};

/**
 * \brief Checks for installed firmware within the system.
 * \param system The system to check for firmware.
 * \return Whether or not the system has installed firmware.
 */
inline bool CheckFirmwarePresence(Core::System &system)
{
    constexpr u64 MiiEditId = static_cast<u64>(Service::AM::AppletProgramId::MiiEdit);

    auto bis_system = system.GetFileSystemController().GetSystemNANDContents();
    if (!bis_system) {
        return false;
    }

    auto mii_applet_nca = bis_system->GetEntry(MiiEditId, FileSys::ContentRecordType::Program);

    if (!mii_applet_nca) {
        return false;
    }

    return true;
}

/**
 * \brief Verifies if firmware is properly installed and is in the correct version range.
 * \param system The system to check firmware on.
 * \return A result code defining the status of the system's firmware.
 */
FirmwareCheckResult VerifyFirmware(Core::System &system);

/**
 * @brief Get the currently installed firmware version.
 * @param system The system to check firmware on.
 * @return A pair of the firmware version format and result code.
 */
inline std::pair<Service::Set::FirmwareVersionFormat, Result> GetFirmwareVersion(Core::System &system)
{
    Service::Set::FirmwareVersionFormat firmware_data{};
    const auto result
        = Service::Set::GetFirmwareVersionImpl(firmware_data,
                                               system,
                                               Service::Set::GetFirmwareVersionType::Version2);

    return {firmware_data, result};
}

// TODO(crueter): GET AS STRING

/// The firmware Harbor is developed against. Others may be installed, with a warning.
inline constexpr std::string_view HarborFirmwareVersion = "23.0.1";

/// The HOME menu program, which a console session boots.
inline constexpr u64 QLaunchProgramId = 0x0100000000001000ULL;

struct FirmwareProgress {
    std::string stage;
    std::optional<double> fraction; ///< empty while the length of the step is unknown
};

/// Called while firmware is copied; returning false cancels.
using FirmwareProgressCallback = std::function<bool(const FirmwareProgress&)>;

struct FirmwareResult {
    bool ok = false;
    std::string version; ///< the display version, e.g. "23.0.1"
    std::string error;   ///< why it failed, in words for the user
};

/// What is installed in the system NAND, read without a Core::System (before a frontend starts one).
struct InstalledFirmware {
    bool present = false;     ///< any firmware content is installed
    bool has_qlaunch = false; ///< the HOME menu program is there and readable with the installed keys
    std::string version;      ///< empty when the system version could not be read
};

/**
 * @brief Copies the firmware at `source` into a staging folder next to the NAND and checks it there:
 * readable with the installed keys, with a system version and the HOME menu program (qlaunch).
 * The installed firmware is not touched.
 * @param source A .zip (a path, or a content URI on Android) or a folder of .nca files.
 */
FirmwareResult StageFirmware(const std::string& source, const FirmwareProgressCallback& progress);

/// Replaces the installed firmware with the staged one (a rename, so it is quick and all or nothing).
bool CommitStagedFirmware(std::string* error);

/// Deletes the staged copy, if any.
void DiscardStagedFirmware();

/**
 * @brief Installs the firmware at `source`: StageFirmware, then CommitStagedFirmware. The installed
 * firmware is replaced only when the new one checked out. A running Core::System must rescan its NAND
 * afterwards (FileSystemController::CreateFactories).
 */
FirmwareResult InstallFirmware(const std::string& source, const FirmwareProgressCallback& progress);

/// The installed firmware's version and whether qlaunch is there.
InstalledFirmware InspectInstalledFirmware();

} // namespace FirmwareManager

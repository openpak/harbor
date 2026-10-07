// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "harbor_setup/device_check.h"

namespace HarborSetup {

/// One downloadable driver package from a publisher's GitHub releases.
struct DriverRelease {
    std::string publisher; ///< the GitHub owner, e.g. "MrPurple666"
    std::string title;     ///< the release name
    std::string file_name; ///< the asset, a .zip
    std::string download_url;
    std::uint64_t size = 0;
    std::optional<std::string> digest; ///< "sha256:<64 hex>" when GitHub publishes one
    std::string date;                  ///< yyyy-mm-dd
    std::string channel;               ///< "Turnip Unified", "Turnip Stable", "Turnip" or "Qualcomm"
    bool recommended = false;

    std::string Detail() const;
};

struct DriverListing {
    std::vector<DriverRelease> releases;
    std::optional<std::string> notice; ///< per-publisher errors, or why nothing can be offered
};

inline constexpr std::uint64_t MaxDriverDownload = 32ULL * 1024 * 1024;
inline constexpr std::uint64_t MaxReleasesResponse = 2ULL * 1024 * 1024;

/// The publishers whose public GitHub releases the driver page lists.
inline constexpr std::array<const char*, 4> DriverRepositories = {
    "MrPurple666/purple-turnip",
    "whitebelyash/AdrenoToolsDrivers",
    "K11MCH1/AdrenoToolsDrivers",
    "StevenMXZ/Adreno-Tools-Drivers",
};

/// The Adreno model in the GPU's name ("Adreno (TM) 650" -> 650), or 0.
int AdrenoModel(const DeviceInfo& device);

/// Whether online downloads can be offered: a reviewed Adreno model, Android 9+ and arm64.
bool CanBrowseDrivers(const DeviceInfo& device);

std::string ReleasesUrl(const std::string& repository);

/// The packages in one publisher's releases JSON (the GitHub API's /releases list) that suit this device,
/// newest first. Malformed JSON gives an empty list and an error.
std::vector<DriverRelease> ParseDriverReleases(const std::string& json, const std::string& repository,
                                               const DeviceInfo& device, std::string* error = nullptr);

/// Puts the listing in the order the page shows it and marks the recommended package: on an Adreno 6xx,
/// MrPurple's Turnip T30, the build tested on the owner's Adreno 650.
void RankDriverListing(DriverListing& listing, const DeviceInfo& device);

/// Only https://github.com/<repository>/releases/download/... is downloaded.
bool AllowedDriverDownload(const std::string& url, const std::string& repository);
bool AllowedDriverDownload(const std::string& url);

/// The Adreno generation a driver library is built for, from its name ("vulkan.ad07xx.so" -> 7), if any.
std::optional<int> LibraryFamily(const std::string& library_name);

/// Why a driver package must not be installed on this device, or an empty string. Checks the Android
/// version it needs and the Adreno generation its library is named for.
std::string DriverPackageProblem(const std::string& name, const std::string& library_name, int min_api,
                                 const DeviceInfo& device);

} // namespace HarborSetup

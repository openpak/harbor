// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>

#include "common/logging.h"
#include "harbor_setup/driver_catalog.h"
#include "harbor_setup/driver_recovery.h"
#include "harbor_setup/host.h"

namespace HarborSetup {

namespace {

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string Megabytes(std::uint64_t bytes) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f", static_cast<double>(bytes) / 1048576.0);
    return text;
}

void Remove(const std::string& path) {
    if (!path.empty()) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
}

} // namespace

std::string DriverDownloadLead(const DeviceInfo& device) {
    return CanBrowseDrivers(device)
               ? "Choose from recent Turnip and Qualcomm releases on GitHub. Packages are filtered for "
                 "your GPU; Android requirements are checked before installation. You can always "
                 "switch back to the phone's driver."
               : "Online downloads are not available for this GPU or Android version. You can still "
                 "install a compatible AdrenoTools driver file.";
}

DriverListing FetchDriverListing(Host& host, const DeviceInfo& device, const std::atomic_bool& cancel) {
    DriverListing listing;
    if (!CanBrowseDrivers(device)) {
        // A non-Adreno or unknown device never contacts GitHub (G4.GPU.11).
        listing.notice = "Online downloads are not available for this GPU or Android version.";
        return listing;
    }
    std::vector<std::string> errors;
    for (const char* repository : DriverRepositories) {
        if (cancel) {
            break;
        }
        std::string error;
        const auto json = host.HttpGet(ReleasesUrl(repository), MaxReleasesResponse, cancel, &error);
        const std::string owner = std::string(repository).substr(0, std::string(repository).find('/'));
        if (!json) {
            LOG_WARNING(Frontend, "harbor: driver listing from {} failed: {}", repository, error);
            errors.push_back(owner + ": " + error);
            continue;
        }
        auto releases = ParseDriverReleases(*json, repository, device, &error);
        if (!error.empty()) {
            errors.push_back(owner + ": " + error);
        }
        listing.releases.insert(listing.releases.end(), releases.begin(), releases.end());
    }
    RankDriverListing(listing, device);
    if (!errors.empty()) {
        std::string notice;
        for (const auto& e : errors) {
            notice += (notice.empty() ? "" : " ") + e + (e.ends_with('.') ? "" : ".");
        }
        listing.notice = notice;
    }
    LOG_INFO(Frontend, "harbor: driver listing: {} packages fit this device", listing.releases.size());
    return listing;
}

DriverInstall InstallDriverPackage(Host& host, const DeviceInfo& device, const std::string& location,
                                   const std::string& file_name) {
    const auto package = host.InspectDriver(location);
    if (!package.error.empty()) {
        return {.error = package.error};
    }
    if (const auto problem = DriverPackageProblem(package.name, package.library_name, package.min_api, device);
        !problem.empty()) {
        host.DiscardDriver(package);
        LOG_WARNING(Frontend, "harbor: driver {} refused: {}", file_name, problem);
        return {.error = problem};
    }
    std::string error;
    const auto installed = host.CommitDriver(package, &error);
    if (!installed) {
        return {.error = error.empty() ? "The driver could not be installed." : error};
    }
    LOG_INFO(Frontend, "harbor: driver {} installed", installed->id);
    return {.installed = installed};
}

std::string DownloadProgressText(const DriverRelease& release, std::uint64_t received) {
    return "Downloading " + release.file_name + " (" + Megabytes(received) + " / " + Megabytes(release.size) +
           " MB)";
}

DriverInstall DownloadDriver(Host& host, const DeviceInfo& device, const DriverRelease& release,
                             const std::function<void(std::uint64_t)>& progress,
                             const std::function<void()>& installing, const std::atomic_bool& cancel) {
    if (!AllowedDriverDownload(release.download_url) || release.size == 0 || release.size > MaxDriverDownload) {
        return {.error = "This is not a supported GitHub driver download."};
    }
    LOG_INFO(Frontend, "harbor: downloading driver {}", release.file_name);
    const auto result = host.Download(release.download_url, release.size, progress, cancel);
    if (cancel) {
        Remove(result.path);
        LOG_INFO(Frontend, "harbor: driver download cancelled");
        return {.error = "Download cancelled. Your selected driver is unchanged.", .cancelled = true};
    }
    if (!result.error.empty()) {
        Remove(result.path);
        return {.error = "The driver could not be downloaded: " + result.error};
    }
    if (result.size != release.size) {
        Remove(result.path);
        return {.error = "The download was incomplete. Try again."};
    }
    if (release.digest && Lower(result.sha256) != Lower(release.digest->substr(7))) {
        Remove(result.path);
        return {.error = "The download checksum did not match GitHub's release asset. Try again."};
    }
    if (installing) {
        installing();
    }
    auto installed = InstallDriverPackage(host, device, result.path, release.file_name);
    Remove(result.path);
    return installed;
}

bool HasRecommendedDriver(const DeviceInfo& device) {
    return AdrenoModel(device) / 100 == 6 && CanBrowseDrivers(device);
}

RecoveryResult InstallRecommendedDriver(Host& host, const DeviceInfo& device, const std::atomic_bool& cancel) {
    if (!HasRecommendedDriver(device)) {
        return {.error = "There is no recommended driver for this GPU."};
    }
    const auto listing = FetchDriverListing(host, device, cancel);
    if (cancel) {
        return {.error = "Cancelled."};
    }
    const auto recommended = std::find_if(listing.releases.begin(), listing.releases.end(),
                                          [](const DriverRelease& release) { return release.recommended; });
    if (recommended == listing.releases.end()) {
        return {.error = "The recommended driver could not be found on GitHub. Try again later."};
    }
    LOG_INFO(Frontend, "harbor: downloading the recommended driver {}", recommended->file_name);
    const auto result = DownloadDriver(host, device, *recommended, [](std::uint64_t) {}, {}, cancel);
    if (!result.installed) {
        return {.error = result.error};
    }
    std::string error;
    if (!host.SelectDriver(result.installed->id, &error)) {
        return {.error = error.empty() ? "The driver choice could not be saved." : error};
    }
    LOG_INFO(Frontend, "harbor: recommended driver {} installed and chosen", result.installed->id);
    return {.id = result.installed->id, .name = result.installed->name};
}

} // namespace HarborSetup

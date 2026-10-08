// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <algorithm>
#include <cctype>
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

} // namespace

bool HasRecommendedDriver(const DeviceInfo& device) {
    return AdrenoModel(device) / 100 == 6 && CanBrowseDrivers(device);
}

RecoveryResult InstallRecommendedDriver(Host& host, const DeviceInfo& device, const std::atomic_bool& cancel) {
    if (!HasRecommendedDriver(device)) {
        return {.error = "There is no recommended driver for this GPU."};
    }
    DriverListing listing;
    for (const char* repository : DriverRepositories) {
        std::string error;
        const auto json = host.HttpGet(ReleasesUrl(repository), MaxReleasesResponse, cancel, &error);
        if (cancel) {
            return {.error = "Cancelled."};
        }
        if (!json) {
            LOG_WARNING(Frontend, "harbor: driver listing from {} failed: {}", repository, error);
            continue;
        }
        auto releases = ParseDriverReleases(*json, repository, device);
        listing.releases.insert(listing.releases.end(), releases.begin(), releases.end());
    }
    RankDriverListing(listing, device);
    const auto recommended = std::find_if(listing.releases.begin(), listing.releases.end(),
                                          [](const DriverRelease& release) { return release.recommended; });
    if (recommended == listing.releases.end()) {
        return {.error = "The recommended driver could not be found on GitHub. Try again later."};
    }
    const DriverRelease release = *recommended;
    if (!AllowedDriverDownload(release.download_url) || release.size == 0 ||
        release.size > MaxDriverDownload) {
        return {.error = "This is not a supported GitHub driver download."};
    }
    LOG_INFO(Frontend, "harbor: downloading the recommended driver {}", release.file_name);
    const auto download = host.Download(release.download_url, release.size, [](std::uint64_t) {}, cancel);
    const auto cleanup = [&] {
        std::error_code ec;
        std::filesystem::remove(download.path, ec);
    };
    if (!download.error.empty()) {
        cleanup();
        return {.error = "The driver could not be downloaded: " + download.error};
    }
    if (download.size != release.size ||
        (release.digest && Lower(download.sha256) != Lower(release.digest->substr(7)))) {
        cleanup();
        return {.error = "The download did not match GitHub's release asset. Try again."};
    }
    const auto package = host.InspectDriver(download.path);
    cleanup();
    if (!package.error.empty()) {
        return {.error = package.error};
    }
    if (const auto problem = DriverPackageProblem(package.name, package.library_name, package.min_api, device);
        !problem.empty()) {
        host.DiscardDriver(package);
        return {.error = problem};
    }
    std::string error;
    const auto installed = host.CommitDriver(package, &error);
    if (!installed) {
        return {.error = error.empty() ? "The driver could not be installed." : error};
    }
    if (!host.SelectDriver(installed->id, &error)) {
        return {.error = error.empty() ? "The driver choice could not be saved." : error};
    }
    LOG_INFO(Frontend, "harbor: recommended driver {} installed and chosen", installed->id);
    return {.id = installed->id, .name = installed->name};
}

} // namespace HarborSetup

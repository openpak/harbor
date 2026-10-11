// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Graphics driver downloads and installs through a Host's driver store and network: one
// implementation for the setup's driver page, the Harbor menu's, the stopped screen's and the
// recovery after a driver failed to load. The catalogue itself (publishers, filters, ranking) is
// driver_catalog.h.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "harbor_setup/device_check.h"
#include "harbor_setup/driver_catalog.h"
#include "harbor_setup/host.h"

namespace HarborSetup {

/// What "Browse GitHub drivers" explains: the catalogue, or why it is not offered here.
std::string DriverDownloadLead(const DeviceInfo& device);

/// Every publisher's releases that suit this device, ranked (RankDriverListing). A publisher that
/// fails is named in `notice` and the others still list. Blocking; not on a UI thread.
DriverListing FetchDriverListing(Host& host, const DeviceInfo& device, const std::atomic_bool& cancel);

/// A driver package put into the store, or why not.
struct DriverInstall {
    std::optional<InstalledDriver> installed;
    std::string error;
    bool cancelled = false; ///< the user cancelled: the selection is unchanged
};

/// Reads a package (a picked .zip or a download), refuses one that does not fit this device
/// (DriverPackageProblem) and commits it to the store. Does not select it. Blocking.
DriverInstall InstallDriverPackage(Host& host, const DeviceInfo& device, const std::string& location,
                                   const std::string& file_name);

/// "Downloading <file> (1.2 / 9.8 MB)".
std::string DownloadProgressText(const DriverRelease& release, std::uint64_t received);

/// Downloads one listed release and installs it: only a GitHub release asset of the four
/// publishers, at most MaxDriverDownload, the exact length, and the SHA-256 GitHub publishes.
/// `progress` gets the bytes received; `installing` runs once the download checked out, after
/// which cancelling has no effect. Does not select the driver. Blocking.
DriverInstall DownloadDriver(Host& host, const DeviceInfo& device, const DriverRelease& release,
                             const std::function<void(std::uint64_t)>& progress,
                             const std::function<void()>& installing, const std::atomic_bool& cancel);

/// The outcome of installing the recommended driver: the store id and name, or why not.
struct RecoveryResult {
    std::string id;
    std::string name;
    std::string error;
};

/// Whether a recommended driver exists for this GPU (MrPurple's Turnip T30 on an Adreno 6xx).
bool HasRecommendedDriver(const DeviceInfo& device);

/// After the chosen driver failed to load: fetches the publishers' GitHub listings, downloads the
/// recommended package (checked as the setup's driver page checks it), installs it into the
/// store and selects it. Blocking; not on a UI thread.
RecoveryResult InstallRecommendedDriver(Host& host, const DeviceInfo& device, const std::atomic_bool& cancel);

} // namespace HarborSetup

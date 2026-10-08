// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <atomic>
#include <string>

#include "harbor_setup/device_check.h"

namespace HarborSetup {

class Host;

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

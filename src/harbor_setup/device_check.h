// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <optional>
#include <string>
#include <vector>

namespace HarborSetup {

/// What the host measured for the device check. An empty optional was not measured, and its row is left
/// out (the desktop reports no Vulkan, Android version or driver choice).
struct DeviceInfo {
    std::optional<std::vector<std::string>> abis; ///< best first: "arm64-v8a", or the desktop's "x64"
    std::optional<long long> total_memory;        ///< bytes the system reports
    std::optional<int> vulkan_major, vulkan_minor; ///< 0.0 = no Vulkan
    std::optional<std::string> gpu;               ///< the GPU's name as its driver reports it
    std::optional<bool> adreno;                   ///< whether Turnip drivers apply (Android only)
    std::optional<int> android_api;
    std::optional<std::string> android_release;
    std::optional<long long> free_storage; ///< bytes free where the data directory is
};

enum class CheckLevel { Pass, Warn, Fail };

struct DeviceCheckRow {
    std::string title, value, reason;
    CheckLevel level;
};

/// Whether this device can run the console. Fail is only for what stops Harbor outright; Warn is for what
/// makes it slow or limited. Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).
std::vector<DeviceCheckRow> RunDeviceCheck(const DeviceInfo& device);

/// The installed memory size that `reported` bytes come from: the smallest common size that holds them.
int InstalledGb(long long reported);

/// Measures this device: every value is read from the running system. The Vulkan probe runs on Android
/// (and on desktop only when `probe_vulkan`).
DeviceInfo ProbeDevice(const std::string& data_dir, bool probe_vulkan);

/// The design's sample devices ("phone", "warn", "blocked", "desktop"), for comparing with the renders.
DeviceInfo SampleDevice(const std::string& name);

} // namespace HarborSetup

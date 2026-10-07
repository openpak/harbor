// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// The device check's thresholds and strings. Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx
// base), DeviceCheck.cs.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include "harbor_setup/device_check.h"

namespace HarborSetup {

namespace {

constexpr int MinMemoryGb = 4;
constexpr int TightMemoryGb = 6;
constexpr int MinVulkanMajor = 1;
constexpr int MinVulkanMinor = 1;
constexpr int MinAndroidApi = 30;
constexpr long long LowStorage = 4LL << 30;

const std::map<std::string, std::string> supported_abis = {
    {"arm64-v8a", "64-bit ARM"},
    {"arm64", "64-bit ARM"},
    {"x64", "x86-64"},
};

constexpr int memory_sizes[] = {1, 2, 3, 4, 6, 8, 10, 12, 16, 18, 20, 24, 32, 48, 64, 96, 128};

std::string Gigabytes(long long bytes) {
    const double gb = static_cast<double>(bytes) / static_cast<double>(1LL << 30);
    char text[32];
    std::snprintf(text, sizeof(text), gb < 10 ? "%.1f GB" : "%.0f GB", gb);
    return text;
}

} // namespace

int InstalledGb(long long reported) {
    const double gib = static_cast<double>(reported) / static_cast<double>(1LL << 30);
    for (const int size : memory_sizes) {
        if (size >= gib - 0.001) {
            return size;
        }
    }
    return static_cast<int>(std::ceil(gib));
}

std::vector<DeviceCheckRow> RunDeviceCheck(const DeviceInfo& d) {
    std::vector<DeviceCheckRow> rows;

    if (d.abis) {
        const auto it = std::find_if(d.abis->begin(), d.abis->end(),
                                     [](const std::string& abi) { return supported_abis.contains(abi); });
        if (it != d.abis->end()) {
            rows.push_back({"Processor", supported_abis.at(*it), "The console runs on this processor.",
                            CheckLevel::Pass});
        } else {
            rows.push_back({"Processor", d.abis->empty() ? "Unknown" : d.abis->front(),
                            "Harbor needs a 64-bit ARM processor.", CheckLevel::Fail});
        }
    }

    if (d.total_memory) {
        const int gb = InstalledGb(*d.total_memory);
        const std::string value = std::to_string(gb) + " GB";
        if (gb < MinMemoryGb) {
            rows.push_back({"Memory", value,
                            "Harbor needs at least " + std::to_string(MinMemoryGb) + " GB of memory.",
                            CheckLevel::Fail});
        } else if (gb <= TightMemoryGb) {
            rows.push_back({"Memory", value, "Enough to start; large games may be closed.",
                            CheckLevel::Warn});
        } else {
            rows.push_back({"Memory", value, "Enough for the console and its games.", CheckLevel::Pass});
        }
    }

    if (d.vulkan_major && d.vulkan_minor) {
        const std::string gpu = d.gpu.value_or("This GPU");
        const std::string minimum =
            std::to_string(MinVulkanMajor) + "." + std::to_string(MinVulkanMinor);
        const std::string version = std::to_string(*d.vulkan_major) + "." + std::to_string(*d.vulkan_minor);
        const bool none = *d.vulkan_major == 0 && *d.vulkan_minor == 0;
        const bool old = *d.vulkan_major < MinVulkanMajor ||
                         (*d.vulkan_major == MinVulkanMajor && *d.vulkan_minor < MinVulkanMinor);
        if (none) {
            rows.push_back({"Vulkan", "None",
                            "The console needs Vulkan " + minimum + " or later; this GPU has none.",
                            CheckLevel::Fail});
        } else if (old) {
            rows.push_back({"Vulkan", version,
                            gpu + ": the console needs Vulkan " + minimum + " or later.",
                            CheckLevel::Fail});
        } else {
            rows.push_back({"Vulkan", version, gpu + " can draw the console.", CheckLevel::Pass});
        }
    }

    if (d.adreno) {
        if (*d.adreno) {
            rows.push_back({"Graphics driver", "Phone's or Turnip",
                            "Turnip drivers can be installed for this Adreno GPU.", CheckLevel::Pass});
        } else {
            rows.push_back({"Graphics driver", "Phone's only", "Turnip drivers are for Adreno GPUs only.",
                            CheckLevel::Warn});
        }
    }

    if (d.android_api) {
        const std::string name =
            "Android " + d.android_release.value_or("API " + std::to_string(*d.android_api));
        if (*d.android_api >= MinAndroidApi) {
            rows.push_back({"System", name, "A version Harbor is built for.", CheckLevel::Pass});
        } else {
            rows.push_back({"System", name, "Harbor is built for Android 11 or later.", CheckLevel::Warn});
        }
    }

    if (d.free_storage) {
        const std::string value = Gigabytes(*d.free_storage) + " free";
        if (*d.free_storage >= LowStorage) {
            rows.push_back({"Storage", value, "Room for the firmware, saves and caches.", CheckLevel::Pass});
        } else {
            rows.push_back({"Storage", value,
                            "The firmware, saves and caches need about " +
                                std::to_string(LowStorage >> 30) + " GB.",
                            CheckLevel::Warn});
        }
    }

    return rows;
}

DeviceInfo SampleDevice(const std::string& name) {
    DeviceInfo d;
    if (name == "desktop") {
        d.abis = std::vector<std::string>{"x64"};
        d.total_memory = 32LL << 30;
        d.free_storage = 420LL << 30;
        return d;
    }
    d.abis = std::vector<std::string>{"arm64-v8a", "armeabi-v7a"};
    d.total_memory = 7600000000LL;
    d.vulkan_major = 1;
    d.vulkan_minor = 3;
    d.gpu = "Adreno (TM) 740";
    d.adreno = true;
    d.android_api = 34;
    d.android_release = "14";
    d.free_storage = 96LL << 30;
    if (name == "warn") {
        d.total_memory = 5800000000LL;
        d.vulkan_minor = 1;
        d.gpu = "Mali-G610 MC6";
        d.adreno = false;
        d.free_storage = 1932735283LL; // 1.8 GB
    } else if (name == "blocked") {
        d.abis = std::vector<std::string>{"armeabi-v7a", "armeabi"};
        d.total_memory = 2900000000LL;
        d.vulkan_minor = 0;
        d.gpu = "Mali-T830";
        d.adreno = false;
    }
    return d;
}

} // namespace HarborSetup

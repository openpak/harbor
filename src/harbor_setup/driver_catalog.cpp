// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Which public GitHub driver releases suit this phone. Behaviour follows the Ryujinx Harbor
// (MIT-licensed Ryujinx base), GpuDriverCatalog.cs, plus the KIMCHI rule below.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <regex>

#include <nlohmann/json.hpp>

#include "harbor_setup/driver_catalog.h"

namespace HarborSetup {

namespace {

constexpr const char* Purple = "MrPurple666/purple-turnip";
constexpr const char* Whitebelyash = "whitebelyash/AdrenoToolsDrivers";
constexpr const char* Kimchi = "K11MCH1/AdrenoToolsDrivers";
constexpr const char* Steven = "StevenMXZ/Adreno-Tools-Drivers";

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool Contains(const std::string& text, const std::string& part) {
    return Lower(text).find(Lower(part)) != std::string::npos;
}

bool EndsWith(const std::string& text, const std::string& suffix) {
    const auto lower = Lower(text);
    return lower.size() >= suffix.size() &&
           lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool IsSha256Digest(const std::string& digest) {
    static const std::regex pattern("^sha256:[0-9a-fA-F]{64}$");
    return std::regex_match(digest, pattern);
}

std::string Text(const nlohmann::json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

bool SupportsAsset(const std::string& repository, const std::string& body, const std::string& asset,
                   int model, bool qualcomm) {
    const int family = model / 100;
    if (qualcomm) {
        // Qualcomm blobs vary by generation. An ambiguous "maybe a6xx" is not a support claim.
        const std::string lower = Lower(body);
        const std::string generation = "a" + std::to_string(family) + "xx";
        const std::regex maybe("(?:maybe|possibly)[^\\r\\n]*" + generation);
        return lower.find(generation) != std::string::npos && !std::regex_search(lower, maybe);
    }

    const bool gen8 = Contains(asset, "a8xx") || Contains(asset, "gen8");
    if (repository == Kimchi || repository == Steven) {
        if ((family == 8) != gen8) {
            return false;
        }
    } else if (gen8 && family != 8) {
        return false;
    }

    // KIMCHI's Turnip builds ship vulkan.ad07xx.so, an Adreno 7xx library (checked 2026-10-07 on
    // v25.3.0 R11 and v26.0.0 R7). Eden loaded R11 on an Adreno 650, fell back to the phone's driver
    // without a word and crashed, so they are offered to 7xx GPUs only.
    if (repository == Kimchi && !gen8 && family != 7) {
        return false;
    }

    if (model == 710 || model == 720 || model == 722) {
        return repository == Steven && Contains(body, "now supported on Upstream");
    }

    return true;
}

} // namespace

std::string DriverRelease::Detail() const {
    char size_text[32];
    std::snprintf(size_text, sizeof(size_text), "%.1f MB", static_cast<double>(size) / 1048576.0);
    std::string detail = publisher + " · " + channel + " · " + date + " · " + size_text;
    return recommended ? "Recommended for this GPU · " + detail : detail;
}

int AdrenoModel(const DeviceInfo& device) {
    static const std::regex pattern("Adreno\\s*(?:\\(TM\\)\\s*)?(\\d{3})L?\\b", std::regex::icase);
    std::smatch match;
    const std::string gpu = device.gpu.value_or("");
    if (!std::regex_search(gpu, match, pattern)) {
        return 0;
    }
    return std::stoi(match[1].str());
}

bool SupportsXclipseDrivers(const DeviceInfo& device) {
    const bool arm64 = device.abis && std::find(device.abis->begin(), device.abis->end(),
                                               "arm64-v8a") != device.abis->end();
    if (!arm64 || device.android_api.value_or(0) < 34) {
        return false;
    }
    static const std::regex stock(R"(\bXclipse\s+(530|540|550|920|940|950|960)\b)", std::regex::icase);
    static const std::regex radv(
        R"(\bRADV\s+(VANGOGHLITE|TITAN|MAGELLAN(?:\s+CARL)?|ARIEL|TRITON|GALILEO|JUNO)\b)",
        std::regex::icase);
    const auto gpu = device.gpu.value_or("");
    return std::regex_search(gpu, stock) || std::regex_search(gpu, radv);
}

bool CanBrowseDrivers(const DeviceInfo& device) {
    if (SupportsXclipseDrivers(device)) {
        return true;
    }
    const int model = AdrenoModel(device);
    // Unknown and new chips stay manual installs until their publishers' support is reviewed.
    static const int reviewed[] = {710, 720, 722, 730, 735, 740, 750, 760, 810, 812, 825, 829, 830, 840};
    const bool supported = (model >= 610 && model < 700) ||
                           std::find(std::begin(reviewed), std::end(reviewed), model) != std::end(reviewed);
    const bool arm64 = device.abis && std::find(device.abis->begin(), device.abis->end(),
                                                "arm64-v8a") != device.abis->end();
    return supported && device.adreno.value_or(false) && device.android_api.value_or(0) >= 28 && arm64;
}

std::string ReleasesUrl(const std::string& repository) {
    return "https://api.github.com/repos/" + repository + "/releases?per_page=20";
}

bool AllowedDriverDownload(const std::string& url, const std::string& repository) {
    const std::string prefix = "https://github.com/" + repository + "/releases/download/";
    if (url.rfind(prefix, 0) != 0) {
        return false;
    }
    // No user info, port, query, fragment or path tricks after the fixed prefix.
    const std::string rest = url.substr(prefix.size());
    return rest.find_first_of("?#@\\") == std::string::npos && rest.find("..") == std::string::npos;
}

bool AllowedDriverDownload(const std::string& url) {
    return std::any_of(DriverRepositories.begin(), DriverRepositories.end(),
                       [&](const char* repository) { return AllowedDriverDownload(url, repository); });
}

std::vector<DriverRelease> ParseDriverReleases(const std::string& json, const std::string& repository,
                                               const DeviceInfo& device, std::string* error) {
    std::vector<DriverRelease> chosen;
    const bool xclipse = SupportsXclipseDrivers(device);
    const bool radv = repository == "JimVulkan/radv-xclipse";
    if (xclipse != radv || (radv && !CanBrowseDrivers(device))) {
        return chosen;
    }
    nlohmann::json releases;
    try {
        releases = nlohmann::json::parse(json);
    } catch (const std::exception& e) {
        if (error) {
            *error = e.what();
        }
        return chosen;
    }
    if (!releases.is_array()) {
        if (error) {
            *error = "GitHub answered with something other than a release list";
        }
        return chosen;
    }

    std::vector<const nlohmann::json*> ordered;
    for (const auto& release : releases) {
        if (release.is_object() && !release.value("draft", false) &&
            !release.value("prerelease", false)) {
            ordered.push_back(&release);
        }
    }
    // ISO 8601 times sort as text.
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) {
        return Text(*a, "published_at") > Text(*b, "published_at");
    });

    const int model = AdrenoModel(device);
    const int api = device.android_api.value_or(0);
    const bool purple = repository == Purple;
    std::map<std::string, int> channels;

    for (const auto* release : ordered) {
        const std::string name = Text(*release, "name");
        const std::string tag = Text(*release, "tag_name");
        const std::string body = Text(*release, "body");
        // 1.7 is the first release covering all seven reviewed Xclipse GPUs.
        if (radv) {
            std::smatch version;
            static const std::regex pattern(R"(^v?(\d+)\.(\d+)\.)");
            if (!std::regex_search(tag, version, pattern) ||
                (std::stoi(version[1].str()) < 1) ||
                (std::stoi(version[1].str()) == 1 && std::stoi(version[2].str()) < 7)) {
                continue;
            }
        }
        if (purple && api < 30) {
            continue; // MrPurple's release notes require Android 11+.
        }
        // This release reports broken Switch rendering, despite being marked non-prerelease.
        if (repository == Kimchi && tag == "v26.0.0-rc08") {
            continue;
        }
        const auto assets = release->find("assets");
        if (assets == release->end() || !assets->is_array()) {
            continue;
        }

        for (const auto& asset : *assets) {
            if (!asset.is_object()) {
                continue;
            }
            const std::string file = Text(asset, "name");
            const std::string url = Text(asset, "browser_download_url");
            const auto size_it = asset.find("size");
            const std::int64_t size =
                size_it != asset.end() && size_it->is_number_integer() ? size_it->get<std::int64_t>() : 0;
            const auto digest_it = asset.find("digest");
            std::optional<std::string> digest;
            if (digest_it != asset.end() && digest_it->is_string()) {
                digest = digest_it->get<std::string>();
            }

            const bool qualcomm = Contains(name, "Qualcomm");
            const bool turnip = Contains(file, "turnip");
            int minimum_api = 0;
            if (repository == Steven && tag == "v863.1_a7xx") {
                minimum_api = 32;
            } else if (repository == Steven && tag == "v891.7") {
                minimum_api = 36;
            } else if (repository == Kimchi && (tag == "v840" || tag == "v837")) {
                minimum_api = 35;
            }

            if ((!radv && !qualcomm && !turnip) ||
                (radv && (!file.starts_with("radv-xclipse-") || Contains(file, "wayland"))) ||
                !EndsWith(file, ".zip") || api < minimum_api || size <= 0 ||
                static_cast<std::uint64_t>(size) > MaxDriverDownload ||
                !AllowedDriverDownload(url, repository) || (digest && !IsSha256Digest(*digest)) ||
                (!radv && !SupportsAsset(repository, body, file, model, qualcomm))) {
                continue;
            }

            const std::string channel = radv ? "RADV Xclipse" : qualcomm ? "Qualcomm"
                                        : purple ? "Turnip Unified"
                                        : Contains(name, "Stable") ? "Turnip Stable"
                                                                   : "Turnip";
            // Keep several variants and recent alternatives, not an endless release history.
            const int limit = repository == Whitebelyash ? 2 : 4;
            if (channels[channel] >= limit) {
                continue;
            }

            const auto slash = repository.find('/');
            chosen.push_back(DriverRelease{
                .publisher = repository.substr(0, slash),
                .title = name.empty() ? tag : name,
                .file_name = file,
                .download_url = url,
                .size = static_cast<std::uint64_t>(size),
                .digest = digest,
                .date = Text(*release, "published_at").substr(0, 10),
                .channel = channel,
            });
            channels[channel]++;
        }
    }
    return chosen;
}

void RankDriverListing(DriverListing& listing, const DeviceInfo& device) {
    if (AdrenoModel(device) / 100 != 6) {
        return;
    }
    for (auto& release : listing.releases) {
        release.recommended = release.publisher == "MrPurple666" &&
                              Contains(release.file_name, "_T30-") && EndsWith(release.file_name, ".zip");
    }
    std::stable_partition(listing.releases.begin(), listing.releases.end(),
                          [](const DriverRelease& release) { return release.recommended; });
}

std::optional<int> LibraryFamily(const std::string& library_name) {
    static const std::regex pattern("ad0?([5-9])xx", std::regex::icase);
    std::smatch match;
    if (!std::regex_search(library_name, match, pattern)) {
        return std::nullopt;
    }
    return match[1].str()[0] - '0';
}

std::string DriverPackageProblem(const std::string& name, const std::string& library_name, int min_api,
                                 const DeviceInfo& device) {
    if (device.android_api && min_api > *device.android_api) {
        return name + " requires Android API " + std::to_string(min_api) + "; this phone has API " +
               std::to_string(*device.android_api) + ". Choose another driver.";
    }
    const bool radv = library_name == "vulkan.radeon.so";
    if (radv && !SupportsXclipseDrivers(device)) {
        return name + " requires a supported Xclipse GPU on Android 14 or newer (ARM64).";
    }
    if (SupportsXclipseDrivers(device) && !radv) {
        return name + " is not a RADV Xclipse driver. Choose an Xclipse driver package.";
    }
    const int family = AdrenoModel(device) / 100;
    if (const auto built_for = LibraryFamily(library_name); built_for && family != 0 && *built_for != family) {
        return name + " is built for Adreno " + std::to_string(*built_for) + "xx GPUs (" + library_name +
               "); this phone has an Adreno " + std::to_string(AdrenoModel(device)) +
               ". The phone's driver would be used instead, so it is not installed.";
    }
    return {};
}

} // namespace HarborSetup

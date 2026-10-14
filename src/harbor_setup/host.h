// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "harbor_setup/device_check.h"
#include "harbor_setup/record.h"

namespace HarborSetup {

/// A file or folder the user picked. `location` is a path, or on Android a content:// URI (a document
/// for files, a tree for folders) that Eden's file layer reads directly.
struct Picked {
    std::string name;     ///< what to show
    std::string location; ///< what to read
    bool is_path = true;
};

using PickCallback = std::function<void(std::optional<Picked>)>;

/// A graphics driver in Eden's driver store (Android).
struct InstalledDriver {
    std::string id; ///< the package's file name in the store
    std::string name;
    std::string detail;
};

/// A driver package copied aside and read, before it goes into the store.
struct DriverPackage {
    std::string staged; ///< where the copy is
    std::string name, version, vendor, library_name;
    int min_api = 0;
    std::string error; ///< set when the package could not be read
};

struct DownloadResult {
    std::string path;   ///< the downloaded file
    std::string sha256; ///< lower-case hex of what was received
    std::uint64_t size = 0;
    std::string error;
};

/// What the wizard needs from the frontend it runs in. Callbacks may arrive on any thread; the wizard
/// moves them to its own. Blocking calls are made from worker threads.
class Host {
public:
    virtual ~Host() = default;

    virtual bool IsAndroid() const = 0;
    virtual DeviceInfo Probe() = 0;

    virtual void PickFile(const std::string& title, const std::vector<std::string>& extensions,
                          PickCallback done) = 0;
    /// Folders the emulator scans for games and add-ons.
    virtual void PickFolder(const std::string& title, PickCallback done) = 0;
    /// Whether firmware may come as a folder of .nca files (needs a real path).
    virtual bool CanPickFirmwareFolder() const {
        return true;
    }

    /// Writes the record's folders where the emulator reads them. Desktop: nothing, the console boot
    /// reads the record.
    virtual bool ApplyFolders(const Record& record, std::string* error) {
        return true;
    }
    /// After firmware was installed: a running emulator core rescans its NAND.
    virtual void FirmwareInstalled() {}
    /// "Start Harbor" was pressed and the completed record saved.
    virtual void Finished(const Record& record) {}

    // Graphics drivers: Android with an Adreno GPU only.
    virtual bool HasDriverStore() const {
        return false;
    }
    virtual std::vector<InstalledDriver> InstalledDrivers() {
        return {};
    }
    /// The driver Eden starts with: an installed id, or empty for the phone's own.
    virtual std::string CurrentDriver() {
        return {};
    }
    virtual DriverPackage InspectDriver(const std::string& location) {
        return {.error = "Drivers cannot be installed here."};
    }
    virtual std::optional<InstalledDriver> CommitDriver(const DriverPackage& package, std::string* error) {
        return std::nullopt;
    }
    virtual void DiscardDriver(const DriverPackage& package) {}
    virtual bool SelectDriver(const std::string& id, std::string* error) {
        return true;
    }
    /// Deletes an installed driver package. The chosen driver is never removed.
    virtual bool RemoveDriver(const std::string& id, std::string* error) {
        return false;
    }
    /// An HTTPS GET of at most `max_bytes`. Empty optional on failure, with `error` set.
    virtual std::optional<std::string> HttpGet(const std::string& url, std::uint64_t max_bytes,
                                               const std::atomic_bool& cancel, std::string* error) {
        if (error) {
            *error = "No network access here.";
        }
        return std::nullopt;
    }
    /// Downloads `url` (at most `max_bytes`) into a temporary file, reporting received bytes.
    virtual DownloadResult Download(const std::string& url, std::uint64_t max_bytes,
                                    const std::function<void(std::uint64_t)>& progress,
                                    const std::atomic_bool& cancel) {
        return {.error = "No network access here."};
    }
};

} // namespace HarborSetup

// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// The setup flow. Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base),
// FirstRunViewModel.cs and FirstRunPages.cs: the same pages, strings, resume rule and record writes.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>

#include "common/logging.h"
#include "frontend_common/firmware_manager.h"
#include "harbor_setup/keys.h"
#include "harbor_setup/wizard.h"

namespace HarborSetup {

namespace {

std::string UrlDecode(const std::string& text) {
    std::string out;
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == '%' && i + 2 < text.size() && std::isxdigit(static_cast<unsigned char>(text[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(text[i + 2]))) {
            out += static_cast<char>(std::stoi(text.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += text[i];
        }
    }
    return out;
}

/// A folder as a person reads it: a path, or for an Android tree URI the place on the phone.
std::string ReadablePath(const std::string& location) {
    if (location.rfind("content://", 0) != 0) {
        return location;
    }
    const auto tree = location.find("/tree/");
    if (tree == std::string::npos) {
        return location;
    }
    auto document = UrlDecode(location.substr(tree + 6));
    if (const auto slash = document.find('/'); slash != std::string::npos) {
        document = document.substr(0, slash);
    }
    const auto colon = document.find(':');
    if (colon == std::string::npos) {
        return document;
    }
    const auto volume = document.substr(0, colon);
    const auto rest = document.substr(colon + 1);
    return (volume == "primary" ? std::string("Internal storage") : volume) + (rest.empty() ? "" : "/" + rest);
}

std::string LastSegment(const std::string& readable) {
    auto trimmed = readable;
    while (trimmed.size() > 1 && (trimmed.back() == '/' || trimmed.back() == '\\')) {
        trimmed.pop_back();
    }
    const auto slash = trimmed.find_last_of("/\\");
    const auto name = slash == std::string::npos ? trimmed : trimmed.substr(slash + 1);
    return name.empty() ? readable : name;
}

std::vector<Folder> ToFolders(const std::optional<std::vector<std::string>>& locations) {
    std::vector<Folder> folders;
    for (const auto& location : locations.value_or(std::vector<std::string>{})) {
        folders.push_back({location, LastSegment(ReadablePath(location)), ReadablePath(location)});
    }
    return folders;
}

std::vector<std::string> ToLocations(const std::vector<Folder>& folders) {
    std::vector<std::string> locations;
    for (const auto& folder : folders) {
        locations.push_back(folder.location);
    }
    return locations;
}

std::string CountFolders(const std::optional<std::vector<std::string>>& folders) {
    if (!folders || folders->empty()) {
        return "None";
    }
    return folders->size() == 1 ? "1 folder" : std::to_string(folders->size()) + " folders";
}

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

} // namespace

Wizard::Wizard(Host& host_, std::filesystem::path record_path_, const Readiness& readiness,
               DeviceInfo device_, Post post_)
    : host{host_}, record_path{std::move(record_path_)}, device{std::move(device_)},
      post{std::move(post_)} {
    setup = LoadRecord(record_path);
    setup.keys_present = readiness.keys_usable;
    setup.firmware_version =
        readiness.FirmwareReady() ? std::optional{readiness.firmware_version} : std::nullopt;

    driver_page = host.HasDriverStore() && device.adreno.value_or(false);
    checks = RunDeviceCheck(device);
    keys_usable = readiness.keys_usable;
    if (readiness.FirmwareReady()) {
        firmware_installed = readiness.firmware_version;
    } else if (readiness.keys_usable && readiness.firmware_present) {
        firmware_problem = readiness.firmware_problem;
    }
    games = ToFolders(setup.game_dirs);
    dlc = ToFolders(setup.dlc_dirs);
    updates = ToFolders(setup.update_dirs);
    if (driver_page) {
        FillDriverChoices(setup.gpu_driver.value_or(host.CurrentDriver()));
    }

    current = FirstIncomplete(readiness, setup, driver_page);
    LOG_INFO(Frontend, "harbor: setup opens at step {}", static_cast<int>(current));
    Enter();
}

Wizard::~Wizard() {
    *alive = false;
    cancel_requested = true;
    if (worker.joinable()) {
        worker.join();
    }
}

Step Wizard::FirstIncomplete(const Readiness& readiness, const Record& record, bool has_driver_page) {
    if (!readiness.keys_usable) {
        return Step::Welcome;
    }
    if (!readiness.FirmwareReady()) {
        return Step::Firmware;
    }
    if (!record.game_dirs || record.game_dirs->empty()) {
        return Step::Games;
    }
    if (!record.dlc_dirs) {
        return Step::Dlc;
    }
    if (!record.update_dirs) {
        return Step::Updates;
    }
    if (has_driver_page && !record.gpu_driver) {
        return Step::Driver;
    }
    return Step::Ready;
}

std::string Wizard::Title() const {
    switch (current) {
    case Step::Welcome:
        return "Welcome";
    case Step::Device:
        return "Device check";
    case Step::Keys:
        return "Keys";
    case Step::Firmware:
        return "Firmware";
    case Step::Games:
        return "Games";
    case Step::Dlc:
        return "DLC";
    case Step::Updates:
        return "Updates";
    case Step::Driver:
        return "Graphics driver";
    case Step::Ready:
        return "Ready";
    }
    return {};
}

std::string Wizard::Lead() const {
    switch (current) {
    case Step::Welcome:
        return "Choose your keys, system software and games to get started.";
    case Step::Device:
        return "Harbor first checks that this device can run the console.";
    case Step::Keys:
        return "Harbor reads the system software and your games with the keys of your own console. "
               "Choose the prod.keys file you dumped from it.";
    case Step::Firmware:
        return host.CanPickFirmwareFolder()
                   ? "Install the system software dumped from your own console: a .zip of its NCA "
                     "files, or a folder of them."
                   : "Install the system software dumped from your own console, as a .zip of its NCA "
                     "files.";
    case Step::Games:
        return "Choose the folders that hold your games. These are your own legally dumped games: "
               "Harbor comes with none and never downloads any.";
    case Step::Dlc:
        return "Optional. Choose the folders that hold DLC for your games. Skip this if you have none.";
    case Step::Updates:
        return "Optional. Choose the folders that hold updates for your games. Skip this if you have "
               "none.";
    case Step::Driver:
        return "Use the phone's Vulkan driver, download an Adreno driver from GitHub, or install a "
               "driver .zip. Set up again from the console's menu (Android back) to change it later.";
    case Step::Ready:
        return "Everything is in place. When Harbor starts, the console boots for the first time and "
               "runs its own setup: language, region, the first user and the network.";
    }
    return {};
}

bool Wizard::CanGoNext() const {
    if (busy) {
        return false;
    }
    switch (current) {
    case Step::Device:
        return std::none_of(checks.begin(), checks.end(),
                            [](const auto& row) { return row.level == CheckLevel::Fail; });
    case Step::Keys:
        return keys_usable;
    case Step::Firmware:
        return !firmware_pending.empty() || !firmware_installed.empty();
    case Step::Games:
        return !games.empty();
    default:
        return true;
    }
}

std::string Wizard::NextLabel() const {
    switch (current) {
    case Step::Welcome:
        return "Start";
    case Step::Firmware:
        return firmware_pending.empty() ? "Next" : "Install";
    case Step::Dlc:
        return dlc.empty() ? "Skip" : "Next";
    case Step::Updates:
        return updates.empty() ? "Skip" : "Next";
    case Step::Ready:
        return "Start Harbor";
    default:
        return "Next";
    }
}

std::vector<int> Wizard::StepDots() const {
    std::vector<int> dots;
    for (int s = static_cast<int>(Step::Device); s <= static_cast<int>(Step::Ready); s++) {
        const auto step = static_cast<Step>(s);
        if (!Shown(step)) {
            continue;
        }
        dots.push_back(step < current ? -1 : step == current ? 0 : 1);
    }
    return dots;
}

std::optional<double> Wizard::ProgressFraction() const {
    const double fraction = progress_fraction.load();
    return fraction < 0 ? std::nullopt : std::optional{fraction};
}

bool Wizard::Shown(Step step) const {
    return step != Step::Driver || driver_page;
}

Step Wizard::Neighbour(Step step, int direction) const {
    auto s = static_cast<int>(step);
    do {
        s += direction;
    } while (!Shown(static_cast<Step>(s)));
    return static_cast<Step>(s);
}

int Wizard::Note(Step step) const {
    int note = 0;
    for (int s = 0; s < static_cast<int>(step); s++) {
        note += Shown(static_cast<Step>(s)) ? 1 : 0;
    }
    return note;
}

void Wizard::GoTo(Step step) {
    current = step;
    Enter();
    if (page_shown) {
        page_shown(Note(step));
    }
    Notify();
}

void Wizard::Enter() {
    status.clear();
    status_tone = Tone::None;
    switch (current) {
    case Step::Device: {
        const auto fail = std::find_if(checks.begin(), checks.end(),
                                       [](const auto& row) { return row.level == CheckLevel::Fail; });
        if (fail != checks.end()) {
            ShowStatus("Harbor cannot run on this device: " + fail->reason, Tone::Bad);
        } else if (std::any_of(checks.begin(), checks.end(),
                               [](const auto& row) { return row.level == CheckLevel::Warn; })) {
            ShowStatus("Harbor can run here, with the limits marked above.", Tone::Warn);
        }
        break;
    }
    case Step::Keys:
        if (keys_usable) {
            ShowStatus("Keys are installed. Choose another file only to replace them.", Tone::Ok);
        }
        break;
    case Step::Firmware:
        if (!firmware_pending.empty()) {
            break;
        }
        if (!firmware_installed.empty()) {
            ShowStatus("Firmware " + firmware_installed +
                           " is installed. Choose another only to replace it.",
                       Tone::Ok);
        } else if (!firmware_problem.empty()) {
            ShowStatus(firmware_problem, Tone::Bad);
        }
        break;
    case Step::Driver:
        if (AdrenoModel(device) / 100 == 6) {
            ShowStatus("On Adreno 6xx GPUs the phone's own driver can crash the HOME menu. MrPurple's "
                       "Turnip T30 is recommended: browse GitHub drivers to install it.",
                       Tone::Warn);
        }
        break;
    default:
        break;
    }
}

void Wizard::Notify() {
    if (changed) {
        changed();
    }
}

void Wizard::ShowStatus(std::string text, Tone tone) {
    status = std::move(text);
    status_tone = status.empty() ? Tone::None : tone;
}

bool Wizard::Save() {
    std::string error;
    if (!SaveRecord(setup, record_path, &error)) {
        ShowStatus("The setup could not be saved: " + error, Tone::Bad);
        return false;
    }
    return true;
}

void Wizard::StartWork(std::string stage, bool can_cancel, std::function<void()> work) {
    busy = true;
    cancellable = can_cancel;
    cancel_requested = false;
    progress_text = std::move(stage);
    progress_fraction = -1.0;
    ShowStatus({}, Tone::None);
    Notify();
    if (worker.joinable()) {
        worker.join();
    }
    worker = std::thread(std::move(work));
}

void Wizard::EndWork() {
    busy = false;
    cancellable = false;
    progress_text.clear();
    progress_fraction = -1.0;
}

void Wizard::ReportProgress(std::string stage, std::optional<double> fraction) {
    // Worker thread. The bar reads the fraction directly; the words go through the UI thread, at most
    // ten times a second.
    progress_fraction = fraction.value_or(-1.0);
    static thread_local auto last = std::chrono::steady_clock::time_point{};
    static thread_local std::string last_stage;
    const auto now = std::chrono::steady_clock::now();
    if (stage == last_stage && now - last < std::chrono::milliseconds(100)) {
        return;
    }
    last = now;
    last_stage = stage;
    auto flag = alive;
    post([this, flag, stage = std::move(stage)] {
        if (*flag && busy) {
            progress_text = stage;
            if (progress_changed) {
                progress_changed();
            } else {
                Notify();
            }
        }
    });
}

std::string Wizard::KeysChosen() const {
    if (!keys_chosen.empty()) {
        return keys_chosen;
    }
    return keys_usable ? "prod.keys is installed" : "Not chosen yet";
}

void Wizard::ChooseProdKeys() {
    if (busy) {
        return;
    }
    auto flag = alive;
    host.PickFile("Choose prod.keys", {"keys"}, [this, flag](std::optional<Picked> picked) {
        post([this, flag, picked] {
            if (!*flag || !picked || busy) {
                return;
            }
            keys_chosen = picked->name;
            StartWork("Checking the keys", false, [this, flag, location = picked->location] {
                const auto result = ImportProdKeys(location);
                post([this, flag, result] {
                    if (!*flag) {
                        return;
                    }
                    keys_usable = keys_usable || result.ok;
                    setup.keys_present = keys_usable;
                    EndWork();
                    ShowStatus(result.message, result.ok ? Tone::Ok : Tone::Bad);
                    Notify();
                });
            });
        });
    });
}

void Wizard::ChooseTitleKeys() {
    if (busy) {
        return;
    }
    auto flag = alive;
    host.PickFile("Choose title.keys", {"keys"}, [this, flag](std::optional<Picked> picked) {
        post([this, flag, picked] {
            if (!*flag || !picked || busy) {
                return;
            }
            StartWork("Checking the title keys", false, [this, flag, location = picked->location] {
                const auto result = ImportTitleKeys(location);
                post([this, flag, result] {
                    if (!*flag) {
                        return;
                    }
                    EndWork();
                    ShowStatus(result.message, result.ok ? Tone::Ok : Tone::Bad);
                    Notify();
                });
            });
        });
    });
}

std::string Wizard::FirmwareChosen() const {
    if (!firmware_chosen.empty()) {
        return firmware_chosen;
    }
    return firmware_installed.empty() ? "Not chosen yet" : "Firmware " + firmware_installed + " is installed";
}

void Wizard::ChooseFirmwareFile() {
    if (busy) {
        return;
    }
    auto flag = alive;
    host.PickFile("Choose a firmware .zip", {"zip"}, [this, flag](std::optional<Picked> picked) {
        post([this, flag, picked] {
            if (!*flag || !picked || busy) {
                return;
            }
            firmware_chosen = picked->name;
            firmware_pending.clear();
            StartWork("Checking the firmware", true, [this, flag, location = picked->location] {
                const auto result = FirmwareManager::StageFirmware(
                    location, [this](const FirmwareManager::FirmwareProgress& p) {
                        ReportProgress(p.stage, p.fraction);
                        return !cancel_requested.load();
                    });
                post([this, flag, result] {
                    if (!*flag) {
                        return;
                    }
                    EndWork();
                    if (!result.ok) {
                        ShowStatus(result.error, Tone::Bad);
                    } else if (result.version == FirmwareManager::HarborFirmwareVersion) {
                        firmware_pending = result.version;
                        ShowStatus("Firmware " + result.version +
                                       " found. Select Install to put it on the console.",
                                   Tone::Ok);
                    } else {
                        firmware_pending = result.version;
                        ShowStatus("Firmware " + result.version + " found. Harbor is developed against " +
                                       std::string(FirmwareManager::HarborFirmwareVersion) + ", so " +
                                       result.version +
                                       " may not reach HOME. You can install it anyway.",
                                   Tone::Warn);
                    }
                    Notify();
                });
            });
        });
    });
}

void Wizard::ChooseFirmwareFolder() {
    if (busy) {
        return;
    }
    auto flag = alive;
    host.PickFolder("Choose a firmware folder", [this, flag](std::optional<Picked> picked) {
        post([this, flag, picked] {
            if (!*flag || !picked || busy) {
                return;
            }
            if (!picked->is_path) {
                ShowStatus("Harbor cannot read this folder directly. Choose the firmware as a .zip instead.",
                           Tone::Bad);
                Notify();
                return;
            }
            firmware_chosen = picked->name;
            firmware_pending.clear();
            StartWork("Checking the firmware", true, [this, flag, location = picked->location] {
                const auto result = FirmwareManager::StageFirmware(
                    location, [this](const FirmwareManager::FirmwareProgress& p) {
                        ReportProgress(p.stage, p.fraction);
                        return !cancel_requested.load();
                    });
                post([this, flag, result] {
                    if (!*flag) {
                        return;
                    }
                    EndWork();
                    if (!result.ok) {
                        ShowStatus(result.error, Tone::Bad);
                    } else {
                        firmware_pending = result.version;
                        const bool developed = result.version == FirmwareManager::HarborFirmwareVersion;
                        ShowStatus(developed ? "Firmware " + result.version +
                                                   " found. Select Install to put it on the console."
                                             : "Firmware " + result.version +
                                                   " found. Harbor is developed against " +
                                                   std::string(FirmwareManager::HarborFirmwareVersion) +
                                                   ", so " + result.version +
                                                   " may not reach HOME. You can install it anyway.",
                                   developed ? Tone::Ok : Tone::Warn);
                    }
                    Notify();
                });
            });
        });
    });
}

std::vector<Folder>* Wizard::CurrentFolders() {
    switch (current) {
    case Step::Games:
        return &games;
    case Step::Dlc:
        return &dlc;
    case Step::Updates:
        return &updates;
    default:
        return nullptr;
    }
}

const std::vector<Folder>& Wizard::Folders() const {
    static const std::vector<Folder> none;
    switch (current) {
    case Step::Games:
        return games;
    case Step::Dlc:
        return dlc;
    case Step::Updates:
        return updates;
    default:
        return none;
    }
}

std::string Wizard::AddFolderLabel() const {
    switch (current) {
    case Step::Games:
        return "Add a games folder";
    case Step::Dlc:
        return "Add a DLC folder";
    default:
        return "Add an updates folder";
    }
}

void Wizard::AddFolder() {
    if (busy || !CurrentFolders()) {
        return;
    }
    auto flag = alive;
    const Step step = current;
    host.PickFolder(AddFolderLabel(), [this, flag, step](std::optional<Picked> picked) {
        post([this, flag, step, picked] {
            if (!*flag || !picked || current != step) {
                return;
            }
            auto* folders = CurrentFolders();
            ShowStatus({}, Tone::None);
            if (std::none_of(folders->begin(), folders->end(),
                             [&](const Folder& f) { return f.location == picked->location; })) {
                folders->push_back({picked->location,
                                    picked->name.empty() ? LastSegment(ReadablePath(picked->location))
                                                         : picked->name,
                                    ReadablePath(picked->location)});
            }
            Notify();
        });
    });
}

void Wizard::RemoveFolder(size_t index) {
    auto* folders = CurrentFolders();
    if (busy || !folders || index >= folders->size()) {
        return;
    }
    folders->erase(folders->begin() + static_cast<std::ptrdiff_t>(index));
    Notify();
}

void Wizard::FillDriverChoices(const std::string& selected) {
    driver_choices.clear();
    const bool adreno6 = AdrenoModel(device) / 100 == 6;
    driver_choices.push_back({"", "Phone's driver",
                              adreno6 ? "Can crash HOME on this GPU" : "Recommended to start"});
    for (const auto& driver : host.InstalledDrivers()) {
        driver_choices.push_back({driver.id, driver.name, driver.detail});
    }
    const bool known = std::any_of(driver_choices.begin(), driver_choices.end(),
                                   [&](const DriverChoice& c) { return c.id == selected; });
    selected_driver = known ? selected : "";
}

void Wizard::SelectDriver(size_t index) {
    if (busy || index >= driver_choices.size()) {
        return;
    }
    selected_driver = driver_choices[index].id;
    Notify();
}

bool Wizard::CanBrowseDrivers() const {
    return HarborSetup::CanBrowseDrivers(device);
}

std::string Wizard::DownloadLead() const {
    return CanBrowseDrivers()
               ? "Choose from recent Turnip and Qualcomm releases on GitHub. Packages are filtered for "
                 "your GPU; Android requirements are checked before installation. You can always "
                 "switch back to the phone's driver."
               : "Online downloads are not available for this GPU or Android version. You can still "
                 "install a compatible AdrenoTools driver file.";
}

void Wizard::InstallDriverPackage(const std::string& location, const std::string& file_name) {
    // Worker thread.
    auto flag = alive;
    const auto finish = [this, flag](std::string message, Tone tone, std::optional<std::string> chosen) {
        post([this, flag, message, tone, chosen] {
            if (!*flag) {
                return;
            }
            EndWork();
            if (chosen) {
                FillDriverChoices(*chosen);
            }
            ShowStatus(message, tone);
            Notify();
        });
    };

    const auto package = host.InspectDriver(location);
    if (!package.error.empty()) {
        finish(package.error, Tone::Bad, std::nullopt);
        return;
    }
    if (const auto problem = DriverPackageProblem(package.name, package.library_name, package.min_api, device);
        !problem.empty()) {
        host.DiscardDriver(package);
        LOG_WARNING(Frontend, "harbor: driver {} refused: {}", file_name, problem);
        finish(problem, Tone::Bad, std::nullopt);
        return;
    }
    std::string error;
    const auto installed = host.CommitDriver(package, &error);
    if (!installed) {
        finish(error.empty() ? "The driver could not be installed." : error, Tone::Bad, std::nullopt);
        return;
    }
    LOG_INFO(Frontend, "harbor: driver {} installed", installed->id);
    finish(installed->name + " is installed. It is chosen; the phone's driver stays one tap away.",
           Tone::Ok, installed->id);
}

void Wizard::InstallDriverFile() {
    if (busy) {
        return;
    }
    auto flag = alive;
    host.PickFile("Choose a driver .zip", {"zip"}, [this, flag](std::optional<Picked> picked) {
        post([this, flag, picked] {
            if (!*flag || !picked || busy) {
                return;
            }
            StartWork("Installing " + picked->name, false,
                      [this, location = picked->location, name = picked->name] {
                          InstallDriverPackage(location, name);
                      });
        });
    });
}

void Wizard::BrowseDrivers() {
    if (busy || !CanBrowseDrivers()) {
        return;
    }
    auto flag = alive;
    StartWork("Checking GitHub releases", true, [this, flag] {
        DriverListing listing;
        std::vector<std::string> errors;
        for (const char* repository : DriverRepositories) {
            if (cancel_requested) {
                break;
            }
            std::string error;
            const auto json = host.HttpGet(ReleasesUrl(repository), MaxReleasesResponse, cancel_requested, &error);
            const std::string owner = std::string(repository).substr(0, std::string(repository).find('/'));
            if (!json) {
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
        const bool cancelled = cancel_requested.load();
        post([this, flag, listing, cancelled] {
            if (!*flag) {
                return;
            }
            EndWork();
            if (cancelled) {
                ShowStatus("Cancelled. Your selected driver is unchanged.", Tone::Warn);
            } else {
                downloads = listing.releases;
                if (listing.notice) {
                    ShowStatus(*listing.notice, Tone::Warn);
                } else if (downloads.empty()) {
                    ShowStatus("No matching releases were found. You can keep the phone's driver or "
                               "install a file.",
                               Tone::Warn);
                }
            }
            Notify();
        });
    });
}

void Wizard::Download(size_t index) {
    if (busy || index >= downloads.size()) {
        return;
    }
    const auto release = downloads[index];
    auto flag = alive;
    StartWork("Downloading " + release.file_name, true, [this, flag, release] {
        const auto fail = [this, flag](std::string message, Tone tone) {
            post([this, flag, message, tone] {
                if (!*flag) {
                    return;
                }
                EndWork();
                ShowStatus(message, tone);
                Notify();
            });
        };
        if (!AllowedDriverDownload(release.download_url) || release.size == 0 ||
            release.size > MaxDriverDownload) {
            fail("This is not a supported GitHub driver download.", Tone::Bad);
            return;
        }
        const auto result = host.Download(
            release.download_url, release.size,
            [this, &release](std::uint64_t received) {
                ReportProgress("Downloading " + release.file_name + " (" + Megabytes(received) + " / " +
                                   Megabytes(release.size) + " MB)",
                               static_cast<double>(received) / static_cast<double>(release.size));
            },
            cancel_requested);
        if (cancel_requested) {
            std::error_code ec;
            std::filesystem::remove(result.path, ec);
            fail("Download cancelled. Your selected driver is unchanged.", Tone::Warn);
            return;
        }
        if (!result.error.empty()) {
            fail("The driver could not be downloaded: " + result.error, Tone::Bad);
            return;
        }
        const auto cleanup = [&] {
            std::error_code ec;
            std::filesystem::remove(result.path, ec);
        };
        if (result.size != release.size) {
            cleanup();
            fail("The download was incomplete. Try again.", Tone::Bad);
            return;
        }
        if (release.digest && Lower(result.sha256) != Lower(release.digest->substr(7))) {
            cleanup();
            fail("The download checksum did not match GitHub's release asset. Try again.", Tone::Bad);
            return;
        }
        cancellable = false;
        ReportProgress("Installing " + release.file_name, std::nullopt);
        InstallDriverPackage(result.path, release.file_name);
        cleanup();
    });
}

std::vector<SummaryRow> Wizard::Summary() const {
    std::vector<SummaryRow> rows{
        {"Keys", setup.keys_present ? "Installed" : "Missing"},
        {"Firmware", setup.firmware_version.value_or("Missing")},
        {"Games", CountFolders(setup.game_dirs)},
        {"DLC", CountFolders(setup.dlc_dirs)},
        {"Updates", CountFolders(setup.update_dirs)},
    };
    if (driver_page) {
        std::string name = "Phone's driver";
        for (const auto& choice : driver_choices) {
            if (!choice.id.empty() && choice.id == setup.gpu_driver.value_or("")) {
                name = choice.title;
            }
        }
        rows.push_back({"Graphics driver", name});
    }
    return rows;
}

void Wizard::Next() {
    if (!CanGoNext()) {
        return;
    }
    const Step page = current;

    // Firmware waiting to be installed: Next installs it first.
    if (page == Step::Firmware && !firmware_pending.empty()) {
        auto flag = alive;
        const auto version = firmware_pending;
        StartWork("Installing firmware " + version, false, [this, flag, version] {
            std::string error;
            const bool ok = FirmwareManager::CommitStagedFirmware(&error);
            if (ok) {
                host.FirmwareInstalled();
            }
            post([this, flag, ok, error, version] {
                if (!*flag) {
                    return;
                }
                EndWork();
                firmware_pending.clear();
                if (!ok) {
                    ShowStatus(error, Tone::Bad);
                    Notify();
                    return;
                }
                firmware_installed = version;
                firmware_problem.clear();
                firmware_chosen.clear();
                setup.firmware_version = version;
                if (!Save()) {
                    Notify();
                    return;
                }
                LOG_INFO(Frontend, "harbor: firmware {} installed", version);
                GoTo(Neighbour(Step::Firmware, 1));
            });
        });
        return;
    }

    switch (page) {
    case Step::Keys:
        setup.keys_present = true;
        break;
    case Step::Firmware:
        setup.firmware_version = firmware_installed;
        break;
    case Step::Games:
        setup.game_dirs = ToLocations(games);
        break;
    case Step::Dlc:
        setup.dlc_dirs = ToLocations(dlc);
        break;
    case Step::Updates:
        setup.update_dirs = ToLocations(updates);
        break;
    case Step::Driver: {
        std::string error;
        if (!host.SelectDriver(selected_driver, &error)) {
            ShowStatus(error, Tone::Bad);
            Notify();
            return;
        }
        setup.gpu_driver = selected_driver;
        break;
    }
    default:
        break;
    }

    // The folders go where the emulator reads them, before the record says so; at Ready again, so a
    // resumed setup that skipped the folder pages still leaves them (and the driver) in place.
    if (page == Step::Games || page == Step::Dlc || page == Step::Updates || page == Step::Ready) {
        std::string error;
        if (page == Step::Ready && driver_page && setup.gpu_driver &&
            !host.SelectDriver(*setup.gpu_driver, &error)) {
            ShowStatus(error, Tone::Bad);
            Notify();
            return;
        }
        if (!host.ApplyFolders(setup, &error)) {
            ShowStatus("The folders could not be saved: " + error, Tone::Bad);
            Notify();
            return;
        }
    }

    // Welcome and the device check record nothing.
    if (page > Step::Device) {
        setup.completed = page == Step::Ready;
        if (!Save()) {
            setup.completed = false;
            Notify();
            return;
        }
    }

    if (page == Step::Ready) {
        LOG_INFO(Frontend, "harbor: setup completed");
        host.Finished(setup);
        if (finished) {
            finished();
        }
        return;
    }
    GoTo(Neighbour(page, 1));
}

void Wizard::Back() {
    if (CanGoBack()) {
        GoTo(Neighbour(current, -1));
    }
}

void Wizard::Skip() {
    if (!CanSkip()) {
        return;
    }
    selected_driver.clear();
    Next();
}

void Wizard::Cancel() {
    if (CanCancel()) {
        cancel_requested = true;
        Notify();
    }
}

} // namespace HarborSetup

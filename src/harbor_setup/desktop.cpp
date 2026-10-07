// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// The desktop host: SDL's own file and folder dialogs (the portal on Linux), run before Qt starts.

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include <SDL3/SDL.h>

#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "harbor_setup/harbor_setup.h"
#include "harbor_setup/host.h"

namespace HarborSetup {

namespace {

void DialogDone(void* userdata, const char* const* files, int) {
    std::unique_ptr<PickCallback> done(static_cast<PickCallback*>(userdata));
    if (!files || !files[0]) {
        (*done)(std::nullopt);
        return;
    }
    const std::filesystem::path path(files[0]);
    auto name = path.filename().string();
    if (name.empty()) {
        name = path.parent_path().filename().string();
    }
    (*done)(Picked{name, path.string(), true});
}

const char* Home() {
    const char* home = std::getenv("HOME");
    return home ? home : nullptr;
}

// Test hook: HARBOR_SETUP_PICKS="path|path|..." answers the next pickers in order without opening a
// dialog, so a scripted run needs no file chooser on the screen.
std::vector<std::string> ScriptedPicks() {
    std::vector<std::string> picks;
    const char* env = std::getenv("HARBOR_SETUP_PICKS");
    if (!env) {
        return picks;
    }
    std::string list = env;
    size_t start = 0;
    while (start <= list.size()) {
        const size_t end = std::min(list.find('|', start), list.size());
        if (end > start) {
            picks.push_back(list.substr(start, end - start));
        }
        start = end + 1;
    }
    return picks;
}

class DesktopHost final : public Host {
public:
    bool IsAndroid() const override {
        return false;
    }

    bool Scripted(PickCallback& done) {
        if (picks.empty()) {
            return false;
        }
        const auto path = picks.front();
        picks.erase(picks.begin());
        LOG_INFO(Frontend, "harbor: scripted pick");
        const char* files[] = {path.c_str(), nullptr};
        DialogDone(new PickCallback(std::move(done)), files, 0);
        return true;
    }

    DeviceInfo Probe() override {
        return ProbeDevice(Common::FS::GetEdenPathString(Common::FS::EdenPath::EdenDir), false);
    }

    void PickFile(const std::string& title, const std::vector<std::string>& extensions,
                  PickCallback done) override {
        if (Scripted(done)) {
            return;
        }
        static std::vector<std::string> patterns;
        patterns.clear();
        static std::vector<SDL_DialogFileFilter> filters;
        filters.clear();
        for (const auto& extension : extensions) {
            patterns.push_back(extension);
        }
        for (const auto& pattern : patterns) {
            filters.push_back({pattern == "keys" ? "Keys" : pattern == "zip" ? "Zip archive" : "File",
                               pattern.c_str()});
        }
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, filters.data());
        SDL_SetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, static_cast<Sint64>(filters.size()));
        SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, title.c_str());
        if (const char* home = Home()) {
            SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_LOCATION_STRING, home);
        }
        SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_OPENFILE, DialogDone,
                                         new PickCallback(std::move(done)), props);
        SDL_DestroyProperties(props);
    }

    void PickFolder(const std::string& title, PickCallback done) override {
        if (Scripted(done)) {
            return;
        }
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, title.c_str());
        if (const char* home = Home()) {
            SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_LOCATION_STRING, home);
        }
        SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_OPENFOLDER, DialogDone,
                                         new PickCallback(std::move(done)), props);
        SDL_DestroyProperties(props);
    }

private:
    std::vector<std::string> picks = ScriptedPicks();
};

} // namespace

namespace {

bool console_allowed = false;

/// Removes `flag` from argv; whether it was there.
bool TakeFlag(int& argc, char** argv, const char* flag) {
    bool found = false;
    for (int i = 1; i < argc;) {
        if (std::strcmp(argv[i], flag) == 0) {
            found = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            argv[argc] = nullptr;
        } else {
            i++;
        }
    }
    return found;
}

/// Whether the command line names what to boot: --qlaunch, --hlaunch, --setup (one or two dashes),
/// -g/--game, or a game path (core/launch_params.cpp).
bool ExplicitBoot(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        const std::string_view arg = argv[i];
        for (const std::string_view target : {"qlaunch", "hlaunch", "setup"}) {
            if (arg == std::string("-").append(target) || arg == std::string("--").append(target)) {
                return true;
            }
        }
        if (arg == "-g" || arg == "--game" || arg.starts_with("--game=")) {
            return true;
        }
        // Options that take the next word as their value.
        if (arg == "-c" || arg == "--config" || arg == "-m" || arg == "--multiplayer" || arg == "-u" ||
            arg == "--user" || arg == "-p" || arg == "--program" || arg == "-x" || arg == "--filter" ||
            arg == "-i" || arg == "--input-profile" || arg == "-d" || arg == "--debug") {
            i++;
            continue;
        }
        if (!arg.empty() && arg[0] != '-') {
            return true;
        }
    }
    return false;
}

} // namespace

bool DesktopConsoleAllowed() {
    return console_allowed;
}

int RunDesktopIfNeeded(int& argc, char** argv) {
    const bool forced = TakeFlag(argc, argv, "--harbor-setup");
    const bool console = TakeFlag(argc, argv, "--harbor-console");

    // Test harness runs (HARBOR_CONTROL) and an explicit boot target (--qlaunch, a game) boot what
    // was asked: no wizard and no console mode, unless --harbor-setup or --harbor-console says so.
    if (!forced && !console && (std::getenv("HARBOR_CONTROL") || ExplicitBoot(argc, argv))) {
        return -1;
    }
    console_allowed = true;
    const auto readiness = CheckReadiness();
    if (!forced && readiness.Ready()) {
        return -1;
    }

    // The wizard's lines go to the emulator's log too; Qt applies the configured filter later.
    Common::FS::CreateEdenPaths();
    Common::Log::Initialize();
    LOG_INFO(Frontend, "harbor: setup {} before the console starts", forced ? "requested" : "needed");

    DesktopHost host;
    return RunWizard(host);
}

} // namespace HarborSetup

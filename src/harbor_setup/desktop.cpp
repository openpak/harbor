// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// The desktop host: SDL's own file and folder dialogs (the portal on Linux), run before Qt starts.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL.h>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/string_util.h"
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

/// Where the pickers open (UTF-8, as SDL takes it); empty when unknown.
std::string Home() {
#ifdef _WIN32
    const wchar_t* home = _wgetenv(L"USERPROFILE");
    return home ? Common::UTF16ToUTF8(std::wstring_view{home}) : std::string{};
#else
    const char* home = std::getenv("HOME");
    return home ? home : std::string{};
#endif
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
        if (const auto home = Home(); !home.empty()) {
            SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_LOCATION_STRING, home.c_str());
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
        if (const auto home = Home(); !home.empty()) {
            SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_LOCATION_STRING, home.c_str());
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

/// Starts the console in a fresh process with this one's arguments (TakeFlag removed the Harbor
/// flags it was started with) and --harbor-console, which says to boot the console even in a
/// test-harness run. Linux and macOS replace this process (same pid); Windows starts a new one and
/// returns true, and this one ends. False when no fresh process could be started.
bool RestartForConsole(int argc, char** argv) {
#ifdef _WIN32
    // The wide command line: argv is in the ANSI code page and may not hold every path.
    static_cast<void>(argc);
    static_cast<void>(argv);
    int count = 0;
    LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!wide) {
        return false;
    }
    std::vector<std::string> args;
    for (int i = 0; i < count; i++) {
        const std::wstring_view arg{wide[i]};
        if (i > 0 && (arg == L"--harbor-setup" || arg == L"--harbor-console")) {
            continue;
        }
        args.push_back(Common::UTF16ToUTF8(arg));
    }
    LocalFree(wide);
    if (args.empty()) {
        return false;
    }
    wchar_t exe[MAX_PATH * 4];
    const DWORD length = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    if (length > 0 && length < std::size(exe)) {
        args[0] = Common::UTF16ToUTF8(std::wstring_view{exe, length});
    }
    args.push_back("--harbor-console");
    std::vector<const char*> pointers;
    for (const auto& arg : args) {
        pointers.push_back(arg.c_str());
    }
    pointers.push_back(nullptr);
    SDL_Process* process = SDL_CreateProcess(pointers.data(), false);
    if (!process) {
        return false;
    }
    SDL_DestroyProcess(process); // only the handle: the console keeps running
    return true;
#else
    std::vector<char*> args(argv, argv + argc);
    static char console_flag[] = "--harbor-console";
    args.push_back(console_flag);
    args.push_back(nullptr);
#if defined(__linux__)
    execv("/proc/self/exe", args.data());
#elif defined(__APPLE__)
    std::string exe(4096, '\0');
    std::uint32_t size = static_cast<std::uint32_t>(exe.size());
    if (_NSGetExecutablePath(exe.data(), &size) == 0) {
        execv(exe.c_str(), args.data());
    }
#else
    execvp(argv[0], args.data());
#endif
    return false; // exec failed
#endif
}

} // namespace

bool DesktopConsoleAllowed() {
    return console_allowed;
}

int RunDesktopIfNeeded(int& argc, char** argv) {
    const bool forced = TakeFlag(argc, argv, "--harbor-setup");
    const bool console = TakeFlag(argc, argv, "--harbor-console");

    // Harbor always starts the console (the setup first when it is needed): there is no plain Eden
    // mode. Only the test harness (HARBOR_CONTROL) boots what its command line asks for (--qlaunch),
    // unless --harbor-setup or --harbor-console says so.
    if (!forced && !console && std::getenv("HARBOR_CONTROL")) {
        return -1;
    }
    if (ExplicitBoot(argc, argv)) {
        Common::Log::Initialize();
        LOG_INFO(Frontend, "harbor: a boot target on the command line is ignored: Harbor starts the console");
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
    const int result = RunWizard(host);
    if (result == 0) {
        // Borealis ran the setup in this process with SDL's window; the Harbor menu starts it with
        // its own platform, once per process. The console starts in a fresh process.
        LOG_INFO(Frontend, "harbor: setup completed; starting the console in a fresh process");
        Common::Log::Stop();
        if (RestartForConsole(argc, argv)) {
            return 1; // Windows: the new process runs the console; this one ends.
        }
        // No fresh process: go on in this one.
    }
    return result;
}

} // namespace HarborSetup

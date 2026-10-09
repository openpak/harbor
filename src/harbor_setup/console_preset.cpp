// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/set/setting_formats/system_settings.h"
#include "core/hle/service/set/system_settings_server.h"
#include "core/hle/service/sm/sm.h"
#include "frontend_common/secret_store.h"
#include "harbor_setup/harbor_setup.h"

namespace HarborSetup {

void ApplyConsolePreset() {
    using Settings::AppletMode;
    auto& v = Settings::values;
    for (auto* mode : {&v.cabinet_applet_mode, &v.controller_applet_mode, &v.error_applet_mode,
                       &v.player_select_applet_mode, &v.swkbd_applet_mode, &v.mii_edit_applet_mode,
                       &v.web_applet_mode, &v.photo_viewer_applet_mode, &v.offline_web_applet_mode}) {
        mode->SetValue(AppletMode::LLE);
    }
    for (auto* mode : {&v.data_erase_applet_mode, &v.net_connect_applet_mode, &v.shop_applet_mode,
                       &v.login_share_applet_mode, &v.wifi_web_auth_applet_mode, &v.my_page_applet_mode}) {
        mode->SetValue(AppletMode::LLE);
    }
#ifdef __ANDROID__
    // A phone is the console in handheld mode.
    v.use_docked_mode.SetValue(Settings::ConsoleMode::Handheld);
#else
    // A PC is a console on its TV dock: the first console start is docked (1080p, Joy-Cons off the
    // rails as one controller); after that the dock toggle's choice is kept. The test harness
    // chooses for its own runs (run-desktop.sh).
    const auto marker = Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "harbor-docked-default";
    // Not before the firmware's first-time setup is done: on a dock it stops at "remove the
    // console from the dock", as a real console's does.
    if (!std::getenv("HARBOR_CONTROL") && !std::filesystem::exists(marker) &&
        ReadInitialSetupDone(Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir) /
                             "system/save/8000000000000050/settings.dat")
            .value_or(false)) {
        v.use_docked_mode.SetValue(Settings::ConsoleMode::Docked);
        auto& players = v.players.GetValue(true);
        players[0].connected = true;
        players[0].controller_type = Settings::ControllerType::ProController;
        players[8].connected = false;
        std::error_code ec;
        std::filesystem::create_directories(marker.parent_path(), ec);
        std::ofstream{marker} << "docked by default on the first console start\n";
        LOG_INFO(Frontend, "harbor: the console starts docked (a PC on its TV dock)");
    }
#endif
    // Every log line reaches the file at once, so a console that crashes leaves its last lines.
    v.log_flush_line.SetValue(true);
    // A console has one network interface: always the one the host reaches the internet through,
    // never a saved pick (a VPN chosen once would stay the console's address).
    v.network_interface.SetValue("");
}

void ApplyRecordFolders(const Record& record) {
#ifdef __ANDROID__
    // The setup's DLC and update folders are SAF tree URIs, which core cannot walk: the launcher
    // registers their files itself (HarborConsole.registerAddons, as Eden's game list does games).
    static_cast<void>(record);
    Settings::values.external_content_dirs.clear();
#else
    Settings::values.external_content_dirs = record.AutoloadDirs();
#endif
    Settings::values.ext_content_from_game_dirs.SetValue(true);
}

OpenPak::BootResult ConnectOpenPak() {
    FrontendCommon::InstallSecretStore();
    auto result =
        OpenPak::BootConnection::Run(Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir));
    LOG_INFO(Frontend, "harbor: OpenPak {} ({}; profile {} v{}, ceiling {} v{}){}{}",
             result.enabled ? "on" : "off", result.online ? "online" : "offline",
             result.profile_source, result.profile_version, result.ceiling_source,
             result.ceiling_version, result.error.empty() ? "" : ": ", result.error);
    return result;
}

PowerRequest ConsolePowerRequest(Core::System& system) {
    switch (system.GetAppletManager().GetPowerRequest()) {
    case Service::AM::AppletManager::PowerRequest::Reboot:
        return PowerRequest::Reboot;
    case Service::AM::AppletManager::PowerRequest::Shutdown:
        return PowerRequest::Shutdown;
    default:
        return PowerRequest::None;
    }
}

// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base): the host theme is the
// console's saved colour set once there is one, the platform's before.
std::optional<bool> SavedConsoleDarkTheme() {
    return ReadConsoleDarkTheme(Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir) /
                                "system/save/8000000000000050/settings.dat");
}

std::optional<bool> LiveConsoleDarkTheme(Core::System& system) {
    if (!system.IsPoweredOn()) {
        return std::nullopt;
    }
    const auto set_sys =
        system.ServiceManager().GetService<Service::Set::ISystemSettingsServer>("set:sys", false);
    if (!set_sys) {
        return std::nullopt;
    }
    Service::Set::ColorSet color{};
    if (set_sys->GetColorSetId(&color).IsError()) {
        return std::nullopt;
    }
    return color == Service::Set::ColorSet::BasicBlack;
}

std::optional<bool> ReadInitialSetupDone(const std::filesystem::path& path) {
    constexpr std::size_t HeaderSize = 16;
    constexpr u64 Magic = Common::MakeMagic('y', 'u', 'z', 'u', '_', 's', 'e', 't');
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    u64 magic = 0;
    u32 version = 0;
    file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    file.read(reinterpret_cast<char*>(&version), sizeof(version));
    if (!file || magic != Magic || version < 4) {
        return std::nullopt;
    }
    u32 flags = 0;
    file.seekg(HeaderSize + offsetof(Service::Set::SystemSettings, initial_launch_settings_packed));
    file.read(reinterpret_cast<char*>(&flags), sizeof(flags));
    if (!file) {
        return std::nullopt;
    }
    return (flags & 1) != 0; // InitialLaunchCompletionFlag
}

std::optional<bool> ReadConsoleDarkTheme(const std::filesystem::path& path) {
    // set:sys keeps its settings as a 16-byte header ('yuzu_set', version >= 4) and the struct.
    constexpr std::size_t HeaderSize = 16;
    constexpr u64 Magic = Common::MakeMagic('y', 'u', 'z', 'u', '_', 's', 'e', 't');
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    u64 magic = 0;
    u32 version = 0;
    file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    file.read(reinterpret_cast<char*>(&version), sizeof(version));
    if (!file || magic != Magic || version < 4) {
        return std::nullopt;
    }
    u32 color_set = 0;
    file.seekg(HeaderSize + offsetof(Service::Set::SystemSettings, color_set_id));
    file.read(reinterpret_cast<char*>(&color_set), sizeof(color_set));
    if (!file) {
        return std::nullopt;
    }
    return color_set == static_cast<u32>(Service::Set::ColorSet::BasicBlack);
}

} // namespace HarborSetup

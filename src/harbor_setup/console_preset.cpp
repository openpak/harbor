// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <cstddef>
#include <fstream>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/set/setting_formats/system_settings.h"
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
    v.use_docked_mode.SetValue(Settings::ConsoleMode::Handheld);
    // Every log line reaches the file at once, so a console that crashes leaves its last lines.
    v.log_flush_line.SetValue(true);
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

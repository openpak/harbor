// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
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
}

void ApplyRecordFolders(const Record& record) {
    Settings::values.external_content_dirs = record.AutoloadDirs();
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
    // TODO(harbor): needs AppletManager::GetPowerRequest() from the am stream (branch p2/am, not
    // in harbor yet). Once it is merged:
    //   switch (system.GetAppletManager().GetPowerRequest()) {
    //   case Service::AM::AppletManager::PowerRequest::Reboot: return PowerRequest::Reboot;
    //   case Service::AM::AppletManager::PowerRequest::Shutdown: return PowerRequest::Shutdown;
    //   default: return PowerRequest::None;
    //   }
    // Until then a restart or power-off from HOME's power menu just stops the console.
    static_cast<void>(system);
    return PowerRequest::None;
}

} // namespace HarborSetup

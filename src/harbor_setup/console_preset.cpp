// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include "common/settings.h"
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

} // namespace HarborSetup

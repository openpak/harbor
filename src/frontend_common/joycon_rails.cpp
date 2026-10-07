// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base), EmulationSession.JoyConAttached.

#include "common/logging.h"
#include "common/settings.h"
#include "frontend_common/joycon_rails.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "hid_core/hid_types.h"

namespace FrontendCommon {

void SetJoyconsDetached(Core::HID::HIDCore& hid, bool detached) {
    using Core::HID::NpadIdType;
    using Core::HID::NpadStyleIndex;
    auto* const handheld = hid.GetEmulatedController(NpadIdType::Handheld);
    auto* const player1 = hid.GetEmulatedController(NpadIdType::Player1);
    // A style changes only while its controller is disconnected.
    player1->Disconnect();
    if (detached) {
        handheld->Disconnect();
        player1->SetNpadStyleIndex(NpadStyleIndex::JoyconDual);
        player1->Connect();
    } else {
        const auto& player = Settings::values.players.GetValue()[0];
        player1->SetNpadStyleIndex(
            Core::HID::EmulatedController::MapSettingsTypeToNPad(player.controller_type));
        if (player.connected) {
            player1->Connect();
        }
        handheld->Connect();
    }
    LOG_INFO(Frontend, "harbor: Joy-Cons {} (Handheld {}, Player 1 {})",
             detached ? "detached" : "attached",
             handheld->IsConnected() ? "connected" : "disconnected",
             player1->IsConnected() ? "connected" : "disconnected");
}

bool JoyconsDetached(const Core::HID::HIDCore& hid) {
    return !hid.GetEmulatedController(Core::HID::NpadIdType::Handheld)->IsConnected();
}

} // namespace FrontendCommon

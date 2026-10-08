// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <QCoreApplication>
#include <QGuiApplication>
#include <QKeySequence>
#include <QMetaObject>
#include <QProcess>
#include <QStyleHints>
#include <QWidget>

#include "common/logging.h"
#include "core/core.h"
#include "frontend_common/joycon_rails.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "qt_common/config/qt_config.h"
#include "qt_common/config/uisettings.h"
#include "qt_common/qt_common.h"
#include "yuzu/harbor_menu.h"
#ifdef HAS_LSFG
#include "yuzu/harbor_frame_gen.h"
#endif

HarborMenu::HarborMenu(QWidget* window_, InputCommon::InputSubsystem& input_, Hooks hooks_)
    : window{window_}, input{input_}, hooks{std::move(hooks_)} {
    HarborSetup::MenuSetHost(*this);
    WatchChord();
}

HarborMenu::~HarborMenu() {
    auto& hid = QtCommon::system->HIDCore();
    if (player1_callback >= 0) {
        hid.GetEmulatedController(Core::HID::NpadIdType::Player1)->DeleteCallback(player1_callback);
    }
    if (handheld_callback >= 0) {
        hid.GetEmulatedController(Core::HID::NpadIdType::Handheld)->DeleteCallback(handheld_callback);
    }
    HarborSetup::MenuShutdown();
}

void HarborMenu::Toggle() {
    if (HarborSetup::MenuIsOpen()) {
        HarborSetup::MenuClose();
    } else {
        HarborSetup::MenuOpen();
    }
}

/// Plus and Minus together open the menu, as on Android. (The press also reaches the console, as
/// Eden's own controller hotkeys do; while the menu is open the controllers are its own.)
void HarborMenu::WatchChord() {
    auto& hid = QtCommon::system->HIDCore();
    const auto watch = [](Core::HID::EmulatedController* controller) {
        return controller->SetCallback({
            .on_change =
                [controller](Core::HID::ControllerTriggerType type) {
                    if (type != Core::HID::ControllerTriggerType::Button || controller->IsConfiguring()) {
                        return;
                    }
                    const auto buttons = controller->GetNpadButtons();
                    if (buttons.plus && buttons.minus && !HarborSetup::MenuIsOpen()) {
                        LOG_INFO(Frontend, "harbor: menu asked for by Plus+Minus");
                        HarborSetup::MenuOpen();
                    }
                },
            .is_npad_service = false,
        });
    };
    player1_callback = watch(hid.GetEmulatedController(Core::HID::NpadIdType::Player1));
    handheld_callback = watch(hid.GetEmulatedController(Core::HID::NpadIdType::Handheld));
}

void HarborMenu::OnGui(std::function<void()> work) {
    QMetaObject::invokeMethod(window, std::move(work), Qt::QueuedConnection);
}

Core::System& HarborMenu::System() {
    return *QtCommon::system;
}

InputCommon::InputSubsystem& HarborMenu::Input() {
    return input;
}

bool HarborMenu::PlatformDarkTheme() {
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
}

void HarborMenu::SaveConfig() {
    OnGui([this] { hooks.save_config(); });
}

bool HarborMenu::JoyconsDetached() {
    return FrontendCommon::JoyconsDetached(QtCommon::system->HIDCore());
}

void HarborMenu::SetJoyconsDetached(bool detached) {
    OnGui([this, detached] { hooks.joycons(detached); });
}

void HarborMenu::Restart() {
    OnGui([this] { hooks.restart(); });
}

void HarborMenu::SetupAgain() {
    // The setup runs before Qt, in a new process (Borealis runs it once per process).
    OnGui([this] {
        LOG_INFO(Frontend, "harbor: set up again: starting the setup in a new process");
        QProcess::startDetached(QCoreApplication::applicationFilePath(), {QStringLiteral("--harbor-setup")});
        hooks.exit();
    });
}

void HarborMenu::Exit() {
    OnGui([this] { hooks.exit(); });
}

bool HarborMenu::ShowFps() {
    return hooks.fps_shown();
}

void HarborMenu::SetShowFps(bool show) {
    // Kept at once, so the menu reads it back; the overlay follows on the GUI thread.
    UISettings::values.show_perf_overlay = show;
    OnGui([this, show] { hooks.show_fps(show); });
}

bool HarborMenu::FrameGenSupported() {
#ifdef HAS_LSFG
    return HarborFrameGen::GpuSupported();
#else
    return false;
#endif
}

void HarborMenu::ChooseLosslessDll() {
#ifdef HAS_LSFG
    OnGui([this] { HarborFrameGen::ChooseDll(window, hooks.change_dll); });
#endif
}

void HarborMenu::RemoveLosslessDll() {
#ifdef HAS_LSFG
    OnGui([this] { HarborFrameGen::RemoveDll(hooks.change_dll, hooks.save_config); });
#endif
}

std::optional<HarborSetup::KeyboardDefaults> HarborMenu::DefaultKeyboard() {
    HarborSetup::KeyboardDefaults defaults;
    defaults.buttons = QtConfig::default_buttons;
    defaults.analogs = QtConfig::default_analogs;
    defaults.modifiers = QtConfig::default_stick_mod;
    return defaults;
}

std::string HarborMenu::KeyName(int code) {
    return QKeySequence(code).toString(QKeySequence::NativeText).toStdString();
}

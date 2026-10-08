// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <QCoreApplication>
#include <QKeyEvent>

#include "core/core.h"
#include "core/hle/service/am/applet_manager.h"
#include "input_common/drivers/virtual_gamepad.h"
#include "input_common/main.h"
#include "qt_common/qt_common.h"
#include "yuzu/harbor_system_keys.h"

namespace {

using VirtualButton = InputCommon::VirtualGamepad::VirtualButton;

// Player 1 and Handheld; the firmware's button poller listens on both.
constexpr std::size_t PlayerPorts[] = {0, 8};

} // namespace

HarborSystemKeys::HarborSystemKeys(InputCommon::InputSubsystem& input_subsystem_, QObject* parent)
    : QObject(parent), input_subsystem{input_subsystem_} {
    QCoreApplication::instance()->installEventFilter(this);
}

bool HarborSystemKeys::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() != QEvent::KeyPress && event->type() != QEvent::KeyRelease) {
        return QObject::eventFilter(watched, event);
    }
    const auto* key = static_cast<QKeyEvent*>(event);
    // F5 power held and F10 force-terminate, on the press. Shift+F5 is Eden's stop.
    if ((key->key() == Qt::Key_F5 || key->key() == Qt::Key_F10) &&
        key->modifiers() == Qt::NoModifier) {
        if (event->type() == QEvent::KeyPress && !key->isAutoRepeat() && QtCommon::system) {
            auto& applet_manager = QtCommon::system->GetAppletManager();
            if (key->key() == Qt::Key_F5) {
                applet_manager.PowerButtonMiddlePress();
            } else {
                applet_manager.ForceTerminateApplication();
            }
        }
        return true;
    }
    VirtualButton button;
    switch (key->key()) {
    case Qt::Key_F8:
        button = VirtualButton::ButtonHome;
        break;
    case Qt::Key_F4:
        button = VirtualButton::ButtonCapture;
        break;
    default:
        return QObject::eventFilter(watched, event);
    }
    // Shift+F4 / Shift+F8 are Eden's pause and filter hotkeys.
    if (key->modifiers() != Qt::NoModifier) {
        return QObject::eventFilter(watched, event);
    }
    if (!key->isAutoRepeat()) {
        const bool pressed = event->type() == QEvent::KeyPress;
        auto* const gamepad = input_subsystem.GetVirtualGamepad();
        for (const auto port : PlayerPorts) {
            gamepad->SetButtonState(port, static_cast<int>(button), pressed);
        }
    }
    return true;
}

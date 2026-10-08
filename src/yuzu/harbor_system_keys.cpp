// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <QCoreApplication>
#include <QKeyEvent>
#include <QPalette>
#include <QTimer>
#include <QWidget>

#include "core/core.h"
#include "common/logging.h"
#include "core/hle/service/am/applet_manager.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "input_common/drivers/virtual_gamepad.h"
#include "input_common/main.h"
#include "qt_common/qt_common.h"
#include "yuzu/harbor_system_keys.h"

namespace {

using VirtualButton = InputCommon::VirtualGamepad::VirtualButton;

// The power button's hold that opens the power menu, the same on every power input.
constexpr int PowerHoldMs = 1000;

// Player 1 and Handheld; the firmware's button poller listens on both.
constexpr std::size_t PlayerPorts[] = {0, 8};

} // namespace

HarborSystemKeys::HarborSystemKeys(InputCommon::InputSubsystem& input_subsystem_, QObject* parent)
    : QObject(parent), input_subsystem{input_subsystem_} {
    QCoreApplication::instance()->installEventFilter(this);

    power_hold = new QTimer(this);
    power_hold->setSingleShot(true);
    power_hold->setInterval(PowerHoldMs);
    connect(power_hold, &QTimer::timeout, this, [] {
        if (QtCommon::system) {
            QtCommon::system->GetAppletManager().PowerButtonMiddlePress();
        }
    });
    wake_poll = new QTimer(this);
    wake_poll->setInterval(100);
    connect(wake_poll, &QTimer::timeout, this, [this] { PollWake(); });

    // am calls this with its locks held: the work runs on the Qt thread.
    QtCommon::system->GetAppletManager().SetSleepHandler([this](bool sleeping) {
        QMetaObject::invokeMethod(this, [this, sleeping] { ApplySleep(sleeping); },
                                  Qt::QueuedConnection);
    });
}

void HarborSystemKeys::ApplySleep(bool sleeping) {
    auto* const window = qobject_cast<QWidget*>(parent());
    if (sleeping) {
        if (sleep_screen != nullptr || window == nullptr) {
            return;
        }
        LOG_INFO(Frontend, "harbor: the console sleeps");
        paused_for_sleep = !QtCommon::system->IsPaused();
        if (paused_for_sleep) {
            QtCommon::system->Pause();
        }
        sleep_screen = new QWidget(window);
        sleep_screen->setAutoFillBackground(true);
        QPalette palette = sleep_screen->palette();
        palette.setColor(QPalette::Window, Qt::black);
        sleep_screen->setPalette(palette);
        sleep_screen->setGeometry(window->rect());
        sleep_screen->show();
        sleep_screen->raise();
        wake_armed = false;
        waking = false;
        wake_poll->start();
        return;
    }
    if (sleep_screen == nullptr) {
        return;
    }
    LOG_INFO(Frontend, "harbor: the console is awake");
    wake_poll->stop();
    sleep_screen->deleteLater();
    sleep_screen = nullptr;
    if (paused_for_sleep) {
        QtCommon::system->Run();
    }
    paused_for_sleep = false;
    waking = false;
}

// A controller button wakes a sleeping console once every button has been let go.
void HarborSystemKeys::PollWake() {
    // The session ended (or a new one started) while asleep: only the screen goes.
    if (!QtCommon::system->GetAppletManager().IsSleeping()) {
        paused_for_sleep = false;
        ApplySleep(false);
        return;
    }
    auto& hid = QtCommon::system->HIDCore();
    bool held = false;
    for (const auto id : {Core::HID::NpadIdType::Player1, Core::HID::NpadIdType::Handheld}) {
        const auto* controller = hid.GetEmulatedController(id);
        held = held || controller->GetNpadButtons().raw != Core::HID::NpadButton::None ||
               controller->GetHomeButtons().raw != 0 || controller->GetCaptureButtons().raw != 0;
    }
    if (!held) {
        wake_armed = true;
        return;
    }
    if (wake_armed && !waking) {
        waking = true;
        LOG_INFO(Frontend, "harbor: wake from sleep (controller)");
        QtCommon::system->GetAppletManager().PowerButtonShortPress();
    }
}

bool HarborSystemKeys::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() != QEvent::KeyPress && event->type() != QEvent::KeyRelease) {
        return QObject::eventFilter(watched, event);
    }
    const auto* key = static_cast<QKeyEvent*>(event);
    // A sleeping console wakes on any key; the key goes no further.
    if (sleep_screen != nullptr && QtCommon::system) {
        if (event->type() == QEvent::KeyPress && !key->isAutoRepeat() && !waking) {
            waking = true;
            LOG_INFO(Frontend, "harbor: wake from sleep (key)");
            QtCommon::system->GetAppletManager().PowerButtonShortPress();
        }
        return true;
    }
    // F5 is the power button, F10 force-terminates on the press. Shift+F5 is Eden's stop.
    if ((key->key() == Qt::Key_F5 || key->key() == Qt::Key_F10) &&
        key->modifiers() == Qt::NoModifier) {
        if (key->isAutoRepeat() || !QtCommon::system) {
            return true;
        }
        auto& applet_manager = QtCommon::system->GetAppletManager();
        if (key->key() == Qt::Key_F10) {
            if (event->type() == QEvent::KeyPress) {
                applet_manager.ForceTerminateApplication();
            }
        } else if (event->type() == QEvent::KeyPress) {
            power_down = true;
            power_hold->start();
        } else if (power_down) {
            power_down = false;
            // Released before the hold time: the short press.
            if (power_hold->isActive()) {
                power_hold->stop();
                applet_manager.PowerButtonShortPress();
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

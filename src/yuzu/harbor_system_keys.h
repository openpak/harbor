// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <QObject>

class QTimer;
class QWidget;

namespace InputCommon {
class InputSubsystem;
}

/**
 * The console's system buttons on the keyboard, held as long as the key is: F8 is HOME (in a HOME
 * session a long press once held 500 ms or the HOME menu's own time, a short press if released
 * before, as am/button_poller.cpp times them) and F4 is
 * Capture. They go through the emulated controller's own buttons (the virtual gamepad, on Player 1
 * and Handheld), the path every pad takes. F5 is the power button: a tap puts the console to sleep
 * or wakes it, a one-second hold opens the overlay's power menu; F10 force-terminates the application the HOME menu launched; both act on the key press, in a
 * HOME session. Installed on the application, so they work whatever has focus; the keys never
 * reach the emulated keyboard.
 */
class HarborSystemKeys final : public QObject {
public:
    explicit HarborSystemKeys(InputCommon::InputSubsystem& input_subsystem, QObject* parent);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    // F5: a tap is the power button's short press (sleep, or wake), held for a second its hold
    // (the power menu). While the console sleeps the presenter shows black, the guest is paused,
    // and any key or controller button wakes it.
    void ApplySleep(bool sleeping);
    void PollWake();
    // idle:sys dims the screen after the dimming time and brings it back on input.
    void ApplyDim(bool dimmed);

    InputCommon::InputSubsystem& input_subsystem;
    QTimer* power_hold{};
    bool power_down{};
    bool asleep{};
    QTimer* wake_poll{};
    bool wake_armed{};
    bool paused_for_sleep{};
    bool waking{};
};

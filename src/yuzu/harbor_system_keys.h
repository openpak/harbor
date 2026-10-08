// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <QObject>

namespace InputCommon {
class InputSubsystem;
}

/**
 * The console's system buttons on the keyboard, held as long as the key is: F8 is HOME (in a HOME
 * session a long press once held 500 ms or the HOME menu's own time, a short press if released
 * before, as am/button_poller.cpp times them) and F4 is
 * Capture. They go through the emulated controller's own buttons (the virtual gamepad, on Player 1
 * and Handheld), the path every pad takes. F5 is the power button held (the overlay's power menu)
 * and F10 force-terminates the application the HOME menu launched; both act on the key press, in a
 * HOME session. Installed on the application, so they work whatever has focus; the keys never
 * reach the emulated keyboard.
 */
class HarborSystemKeys final : public QObject {
public:
    explicit HarborSystemKeys(InputCommon::InputSubsystem& input_subsystem, QObject* parent);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    InputCommon::InputSubsystem& input_subsystem;
};

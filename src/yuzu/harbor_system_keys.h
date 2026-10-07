// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <QObject>

namespace InputCommon {
class InputSubsystem;
}

/**
 * The console's system buttons on the keyboard, held as long as the key is: F8 is HOME (a press under
 * 400 ms is a short press, 800 ms or more a long press, as am/button_poller.cpp times them) and F4 is
 * Capture. They go through the emulated controller's own buttons (the virtual gamepad, on Player 1
 * and Handheld), the path every pad takes. Installed on the application, so they work whatever has
 * focus; the keys never reach the emulated keyboard.
 */
class HarborSystemKeys final : public QObject {
public:
    explicit HarborSystemKeys(InputCommon::InputSubsystem& input_subsystem, QObject* parent);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    InputCommon::InputSubsystem& input_subsystem;
};

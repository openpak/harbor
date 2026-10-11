// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// The desktop console's host pieces: its own preferences, the letterbox colour, the automatic
// resolution scale and the crash-report dialogs.

#pragma once

#include <array>
#include <functional>

#include <QWidget>

#include "common/common_types.h"
#include "common/settings_enums.h"

class QLabel;
class QPushButton;

namespace Core {
class System;
}

namespace HarborDesktop {

/// The desktop console's preferences, kept in <config>/harbor-desktop.ini (not Eden's config).
/// Automatic resolution is on unless turned off here (the Harbor menu's toggle, when it has one).
bool AutoResolution();
void SetAutoResolution(bool enabled);

/// The render scale for a picture shown in a window of `pixel_width` x `pixel_height` device
/// pixels: the picture's height over what the guest draws (1080 docked, 720 handheld), in steps
/// of 0.5 from 1 to 4, rounded up to a scale Eden has. Behaviour follows the Ryujinx Harbor
/// (MIT-licensed Ryujinx base).
Settings::ResolutionSetup AutoResolutionFor(int pixel_width, int pixel_height, bool docked);

/// The bars around the picture in a window that is not 16:9: HOME's background (Basic White
/// 0xEBEBEB or Basic Black 0x2D2D2D) while HOME or an applet is in front, black behind a title.
/// Empty outside a HOME session or before set:sys runs. Behaviour follows the Ryujinx Harbor
/// (MIT-licensed Ryujinx base).
std::array<u8, 3> LetterboxColour(Core::System& system, bool* known);

/// Whether Harbor's own screens are dark: the console's colour set while it runs, its saved one
/// while it is stopped, the desktop's before the console has booted once. Behaviour follows the
/// Ryujinx Harbor (MIT-licensed Ryujinx base), App.FollowConsoleTheme.
bool DarkTheme(Core::System* running);

/// "Harbor stopped" (the console stopped on its own, with the reason) or "Harbor cannot start",
/// drawn in the console's colours, in place of the console's picture: Start again, Set up again,
/// Exit. A stop the user asked for never shows it. The graphics driver row is Android's only.
class StoppedPage final : public QWidget {
public:
    struct Actions {
        std::function<void()> start_again;
        std::function<void()> set_up_again;
        std::function<void()> exit;
    };

    StoppedPage(QWidget* parent, Actions actions);

    void ShowStopped(const QString& reason, bool dark);
    void ShowCannotStart(const QString& reason, bool dark);

private:
    void Show(const QString& heading, const QString& reason, bool dark, bool start_again);

    QLabel* heading;
    QLabel* message;
    QPushButton* start_again;
};

} // namespace HarborDesktop

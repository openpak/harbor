// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// The desktop console's host pieces: its own preferences, the letterbox colour, the automatic
// resolution scale and the crash-report dialogs.

#pragma once

#include <array>
#include <functional>
#include <memory>

#include <QWidget>

#include "common/common_types.h"
#include "common/settings_enums.h"


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

/// Harbor's own pages where the console's picture would be, while there is none: the status line
/// while the console gets ready ("Connecting to OpenPak..."), "Harbor stopped" (the console stopped
/// on its own, with the reason) and "Harbor cannot start". They are Borealis pages drawn by the
/// Harbor menu (harbor_setup/menu.h, MenuShowHostPage), the same as Android's; this widget shows
/// their pictures and passes the keyboard (Enter/Space, Escape/Backspace, the arrows, X, Y) and the
/// mouse (as touch) on. What a page asks for reaches the frontend through MenuHost::HostPageAction.
/// A stop the user asked for never shows "Harbor stopped".
class HostPages final : public QWidget {
public:
    explicit HostPages(QWidget* parent);
    ~HostPages() override;

    void ShowStatus(const QString& text);
    void ShowStopped(const QString& reason);
    void ShowCannotStart(const QString& reason);
    /// Hides the widget and ends its page.
    void Hide();
    /// The harness's screenshot: the page's last picture as a PNG.
    bool SavePicture(const QString& path) const;

protected:
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    void Show(int kind, const QString& text);
    void Touch(const QPointF& position, bool down);
    QRectF PictureRect() const;

    struct Shared;
    std::shared_ptr<Shared> shared;
};

} // namespace HarborDesktop

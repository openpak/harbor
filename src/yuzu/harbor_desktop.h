// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// The desktop console's host pieces: its own preferences, the letterbox colour, the automatic
// resolution scale and the crash-report dialogs.

#pragma once

#include <array>
#include <functional>
#include <memory>
#include <optional>

#include <QMessageBox>
#include <QString>
#include <QStringList>
#include <QWidget>

#include "common/common_types.h"
#include "common/settings_enums.h"
#include "harbor_setup/menu.h"


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
    /// Shows any host page (a dialog included) in this widget.
    void ShowPage(HarborSetup::HostPage page);
    /// Draws this widget's page again, after a dialog that stood over it went away. Does nothing
    /// when the widget has no page.
    void Reshow();
    /// Hides the widget and ends its page (`end_page` false: another page takes over at once).
    void Hide(bool end_page = true);
    /// Whether the menu has drawn a picture of the page yet.
    bool HasPicture() const;
    /// A widget of a dialog window (Ask, ProgressPage): its pages are not held back while a dialog
    /// is up, as the pages of the main window's widgets are.
    void SetOverlay(bool is_overlay) {
        overlay = is_overlay;
    }
    /// The widget that showed a page last and has not hidden it, or null.
    static HostPages* Active();
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
    void Touch(const QPointF& position, bool down);
    QRectF PictureRect() const;

    struct Shared;
    std::shared_ptr<Shared> shared;
    std::optional<HarborSetup::HostPage> page; ///< what the widget shows, while it shows one
    bool overlay = false;
};

/// Harbor's own system dialogs on the desktop, in Borealis like every Harbor screen: the console's
/// confirmation and error layout (a centred panel, its buttons along the bottom, the cyan cursor,
/// B for the first button) in a window that stands over the main window and blocks it until
/// answered. They replace the Qt message boxes and progress dialogs; Qt remains only where the
/// system owns the screen (file pickers) and for text input.
///
/// The dialog is drawn by the Harbor menu (harbor_setup/menu.h), so when that is not running (no
/// frontend host, no GL, no picture within five seconds) the calls fall back to the Qt message box
/// rather than leave the user without an answer.

/// A dialog with `buttons` (the first is B's, Cancel or Close; the last is highlighted). Returns
/// the chosen button's index, 0 when the window is closed, or -1 when no Borealis dialog could be
/// shown. `default_button` takes the focus first.
int Ask(QWidget* parent, const QString& title, const QString& text, const QStringList& buttons,
        int default_button = 0);

/// Closes the dialog Ask is showing (as if its window were closed): for a dialog whose reason goes
/// away by itself, such as the error applet when the game asks it to leave.
void CloseAsk();

/// QMessageBox's static calls, on a Borealis dialog (the icon is not drawn: the console's error
/// layout has none). Buttons are laid out with the cancelling ones first and the accepting ones
/// last whatever order they were given in.
QMessageBox::StandardButton Message(QWidget* parent, QMessageBox::Icon icon, const QString& title,
                                    const QString& text,
                                    QMessageBox::StandardButtons buttons = QMessageBox::Ok,
                                    QMessageBox::StandardButton default_button = QMessageBox::NoButton);

inline QMessageBox::StandardButton Information(
    QWidget* parent, const QString& title, const QString& text,
    QMessageBox::StandardButtons buttons = QMessageBox::Ok,
    QMessageBox::StandardButton default_button = QMessageBox::NoButton) {
    return Message(parent, QMessageBox::Information, title, text, buttons, default_button);
}
inline QMessageBox::StandardButton Warning(
    QWidget* parent, const QString& title, const QString& text,
    QMessageBox::StandardButtons buttons = QMessageBox::Ok,
    QMessageBox::StandardButton default_button = QMessageBox::NoButton) {
    return Message(parent, QMessageBox::Warning, title, text, buttons, default_button);
}
inline QMessageBox::StandardButton Critical(
    QWidget* parent, const QString& title, const QString& text,
    QMessageBox::StandardButtons buttons = QMessageBox::Ok,
    QMessageBox::StandardButton default_button = QMessageBox::NoButton) {
    return Message(parent, QMessageBox::Critical, title, text, buttons, default_button);
}
/// QMessageBox::question's default buttons are Yes and No.
inline QMessageBox::StandardButton Question(
    QWidget* parent, const QString& title, const QString& text,
    QMessageBox::StandardButtons buttons = QMessageBox::Yes | QMessageBox::No,
    QMessageBox::StandardButton default_button = QMessageBox::NoButton) {
    return Message(parent, QMessageBox::Question, title, text, buttons, default_button);
}

/// QProgressDialog's place: the status line (or, with a cancel button, a dialog) shown over the
/// main window while a job runs and the caller keeps the event loop going.
class ProgressPage {
public:
    /// `cancel` empty: no cancel button.
    ProgressPage(QWidget* parent, const QString& label, const QString& cancel = {}, int minimum = 0,
                 int maximum = 0);
    ~ProgressPage();
    ProgressPage(const ProgressPage&) = delete;
    ProgressPage& operator=(const ProgressPage&) = delete;

    void Show();
    void SetLabel(const QString& label);
    void SetRange(int minimum, int maximum);
    void SetValue(int value);
    bool Canceled() const;
    /// Takes the page away. Also done by the destructor.
    void Close();

private:
    void Update(bool force);

    struct State;
    std::unique_ptr<State> state;
};

} // namespace HarborDesktop

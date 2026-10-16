// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QGuiApplication>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QTextDocument>
#include <QTimer>
#include <QSettings>
#include <QStyleHints>
#include <QThread>
#include <QVBoxLayout>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "core/core.h"
#include "core/hle/service/am/applet.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/am/window_system.h"
#include "core/hle/service/set/system_settings_server.h"
#include "core/hle/service/sm/sm.h"
#include "harbor_setup/harbor_setup.h"
#include "harbor_setup/menu.h"
#include "video_core/host_screen.h"
#include "yuzu/harbor_desktop.h"

namespace HarborDesktop {

namespace {

QSettings& Preferences() {
    static QSettings settings(
        QString::fromStdString(Common::FS::PathToUTF8String(
            Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "harbor-desktop.ini")),
        QSettings::IniFormat);
    return settings;
}

} // namespace

bool AutoResolution() {
    return Preferences().value(QStringLiteral("auto_resolution"), true).toBool();
}

void SetAutoResolution(bool enabled) {
    Preferences().setValue(QStringLiteral("auto_resolution"), enabled);
    Preferences().sync();
}

Settings::ResolutionSetup AutoResolutionFor(int pixel_width, int pixel_height, bool docked) {
    const float shown = std::min(static_cast<float>(pixel_height), pixel_width * 9.0f / 16.0f);
    const float native = docked ? 1080.0f : 720.0f;
    const float scale = std::clamp(std::ceil(shown / native * 2.0f) / 2.0f, 1.0f, 4.0f);
    // Eden has 1x, 1.5x, 2x, 3x and 4x in this range: the next one up.
    if (scale <= 1.0f) {
        return Settings::ResolutionSetup::Res1X;
    }
    if (scale <= 1.5f) {
        return Settings::ResolutionSetup::Res3_2X;
    }
    if (scale <= 2.0f) {
        return Settings::ResolutionSetup::Res2X;
    }
    if (scale <= 3.0f) {
        return Settings::ResolutionSetup::Res3X;
    }
    return Settings::ResolutionSetup::Res4X;
}

std::array<u8, 3> LetterboxColour(Core::System& system, bool* known) {
    *known = false;
    if (!system.IsPoweredOn() || !system.GetAppletManager().IsHomeSession()) {
        return {0, 0, 0};
    }
    auto* const window_system = system.GetAppletManager().GetWindowSystem();
    if (window_system == nullptr) {
        return {0, 0, 0};
    }
    // A title in front (not obscured by an applet over it): black.
    if (const auto main = window_system->GetMainApplet();
        main && main->applet_id == Service::AM::AppletId::Application) {
        std::scoped_lock lk{main->lock};
        if (main->lifecycle_manager.GetActivityState() ==
            Service::AM::ActivityState::ForegroundVisible) {
            *known = true;
            return {0, 0, 0};
        }
    }
    const auto set_sys =
        system.ServiceManager().GetService<Service::Set::ISystemSettingsServer>("set:sys", false);
    if (!set_sys) {
        return {0, 0, 0};
    }
    Service::Set::ColorSet color{};
    if (set_sys->GetColorSetId(&color).IsError()) {
        return {0, 0, 0};
    }
    *known = true;
    if (color == Service::Set::ColorSet::BasicBlack) {
        return {0x2d, 0x2d, 0x2d};
    }
    return {0xeb, 0xeb, 0xeb};
}

bool DarkTheme(Core::System* running) {
    if (running != nullptr && running->IsPoweredOn()) {
        const auto set_sys = running->ServiceManager().GetService<Service::Set::ISystemSettingsServer>(
            "set:sys", false);
        Service::Set::ColorSet color{};
        if (set_sys && set_sys->GetColorSetId(&color).IsSuccess()) {
            return color == Service::Set::ColorSet::BasicBlack;
        }
    }
    if (const auto saved = HarborSetup::SavedConsoleDarkTheme()) {
        return *saved;
    }
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
}

namespace {

/// The widget whose page is up, and how many dialogs stand over pages (a status that arrives while
/// a dialog is up waits for it instead of replacing it).
QPointer<HostPages> active_pages;
int dialogs_up = 0;

} // namespace

struct HostPages::Shared {
    std::mutex mutex;
    QImage picture;
    QPointer<HostPages> widget;
};

HostPages::HostPages(QWidget* parent) : QWidget(parent), shared{std::make_shared<Shared>()} {
    shared->widget = this;
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(false);
    QWidget::hide();
}

HostPages::~HostPages() {
    if (page && active_pages == this) {
        HarborSetup::MenuEndHostPage();
    }
}

namespace {


} // namespace

HostPages* HostPages::Active() {
    return active_pages.data();
}

void HostPages::ShowStatus(const QString& text) {
    ShowPage({.kind = HarborSetup::HostPageKind::Status, .text = text.toStdString()});
}

void HostPages::ShowStopped(const QString& reason) {
    ShowPage({.kind = HarborSetup::HostPageKind::Stopped, .text = reason.toStdString()});
}

void HostPages::ShowCannotStart(const QString& reason) {
    ShowPage({.kind = HarborSetup::HostPageKind::CannotStart, .text = reason.toStdString()});
}

void HostPages::ShowPage(HarborSetup::HostPage next) {
    QWidget::show();
    setFocus();
    page = next;
    active_pages = this;
    if (!overlay && dialogs_up > 0) {
        return; // drawn again by Reshow once the dialog has gone
    }
    // After the window is built: the menu's host (the Harbor menu) is made later in the same
    // constructor that shows the first status.
    QTimer::singleShot(0, this, [this, next] {
        if (!isVisible()) {
            return;
        }
        auto weak = std::weak_ptr<Shared>(shared);
        HarborSetup::MenuShowHostPage(
            next,
            [weak](std::shared_ptr<const VideoCore::HostScreen::Frame> frame) {
                const auto state = weak.lock();
                if (!state || !frame) {
                    return;
                }
                QImage image(frame->rgba.data(), static_cast<int>(frame->width),
                             static_cast<int>(frame->height), QImage::Format_RGBA8888);
                {
                    std::scoped_lock lk{state->mutex};
                    state->picture = image.copy();
                }
                QMetaObject::invokeMethod(state->widget, [state] {
                    if (state->widget) {
                        state->widget->update();
                    }
                }, Qt::QueuedConnection);
            });
    });
}

void HostPages::Reshow() {
    if (page && isVisible()) {
        ShowPage(*page);
    }
}

void HostPages::Hide(bool end_page) {
    // Only the widget that drew last ends the menu's page: another one may have taken it over.
    const bool owns = active_pages == this;
    if (page && end_page && owns) {
        HarborSetup::MenuEndHostPage();
    }
    page.reset();
    if (owns) {
        active_pages = nullptr;
    }
    QWidget::hide();
}

bool HostPages::HasPicture() const {
    std::scoped_lock lk{shared->mutex};
    return !shared->picture.isNull();
}

bool HostPages::SavePicture(const QString& path) const {
    QImage picture;
    {
        std::scoped_lock lk{shared->mutex};
        picture = shared->picture;
    }
    return !picture.isNull() && picture.save(path, "PNG");
}

QRectF HostPages::PictureRect() const {
    QSizeF size;
    {
        std::scoped_lock lk{shared->mutex};
        size = shared->picture.isNull() ? QSizeF(1280, 720) : QSizeF(shared->picture.size());
    }
    const QSizeF fitted = size.scaled(QSizeF(width(), height()), Qt::KeepAspectRatio);
    return QRectF(QPointF((width() - fitted.width()) / 2, (height() - fitted.height()) / 2), fitted);
}

void HostPages::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    QImage picture;
    {
        std::scoped_lock lk{shared->mutex};
        picture = shared->picture;
    }
    if (picture.isNull()) {
        painter.fillRect(rect(), DarkTheme(nullptr) ? QColor(0x2d, 0x2d, 0x2d) : QColor(0xeb, 0xeb, 0xeb));
        return;
    }
    // The bars take the page's own paper colour.
    painter.fillRect(rect(), picture.pixelColor(0, picture.height() / 2));
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(PictureRect(), picture);
}

namespace {

std::optional<HarborSetup::HostButton> ButtonFor(int key) {
    using HarborSetup::HostButton;
    switch (key) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space:
        return HostButton::A;
    case Qt::Key_Escape:
    case Qt::Key_Backspace:
        return HostButton::B;
    case Qt::Key_X:
        return HostButton::X;
    case Qt::Key_Y:
        return HostButton::Y;
    case Qt::Key_Up:
        return HostButton::Up;
    case Qt::Key_Down:
        return HostButton::Down;
    case Qt::Key_Left:
        return HostButton::Left;
    case Qt::Key_Right:
        return HostButton::Right;
    default:
        return std::nullopt;
    }
}

} // namespace

void HostPages::keyPressEvent(QKeyEvent* event) {
    if (const auto button = ButtonFor(event->key()); button && !event->isAutoRepeat()) {
        HarborSetup::MenuHostButton(*button, true);
        return;
    }
    QWidget::keyPressEvent(event);
}

void HostPages::keyReleaseEvent(QKeyEvent* event) {
    if (const auto button = ButtonFor(event->key()); button && !event->isAutoRepeat()) {
        HarborSetup::MenuHostButton(*button, false);
        return;
    }
    QWidget::keyReleaseEvent(event);
}

void HostPages::Touch(const QPointF& position, bool down) {
    const QRectF picture = PictureRect();
    HarborSetup::MenuHostTouch(0, static_cast<float>((position.x() - picture.x()) / picture.width()),
                               static_cast<float>((position.y() - picture.y()) / picture.height()), down);
}

void HostPages::mousePressEvent(QMouseEvent* event) {
    setFocus();
    Touch(event->position(), true);
}

void HostPages::mouseMoveEvent(QMouseEvent* event) {
    if (event->buttons() & Qt::LeftButton) {
        Touch(event->position(), true);
    }
}

void HostPages::mouseReleaseEvent(QMouseEvent* event) {
    Touch(event->position(), false);
}

// ---- System dialogs and progress ----

namespace {

/// A window over the main window that holds a HostPages widget: the dialog's or the progress'.
/// While it is open it owns the menu's page; the page of the widget that was up before (the
/// loading screen, "Harbor stopped") is drawn again when it closes.
class Overlay {
public:
    explicit Overlay(QWidget* parent) {
        QWidget* window = parent ? parent->window() : QApplication::activeWindow();
        dialog = std::make_unique<QDialog>(window, Qt::Dialog | Qt::FramelessWindowHint);
        QSize size(1280, 720);
        if (window && window->isVisible()) {
            size = window->size();
            dialog->move(window->mapToGlobal(QPoint(0, 0)));
        }
        dialog->resize(size.expandedTo(QSize(640, 360)));
        auto* layout = new QVBoxLayout(dialog.get());
        layout->setContentsMargins(0, 0, 0, 0);
        pages = new HostPages(dialog.get());
        pages->SetOverlay(true);
        layout->addWidget(pages);
    }

    ~Overlay() {
        Close();
    }

    QDialog* Window() {
        return dialog.get();
    }
    HostPages* Pages() {
        return pages;
    }

    /// Takes the menu's page over: `page` is drawn in this window's widget.
    void Begin(HarborSetup::HostPage page) {
        previous = HostPages::Active();
        dialogs_up++;
        opened = true;
        pages->ShowPage(std::move(page));
    }

    void Close() {
        if (!opened) {
            return;
        }
        opened = false;
        dialogs_up--;
        if (previous && previous != pages) {
            pages->Hide(false);
            previous->Reshow();
        } else {
            pages->Hide();
        }
        dialog->hide();
    }

private:
    std::unique_ptr<QDialog> dialog;
    HostPages* pages = nullptr;
    QPointer<HostPages> previous;
    bool opened = false;
};

/// The text a Qt message box would have drawn, without its markup.
std::string PlainText(const QString& text) {
    if (Qt::mightBeRichText(text)) {
        QTextDocument document;
        document.setHtml(text);
        return document.toPlainText().trimmed().toStdString();
    }
    return text.toStdString();
}

/// How a message box's standard buttons are laid out: the cancelling ones first (B takes the
/// first), the accepting ones last (the highlighted one).
struct ButtonSpec {
    QMessageBox::StandardButton button;
    const char* label;
};
constexpr ButtonSpec ButtonOrder[] = {
    {QMessageBox::Cancel, "Cancel"},
    {QMessageBox::Close, "Close"},
    {QMessageBox::Abort, "Abort"},
    {QMessageBox::Discard, "Discard"},
    {QMessageBox::No, "No"},
    {QMessageBox::NoToAll, "No to all"},
    {QMessageBox::Ignore, "Ignore"},
    {QMessageBox::Reset, "Reset"},
    {QMessageBox::RestoreDefaults, "Restore defaults"},
    {QMessageBox::Help, "Help"},
    {QMessageBox::Retry, "Retry"},
    {QMessageBox::Apply, "Apply"},
    {QMessageBox::Open, "Open"},
    {QMessageBox::Save, "Save"},
    {QMessageBox::SaveAll, "Save all"},
    {QMessageBox::YesToAll, "Yes to all"},
    {QMessageBox::Yes, "Yes"},
    {QMessageBox::Ok, "OK"},
};

} // namespace

namespace {
QPointer<QDialog> ask_window; ///< the window of the dialog Ask is showing
} // namespace

void CloseAsk() {
    if (ask_window) {
        ask_window->done(0);
    }
}

int Ask(QWidget* parent, const QString& title, const QString& text, const QStringList& buttons,
        int default_button) {
    // Qt's windows belong to the GUI thread; from any other the Qt box (which has the same limit)
    // is what the caller gets.
    if (buttons.isEmpty() || !HarborSetup::MenuHostReady() ||
        QThread::currentThread() != QCoreApplication::instance()->thread()) {
        return -1;
    }
    Overlay overlay(parent);
    QPointer<QDialog> window = overlay.Window();
    const auto answered = std::make_shared<std::atomic_bool>(false);

    HarborSetup::HostPage page{.kind = HarborSetup::HostPageKind::Dialog,
                               .text = PlainText(text),
                               .title = title.toStdString()};
    for (const auto& button : buttons) {
        page.buttons.push_back(button.toStdString());
    }
    page.default_button = default_button;
    // From the menu's thread: the answer is the GUI thread's, once.
    page.answered = [window, answered](int index) {
        if (answered->exchange(true)) {
            return;
        }
        QMetaObject::invokeMethod(
            qApp,
            [window, index] {
                if (window) {
                    window->done(index + 1);
                }
            },
            Qt::QueuedConnection);
    };
    overlay.Begin(std::move(page));
    ask_window = window;

    // The dialog is the menu's picture: when none comes (no GL for the menu), Qt answers instead.
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, overlay.Window(), [&overlay] {
        if (!overlay.Pages()->HasPicture()) {
            LOG_WARNING(Frontend, "harbor: the dialog could not be drawn; Qt's message box answers");
            overlay.Window()->done(-1);
        }
    });
    watchdog.start(5000);

    const int result = overlay.Window()->exec();
    watchdog.stop();
    overlay.Close();
    if (result < 0) {
        return -1;
    }
    // Closed from the window itself (0): B, the first button.
    return result == 0 ? 0 : result - 1;
}

QMessageBox::StandardButton Message(QWidget* parent, QMessageBox::Icon icon, const QString& title,
                                    const QString& text, QMessageBox::StandardButtons buttons,
                                    QMessageBox::StandardButton default_button) {
    if (!buttons) {
        buttons = QMessageBox::Ok;
    }
    QStringList labels;
    std::vector<QMessageBox::StandardButton> order;
    for (const auto& spec : ButtonOrder) {
        if (buttons.testFlag(spec.button)) {
            labels << QCoreApplication::translate("HarborDesktop", spec.label);
            order.push_back(spec.button);
        }
    }
    int focus = 0;
    for (std::size_t i = 0; i < order.size(); i++) {
        if (order[i] == default_button) {
            focus = static_cast<int>(i);
        }
    }
    const int index = Ask(parent, title, text, labels, focus);
    if (index >= 0 && index < static_cast<int>(order.size())) {
        return order[static_cast<std::size_t>(index)];
    }
    // No Borealis dialog could be drawn: the Qt box still asks.
    QMessageBox box(icon, title, text, buttons, parent);
    if (default_button != QMessageBox::NoButton) {
        box.setDefaultButton(default_button);
    }
    return static_cast<QMessageBox::StandardButton>(box.exec());
}

struct ProgressPage::State {
    QPointer<QWidget> parent;
    QString label;
    QString cancel;
    int minimum = 0;
    int maximum = 0;
    int value = 0;
    std::unique_ptr<Overlay> overlay;
    std::shared_ptr<std::atomic_bool> canceled = std::make_shared<std::atomic_bool>(false);
    std::chrono::steady_clock::time_point last{};
};

ProgressPage::ProgressPage(QWidget* parent, const QString& label, const QString& cancel, int minimum,
                           int maximum)
    : state(std::make_unique<State>()) {
    state->parent = parent;
    state->label = label;
    state->cancel = cancel;
    state->minimum = minimum;
    state->maximum = maximum;
    state->value = minimum;
}

ProgressPage::~ProgressPage() {
    Close();
}

void ProgressPage::Show() {
    if (state->overlay || !HarborSetup::MenuHostReady() ||
        QThread::currentThread() != QCoreApplication::instance()->thread()) {
        return;
    }
    state->overlay = std::make_unique<Overlay>(state->parent);
    state->overlay->Window()->setWindowModality(Qt::WindowModal);
    state->overlay->Window()->show();
    state->overlay->Begin({.kind = HarborSetup::HostPageKind::Status});
    Update(true);
}

void ProgressPage::SetLabel(const QString& label) {
    state->label = label;
    Update(true);
}

void ProgressPage::SetRange(int minimum, int maximum) {
    state->minimum = minimum;
    state->maximum = maximum;
    Update(true);
}

void ProgressPage::SetValue(int value) {
    state->value = value;
    Update(value >= state->maximum);
}

bool ProgressPage::Canceled() const {
    return state->canceled->load();
}

void ProgressPage::Close() {
    state->overlay.reset();
}

void ProgressPage::Update(bool force) {
    if (!state->overlay) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!force && now - state->last < std::chrono::milliseconds(150)) {
        return;
    }
    state->last = now;
    QString text = state->label;
    if (state->maximum > state->minimum) {
        const int percent = std::clamp(
            static_cast<int>(100.0 * (state->value - state->minimum) / (state->maximum - state->minimum)),
            0, 100);
        text += QStringLiteral(" %1%").arg(percent);
    }
    HarborSetup::HostPage page{.kind = HarborSetup::HostPageKind::Status, .text = PlainText(text)};
    if (!state->cancel.isEmpty()) {
        page.kind = HarborSetup::HostPageKind::Dialog;
        page.buttons = {state->cancel.toStdString()};
        page.answered = [canceled = state->canceled](int) { canceled->store(true); };
    }
    state->overlay->Pages()->ShowPage(std::move(page));
    // As QProgressDialog does: the page's picture is painted from the event loop.
    QCoreApplication::processEvents();
}

} // namespace HarborDesktop

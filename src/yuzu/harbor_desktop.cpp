// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <algorithm>
#include <cmath>
#include <mutex>
#include <optional>

#include <QGuiApplication>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QTimer>
#include <QSettings>
#include <QStyleHints>

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
    if (isVisible()) {
        HarborSetup::MenuEndHostPage();
    }
}

void HostPages::ShowStatus(const QString& text) {
    Show(static_cast<int>(HarborSetup::HostPageKind::Status), text);
}

void HostPages::ShowStopped(const QString& reason) {
    Show(static_cast<int>(HarborSetup::HostPageKind::Stopped), reason);
}

void HostPages::ShowCannotStart(const QString& reason) {
    Show(static_cast<int>(HarborSetup::HostPageKind::CannotStart), reason);
}

void HostPages::Show(int kind, const QString& text) {
    QWidget::show();
    setFocus();
    // After the window is built: the menu's host (the Harbor menu) is made later in the same
    // constructor that shows the first status.
    QTimer::singleShot(0, this, [this, kind, text] {
        if (!isVisible()) {
            return;
        }
        auto weak = std::weak_ptr<Shared>(shared);
        HarborSetup::MenuShowHostPage(
            {.kind = static_cast<HarborSetup::HostPageKind>(kind), .text = text.toStdString()},
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

void HostPages::Hide() {
    if (isVisible()) {
        HarborSetup::MenuEndHostPage();
    }
    QWidget::hide();
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

} // namespace HarborDesktop

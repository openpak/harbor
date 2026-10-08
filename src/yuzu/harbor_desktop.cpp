// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <algorithm>
#include <cmath>
#include <mutex>

#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStyleHints>
#include <QVBoxLayout>

#include "common/fs/path_util.h"
#include "common/harbor_crash.h"
#include "common/logging.h"
#include "core/core.h"
#include "core/hle/service/am/applet.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/am/window_system.h"
#include "core/hle/service/set/system_settings_server.h"
#include "core/hle/service/sm/sm.h"
#include "harbor_setup/harbor_setup.h"
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

/// One report: its summary, the text under "Show Details...", and Save a copy / Delete / Keep.
void ShowReport(QWidget* parent, const Common::HarborCrash::Report& report, const QString& title) {
    QMessageBox box(parent);
    box.setIcon(QMessageBox::Information);
    box.setWindowTitle(title);
    box.setText(QObject::tr("Harbor saved a crash report: %1 (%2).")
                    .arg(QString::fromStdString(report.summary), QString::fromStdString(report.when)));
    box.setInformativeText(
        QObject::tr("The report is kept on this computer only; nothing has been sent. Show Details "
                    "to read it, save a copy to share it yourself, or delete it."));
    box.setDetailedText(QString::fromStdString(Common::HarborCrash::Read(report)));
    auto* save = box.addButton(QObject::tr("Save a copy..."), QMessageBox::ActionRole);
    auto* delete_button = box.addButton(QObject::tr("Delete"), QMessageBox::DestructiveRole);
    auto* keep = box.addButton(QObject::tr("Keep"), QMessageBox::RejectRole);
    box.setDefaultButton(keep);
    box.exec();

    if (box.clickedButton() == save) {
        const QString target = QFileDialog::getSaveFileName(
            parent, QObject::tr("Save a copy of the crash report"),
            QDir::home().filePath(QString::fromStdString(report.name)),
            QObject::tr("Text files (*.txt)"));
        if (!target.isEmpty()) {
            QFile::remove(target);
            const bool copied = QFile::copy(
                QString::fromStdString(Common::FS::PathToUTF8String(report.path)), target);
            LOG_INFO(Frontend, "harbor: crash report {} {}", report.name,
                     copied ? "saved as a copy" : "could not be copied");
        }
    } else if (box.clickedButton() == delete_button) {
        Common::HarborCrash::Delete(report);
        LOG_INFO(Frontend, "harbor: crash report {} deleted", report.name);
    }
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

void AskCrashConsent(QWidget* parent) {
    QMessageBox box(parent);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(QObject::tr("Crash reports"));
    box.setText(QObject::tr("Keep crash reports on this computer?"));
    box.setInformativeText(QObject::tr(
        "If Harbor or a game crashes, Harbor can save a report on this computer: what crashed and "
        "the last lines of its log, with account ids, names, tokens and typed text removed. "
        "Nothing is sent anywhere. At the next start Harbor offers the report: you can read it, "
        "save a copy to share it yourself, or delete it."));
    auto* keep = box.addButton(QObject::tr("Keep reports"), QMessageBox::AcceptRole);
    auto* decline = box.addButton(QObject::tr("Don't keep"), QMessageBox::RejectRole);
    box.setDefaultButton(decline);
    box.exec();
    // Closed without an answer: asked again next time; until then nothing is kept.
    if (box.clickedButton() == keep || box.clickedButton() == decline) {
        const bool accepted = box.clickedButton() == keep;
        Common::HarborCrash::SetConsent(accepted);
        LOG_INFO(Frontend, "harbor: crash reports {}", accepted ? "kept from now on" : "not kept");
    }
}

void OfferLastCrashReport(QWidget* parent) {
    const auto report = Common::HarborCrash::TakeOffer();
    if (!report) {
        return;
    }
    LOG_INFO(Frontend, "harbor: offering crash report {}", report->name);
    ShowReport(parent, *report, QObject::tr("Harbor crashed last time"));
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

StoppedPage::StoppedPage(QWidget* parent, Actions actions) : QWidget(parent) {
    setObjectName(QStringLiteral("HarborStoppedPage"));
    setAttribute(Qt::WA_StyledBackground, true);
    heading = new QLabel(this);
    heading->setObjectName(QStringLiteral("heading"));
    message = new QLabel(this);
    message->setWordWrap(true);
    message->setTextInteractionFlags(Qt::TextSelectableByMouse);
    start_again = new QPushButton(tr("Start again"), this);
    auto* setup_button = new QPushButton(tr("Set up again"), this);
    auto* exit_button = new QPushButton(tr("Exit"), this);
    connect(start_again, &QPushButton::clicked, this, [run = actions.start_again] { run(); });
    connect(setup_button, &QPushButton::clicked, this, [run = actions.set_up_again] { run(); });
    connect(exit_button, &QPushButton::clicked, this, [run = actions.exit] { run(); });

    auto* column = new QVBoxLayout;
    column->addWidget(heading);
    column->addWidget(message);
    column->addSpacing(8);
    for (auto* button : {start_again, setup_button, exit_button}) {
        button->setMinimumHeight(40);
        column->addWidget(button);
    }
    auto* holder = new QWidget(this);
    holder->setLayout(column);
    holder->setMaximumWidth(560);
    auto* outer = new QVBoxLayout(this);
    outer->addStretch();
    auto* row = new QHBoxLayout;
    row->addStretch();
    row->addWidget(holder, 1);
    row->addStretch();
    outer->addLayout(row);
    outer->addStretch();
    hide();
}

void StoppedPage::ShowStopped(const QString& reason, bool dark) {
    Show(tr("Harbor stopped"), reason, dark, true);
}

void StoppedPage::ShowCannotStart(const QString& reason, bool dark) {
    Show(tr("Harbor cannot start"), reason, dark, true);
}

void StoppedPage::Show(const QString& title, const QString& reason, bool dark, bool can_start) {
    // HOME's colours: Basic White paper #ebebeb with #2d2d2d ink, Basic Black the other way round.
    const QString paper = dark ? QStringLiteral("#2d2d2d") : QStringLiteral("#ebebeb");
    const QString ink = dark ? QStringLiteral("#ebebeb") : QStringLiteral("#2d2d2d");
    const QString button = dark ? QStringLiteral("#464646") : QStringLiteral("#ffffff");
    const QString accent = QStringLiteral("#00c3e3");
    setStyleSheet(QStringLiteral(
                      "#HarborStoppedPage { background: %1; }"
                      "#HarborStoppedPage QLabel { color: %2; font-size: 16px; background: transparent; }"
                      "#HarborStoppedPage QLabel#heading { font-size: 26px; font-weight: bold; }"
                      "#HarborStoppedPage QPushButton { color: %2; background: %3; border: 2px solid %3;"
                      " border-radius: 6px; font-size: 16px; padding: 6px 16px; text-align: left; }"
                      "#HarborStoppedPage QPushButton:focus, #HarborStoppedPage QPushButton:hover"
                      " { border-color: %4; }")
                      .arg(paper, ink, button, accent));
    heading->setText(title);
    message->setText(reason);
    start_again->setVisible(can_start);
    show();
    start_again->setFocus();
}

} // namespace HarborDesktop

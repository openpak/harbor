// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <cstdlib>
#include <filesystem>

#include <QApplication>
#include <QDateTime>
#include <QFileDialog>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressDialog>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "core/harbor_calibration/calibration.h"
#include "qt_common/util/compress.h"
#include "yuzu/harbor_console_menu.h"
#include "yuzu/harbor_desktop.h"

namespace HarborConsoleMenu {

namespace {

constexpr int MinimumPassphrase = 8;

// Test hook: HARBOR_CONSOLE_BACKUP_FILE=<path> answers the backup and restore file pickers.
QString PickedFile() {
    const char* env = std::getenv("HARBOR_CONSOLE_BACKUP_FILE");
    return env != nullptr ? QString::fromUtf8(env) : QString{};
}

std::filesystem::path NandDir() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir);
}

QString Ask(QWidget* parent, const QString& title, const QString& label, bool* ok) {
    return QInputDialog::getText(parent, title, label, QLineEdit::Password, QString{}, ok);
}

/// A progress page (Borealis) for a zip of the NAND, kept responsive from the copy's own callback.
QtCommon::QtProgressCallback Progress(HarborDesktop::ProgressPage& dialog) {
    return [&dialog](std::size_t total, std::size_t done) {
        dialog.SetValue(total == 0 ? 0 : static_cast<int>(done * 100 / total));
        QApplication::processEvents();
        return !dialog.Canceled();
    };
}

} // namespace

void BackUp(QWidget* parent, const WhileStopped& while_stopped) {
    const QString title = QObject::tr("Back up console");
    bool ok = false;
    const QString passphrase =
        Ask(parent, title,
            QObject::tr("The backup carries this console's device key, locked with a passphrase. "
                        "Restoring it on any computer needs this passphrase; Harbor never "
                        "keeps it.\n\nPassphrase (at least %1 characters):")
                .arg(MinimumPassphrase),
            &ok);
    if (!ok) {
        return;
    }
    if (passphrase.size() < MinimumPassphrase) {
        HarborDesktop::Warning(parent, title,
                             QObject::tr("The passphrase needs at least %1 characters.")
                                 .arg(MinimumPassphrase));
        return;
    }
    if (Ask(parent, title, QObject::tr("The passphrase again:"), &ok) != passphrase || !ok) {
        if (ok) {
            HarborDesktop::Warning(parent, title, QObject::tr("The passphrases differ."));
        }
        return;
    }
    QString target = PickedFile();
    if (target.isEmpty()) {
        target = QFileDialog::getSaveFileName(
            parent, title,
            QStringLiteral("harbor-console-%1.zip")
                .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmm"))),
            QObject::tr("Harbor console backup (*.zip)"));
    }
    if (target.isEmpty()) {
        return;
    }

    bool done = false;
    while_stopped([&] {
        if (!Harbor::WriteDeviceKeyBackup(passphrase.toStdString())) {
            return;
        }
        HarborDesktop::ProgressPage progress(parent, QObject::tr("Backing up the console..."),
                                             QObject::tr("Cancel"), 0, 100);
        progress.Show();
        done = QtCommon::Compress::compressDir(
            target, QString::fromStdString(NandDir().string()),
            QtCommon::Compress::Options(QtCommon::Compress::Options::Storage), Progress(progress));
        Harbor::RemoveDeviceKeyBackup();
    });
    LOG_INFO(Frontend, "harbor: console backup {}", done ? "written" : "failed");
    if (done) {
        HarborDesktop::Information(parent, title, QObject::tr("The console is backed up."));
    } else {
        HarborDesktop::Critical(parent, title, QObject::tr("The backup could not be written."));
    }
}

void Restore(QWidget* parent, const WhileStopped& while_stopped) {
    const QString title = QObject::tr("Restore console");
    QString source = PickedFile();
    if (source.isEmpty()) {
        source = QFileDialog::getOpenFileName(parent, title, QString{},
                                              QObject::tr("Harbor console backup (*.zip)"));
    }
    if (source.isEmpty()) {
        return;
    }
    if (HarborDesktop::Warning(parent, title,
                             QObject::tr("The backup replaces this console's system memory: its "
                                         "users, saves and settings. Continue?"),
                             QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) {
        return;
    }

    QString outcome;
    while_stopped([&] {
        namespace fs = std::filesystem;
        const fs::path nand = NandDir();
        const fs::path incoming = fs::path{nand.string() + ".restore"};
        const fs::path previous = fs::path{nand.string() + ".previous"};
        std::error_code ec;
        fs::remove_all(incoming, ec);
        fs::create_directories(incoming, ec);
        {
            HarborDesktop::ProgressPage progress(parent, QObject::tr("Restoring the console..."),
                                                 QObject::tr("Cancel"), 0, 100);
            progress.Show();
            const auto files = QtCommon::Compress::extractDir(
                source, QString::fromStdString(incoming.string()), Progress(progress));
            if (files.isEmpty() || !fs::exists(incoming / "calibration" / "prodinfo.bin")) {
                fs::remove_all(incoming, ec);
                outcome = QObject::tr("This is not a Harbor console backup.");
                return;
            }
        }
        fs::remove_all(previous, ec);
        fs::rename(nand, previous, ec);
        if (ec) {
            fs::remove_all(incoming, ec);
            outcome = QObject::tr("The console's system memory could not be replaced.");
            return;
        }
        fs::rename(incoming, nand, ec);
        if (ec) {
            fs::rename(previous, nand, ec);
            outcome = QObject::tr("The console's system memory could not be replaced.");
            return;
        }
        fs::remove_all(previous, ec);
        Harbor::Calibration::Reload();

        // Its device key: back in this host's password store, or the console cannot prove
        // itself.
        while (Harbor::DeviceKeyRestorePending()) {
            bool ok = false;
            const QString passphrase = Ask(
                parent, title, QObject::tr("The backup's passphrase:"), &ok);
            if (!ok) {
                outcome = QObject::tr("The console is restored, but its device key is still "
                                      "locked: restore the backup again with its passphrase, or "
                                      "make this a new console.");
                return;
            }
            const auto result = Harbor::RestoreDeviceKey(passphrase.toStdString());
            if (result == Harbor::RestoreResult::WrongPassphrase) {
                HarborDesktop::Warning(parent, title, QObject::tr("That passphrase does not open "
                                                                "this backup."));
            } else if (result != Harbor::RestoreResult::Restored) {
                outcome = QObject::tr("The device key could not be kept on this computer.");
                return;
            }
        }
        // A backup restored on the computer it came from: the key never left; the sealed copy
        // goes either way.
        Harbor::RemoveDeviceKeyBackup();
        outcome = QObject::tr("The console is restored.");
    });
    LOG_INFO(Frontend, "harbor: console restore: {}", outcome.toStdString());
    HarborDesktop::Information(parent, title, outcome);
}

std::string MakeNew(const WhileStopped& while_stopped) {
    // Confirmed in the Harbor menu (a Borealis dialog); the outcome is a line on the console's
    // screen once HOME is back.
    bool made = false;
    while_stopped([&] { made = Harbor::MakeNewConsole(); });
    LOG_INFO(Frontend, "harbor: new console identity {}", made ? "made" : "not made");
    return made ? "This is a new console. Link each user's Nintendo Account again in System Settings › "
                  "Users."
                : "No new identity could be made.";
}

void WarnIfLocked(QWidget* parent) {
    using Keys = Harbor::Calibration::KeyState;
    const Keys keys = Harbor::Calibration::Get().Keys();
    if (keys != Keys::NotOnThisHost && keys != Keys::BackupPending) {
        return;
    }
    LOG_WARNING(Frontend, "harbor: the console's keys are not on this computer; told the user");
    if (std::getenv("HARBOR_CONTROL") != nullptr) {
        return; // test runs: the log line is the signal, no dialog in the way
    }
    HarborDesktop::Warning(
        parent, QObject::tr("This console's keys"),
        keys == Keys::BackupPending
            ? QObject::tr("This console was restored from a backup and its keys are still locked. "
                          "Use Harbor > Restore console with the backup's passphrase. Until then "
                          "it does not sign in to OpenPak.")
            : QObject::tr("This console's keys are not on this computer, so it does not sign in "
                          "to OpenPak. Restore it from a backup (Harbor > Restore console), or "
                          "make it a new console (Harbor > Make this a new console): its users then "
                          "link their Nintendo Accounts again."));
}

} // namespace HarborConsoleMenu

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QAbstractButton>
#include <QFileDialog>
#include <QInputDialog>
#include <QMessageBox>
#include <QProgressDialog>

#include "libqt_common.h"
#include "qt_common/abstract/frontend.h"
#include "qt_common/abstract/progress.h"
#include "qt_common/qt_common.h"

namespace QtCommon::Frontend {

StandardButton ShowMessage(Icon icon, const QString& title, const QString& text,
                           StandardButtons buttons, QObject* parent) {
    // Harbor's dialogs are Borealis (harbor_desktop.h); Qt's box only answers when that cannot draw.
    const auto result = HarborDesktop::Message(
        qobject_cast<QWidget*>(parent), QMessageBox::Icon(int(icon)), title, text,
        QMessageBox::StandardButtons(int(buttons)));
    return StandardButton(int(result));
}

WidgetsProgressDialog::WidgetsProgressDialog(const QString& labelText,
                                             const QString& cancelButtonText, int minimum,
                                             int maximum, QWidget* parent, Qt::WindowFlags f)
    : QtProgressDialog(labelText, cancelButtonText, minimum, maximum, parent, f),
      m_page(std::make_unique<HarborDesktop::ProgressPage>(parent, labelText, cancelButtonText,
                                                           minimum, maximum)),
      m_created(std::chrono::steady_clock::now()), m_minimum(minimum), m_maximum(maximum) {}

void WidgetsProgressDialog::Update() {
    if (!m_shown && !m_closed &&
        std::chrono::steady_clock::now() - m_created >=
            std::chrono::milliseconds(m_minimum_duration)) {
        m_shown = true;
        m_page->Show();
    }
}

bool WidgetsProgressDialog::wasCanceled() const {
    return m_page->Canceled();
}

void WidgetsProgressDialog::setWindowModality(Qt::WindowModality) {}

void WidgetsProgressDialog::setMinimumDuration(int durationMs) {
    m_minimum_duration = durationMs;
}

void WidgetsProgressDialog::setAutoClose(bool) {}

void WidgetsProgressDialog::setAutoReset(bool) {}

void WidgetsProgressDialog::setTitle(QString) {}

void WidgetsProgressDialog::setLabelText(QString text) {
    m_page->SetLabel(text);
}

void WidgetsProgressDialog::setMinimum(int min) {
    m_minimum = min;
    m_page->SetRange(m_minimum, m_maximum);
}

void WidgetsProgressDialog::setMaximum(int max) {
    m_maximum = max;
    m_page->SetRange(m_minimum, m_maximum);
}

void WidgetsProgressDialog::setValue(int value) {
    Update();
    m_page->SetValue(value);
}

bool WidgetsProgressDialog::close() {
    m_closed = true;
    m_page->Close();
    return true;
}

void WidgetsProgressDialog::show() {
    if (m_closed) {
        return;
    }
    m_shown = true;
    m_page->Show();
}

std::unique_ptr<QtProgressDialog> newProgressDialog(const QString& labelText,
                                                    const QString& cancelButtonText, int minimum,
                                                    int maximum, Qt::WindowFlags f) {
    return std::make_unique<WidgetsProgressDialog>(labelText, cancelButtonText, minimum, maximum,
                                                   (QWidget*)rootObject, f);
}

QtProgressDialog* newProgressDialogPtr(const QString& labelText, const QString& cancelButtonText,
                                       int minimum, int maximum, Qt::WindowFlags f) {
    return new WidgetsProgressDialog(labelText, cancelButtonText, minimum, maximum,
                                     (QWidget*)rootObject, f);
}

int Choice(const QString& title, const QString& caption, const QStringList& options) {
    // The options along the bottom, Cancel first (B); -1 is Cancel.
    QStringList buttons{QObject::tr("Cancel")};
    buttons << options;
    const int index = HarborDesktop::Ask(qobject_cast<QWidget*>(rootObject), title, caption, buttons,
                                         options.isEmpty() ? 0 : 1);
    if (index >= 0) {
        return index - 1;
    }

    // No Borealis dialog could be drawn: Qt's box asks.
    QMessageBox box(rootObject);
    box.setText(caption);
    box.setWindowTitle(title);

    for (const QString& opt : options) {
        box.addButton(opt, QMessageBox::AcceptRole);
    }

    box.addButton(QMessageBox::Cancel);

    box.exec();
    auto button = box.clickedButton();
    return options.indexOf(button->text());
}

const QString GetTextInput(const QString& title, const QString& caption,
                           const QString& defaultText) {
    return QInputDialog::getText(rootObject, title, caption, QLineEdit::Normal, defaultText);
}

} // namespace QtCommon::Frontend

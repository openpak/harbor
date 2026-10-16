// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "qt_common/abstract/progress.h"

#include <chrono>
#include <memory>

#include "yuzu/harbor_desktop.h"

namespace QtCommon::Frontend {

class WidgetsProgressDialog : public QtProgressDialog {
    Q_OBJECT
public:
    WidgetsProgressDialog(const QString& labelText, const QString& cancelButtonText, int minimum,
                          int maximum, QWidget* parent = nullptr, Qt::WindowFlags f = {});

    bool wasCanceled() const override;
    void setWindowModality(Qt::WindowModality modality) override;
    void setMinimumDuration(int durationMs) override;
    void setAutoClose(bool autoClose) override;
    void setAutoReset(bool autoReset) override;

public slots:
    void setTitle(QString title) override;
    void setLabelText(QString text) override;
    void setMinimum(int min) override;
    void setMaximum(int max) override;
    void setValue(int value) override;
    bool close() override;
    void show() override;

private:
    // Harbor's progress is a Borealis page over the main window (harbor_desktop.h), shown once the
    // minimum duration has passed, as QProgressDialog did.
    void Update();

    std::unique_ptr<HarborDesktop::ProgressPage> m_page;
    std::chrono::steady_clock::time_point m_created;
    int m_minimum_duration = 100;
    int m_minimum = 0;
    int m_maximum = 0;
    bool m_shown = false;
    bool m_closed = false;
};

} // namespace QtCommon::Frontend

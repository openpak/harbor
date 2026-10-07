// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <functional>

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;

/// The Harbor menu's "Frame generation" page on desktop: the user's own Lossless.dll (Lossless
/// Scaling's shaders, read as data and never loaded), then the on/off and mode settings Android's
/// quick settings have. Off by default; the settings are console-wide.
class HarborFrameGenDialog final : public QDialog {
    Q_OBJECT

public:
    /// `change_dll` runs a change of the DLL with the console stopped, then boots it again (the
    /// renderer reads the DLL when it starts). `save` writes the configuration.
    HarborFrameGenDialog(QWidget* parent, std::function<void(std::function<void()>)> change_dll,
                         std::function<void()> save);

private:
    void ChooseDll();
    void RemoveDll();
    void Refresh();
    void Store();

    std::function<void(std::function<void()>)> change_dll;
    std::function<void()> save;

    QLabel* status;
    QPushButton* choose;
    QPushButton* remove;
    QCheckBox* enabled;
    QComboBox* multiplier;
    QComboBox* target_rate;
    QComboBox* flow_scale;
    bool gpu_ok;
    bool refreshing = false;
};

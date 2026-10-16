// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Based on Eden's loading screen (SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project,
// GPL-3.0-or-later; yuzu Emulator Project, GPL-2.0-or-later), whose Qt progress bar, banner and logo
// are gone: Harbor's loading screen is the Borealis status page the console's boot shows.

#pragma once

#include <chrono>
#include <cstddef>

#include <QString>
#include <QWidget>
#include <QtGlobal>

namespace Loader {
class AppLoader;
}

namespace VideoCore {
enum class LoadCallbackStage;
}

namespace HarborDesktop {
class HostPages;
}

/// What the main window shows while a game or HOME boots and its shaders load: a status page of
/// Harbor's Borealis (harbor_desktop.h, the page "Connecting to OpenPak..." uses) with the stage
/// as its line ("Loading...", "Loading shaders 120 / 800", "Launching..."). Drawn by the Harbor
/// menu, so it looks the same as the Android loading page.
class LoadingScreen : public QWidget {
    Q_OBJECT

public:
    explicit LoadingScreen(QWidget* parent = nullptr);

    ~LoadingScreen();

    /// Call before showing the loading screen: starts again from "Loading...".
    void Prepare(Loader::AppLoader& loader);

    /// After the loading screen is hidden: nothing is held any more (kept for the owner's call).
    void Clear();

    /// Slot used to update the stage line
    void OnLoadProgress(VideoCore::LoadCallbackStage stage, std::size_t value, std::size_t total);

    /// Hides the LoadingScreen and tells the owner (Hidden)
    void OnLoadComplete();

    /// Showing and hiding put the status page up and take it away.
    void setVisible(bool visible) override;

signals:
    void LoadProgress(VideoCore::LoadCallbackStage stage, std::size_t value, std::size_t total);
    /// Signals that this widget is completely hidden now and should be replaced with the other
    /// widget
    void Hidden();

private:
    void SetLine(const QString& text, bool force);

    HarborDesktop::HostPages* pages = nullptr;
    bool up = false;
    QString line;
    std::chrono::steady_clock::time_point last_update;
};

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
Q_DECLARE_METATYPE(VideoCore::LoadCallbackStage);
#endif

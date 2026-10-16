// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Based on Eden's loading screen (SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project,
// GPL-3.0-or-later; yuzu Emulator Project, GPL-2.0-or-later).

#include <QVBoxLayout>

#include "video_core/rasterizer_interface.h"
#include "yuzu/harbor_desktop.h"
#include "yuzu/loading_screen.h"

LoadingScreen::LoadingScreen(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    pages = new HarborDesktop::HostPages(this);
    layout->addWidget(pages);
    connect(this, &LoadingScreen::LoadProgress, this, &LoadingScreen::OnLoadProgress,
            Qt::QueuedConnection);
    qRegisterMetaType<VideoCore::LoadCallbackStage>();
}

LoadingScreen::~LoadingScreen() = default;

void LoadingScreen::Prepare(Loader::AppLoader&) {
    SetLine(tr("Loading..."), true);
}

void LoadingScreen::Clear() {}

void LoadingScreen::setVisible(bool visible) {
    QWidget::setVisible(visible);
    if (visible == up) {
        return;
    }
    up = visible;
    if (visible) {
        pages->ShowStatus(line.isEmpty() ? tr("Loading...") : line);
    } else {
        pages->Hide();
    }
}

void LoadingScreen::SetLine(const QString& text, bool force) {
    using namespace std::chrono;
    const auto now = steady_clock::now();
    // The page is drawn again for each change: shader progress moves a few times a second.
    if (text == line || (!force && now - last_update < milliseconds{250})) {
        return;
    }
    line = text;
    last_update = now;
    if (up) {
        pages->ShowStatus(line);
    }
}

void LoadingScreen::OnLoadProgress(VideoCore::LoadCallbackStage stage, std::size_t value,
                                   std::size_t total) {
    switch (stage) {
    case VideoCore::LoadCallbackStage::Build:
        SetLine(tr("Loading shaders %1 / %2").arg(value).arg(total), false);
        break;
    case VideoCore::LoadCallbackStage::Complete:
        SetLine(tr("Launching..."), true);
        break;
    default:
        SetLine(tr("Loading..."), true);
        break;
    }
}

void LoadingScreen::OnLoadComplete() {
    hide();
    emit Hidden();
}

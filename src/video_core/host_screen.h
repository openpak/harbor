// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Harbor's host screen: a picture the frontend draws (the Harbor menu) shown in place of the
// console's own picture, through the same presenter, scaling and letterboxing. While it is shown
// the console keeps running; its picture comes back when the host screen is hidden.

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "common/common_types.h"

namespace Tegra {
struct FramebufferConfig;
}

namespace VideoCore::HostScreen {

/// One picture: tightly packed RGBA bytes (R first), top row first.
struct Frame {
    u32 width{};
    u32 height{};
    std::vector<u8> rgba;
};

/// The framebuffer address the presenter sees for the host screen (never a guest address).
constexpr u64 HostAddress = ~u64{0};
/// ... and for the host overlay: a picture with alpha drawn over the console's (the FPS line).
constexpr u64 HostOverlayAddress = ~u64{0} - 1;

/// Starts showing the host screen (the next published frame).
void Show();
/// Stops showing it: the console's picture is presented again at the next composition.
void Hide();
/// Whether the host screen replaces the console's picture now.
bool Active();

/// The compositor composed a picture of the console's own: from now on the host screen is seen
/// when shown (before it, nothing presents it). Once a frame; cheap.
void NoteGuestFrame();
/// Whether the console has put a picture on the screen since the process started.
bool GuestFrameShown();
/// How many pictures of its own the console has put on the screen since the process started (a
/// notice waits for new ones before its time starts running).
u64 GuestFrames();

/// Whether any host layer (screen, overlay, sleep or dim shade) is in use: while not, presenting
/// does nothing of Harbor's (one atomic read per frame).
bool AnyLayer();

/// A new picture from the frontend (any thread).
void Publish(std::shared_ptr<const Frame> frame);
/// The picture to present, or null while there is none.
std::shared_ptr<const Frame> Current();

/// For the compositor: whether something changed since it last composed (a new host frame, or the
/// host screen was shown or hidden), so it presents even when the guest queued nothing new.
bool TakeChanged();

/// The single layer that stands for the host screen, sized to `frame`.
Tegra::FramebufferConfig MakeLayer(const Frame& frame);

/// The host overlay: premultiplied RGBA over the console's picture, shown while the host screen
/// is not. Null clears it.
void PublishOverlay(std::shared_ptr<const Frame> frame);
std::shared_ptr<const Frame> CurrentOverlay();
Tegra::FramebufferConfig MakeOverlayLayer(const Frame& frame);

/// The picture behind a host address (the screen, the overlay or the shade), or null.
std::shared_ptr<const Frame> FrameAt(u64 address);

// Harbor's sleep and dimming, done here so every host and a hidden window show them.

/// ... and for the shade: one black pixel the presenter draws for sleep and dimming.
constexpr u64 ShadeAddress = ~u64{0} - 2;

/// Whether `address` is one of the host's own layers (the screen, the overlay or the shade).
bool IsHostAddress(u64 address);

/// The console sleeps: the whole surface is black (letterbox too), the console's picture is not
/// shown. Wins over the host screen, the overlay and the dimming.
void SetBlackout(bool blackout);
bool Blackout();
/// idle's dimming: a black layer at `level` (0 none .. 1 black) over the console's picture.
void SetDim(float level);
float DimLevel();
/// The shade layer: opaque black for the blackout, premultiplied black at the dim level otherwise.
Tegra::FramebufferConfig MakeShadeLayer();

} // namespace VideoCore::HostScreen

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

/// Starts showing the host screen (the next published frame).
void Show();
/// Stops showing it: the console's picture is presented again at the next composition.
void Hide();
/// Whether the host screen replaces the console's picture now.
bool Active();

/// A new picture from the frontend (any thread).
void Publish(std::shared_ptr<const Frame> frame);
/// The picture to present, or null while there is none.
std::shared_ptr<const Frame> Current();

/// For the compositor: whether something changed since it last composed (a new host frame, or the
/// host screen was shown or hidden), so it presents even when the guest queued nothing new.
bool TakeChanged();

/// The single layer that stands for the host screen, sized to `frame`.
Tegra::FramebufferConfig MakeLayer(const Frame& frame);

} // namespace VideoCore::HostScreen

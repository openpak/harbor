// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <atomic>
#include <mutex>

#include "video_core/framebuffer_config.h"
#include "video_core/host_screen.h"

namespace VideoCore::HostScreen {

namespace {
std::mutex mutex;
std::shared_ptr<const Frame> current;
std::atomic_bool active{false};
std::atomic_bool changed{false};
std::atomic_bool blackout{false};
std::atomic<float> dim_level{0.0f};
} // namespace

void Show() {
    active = true;
    changed = true;
}

void Hide() {
    active = false;
    changed = true;
    std::scoped_lock lk{mutex};
    current.reset();
}

bool Active() {
    return active.load();
}

void Publish(std::shared_ptr<const Frame> frame) {
    {
        std::scoped_lock lk{mutex};
        current = std::move(frame);
    }
    changed = true;
}

std::shared_ptr<const Frame> Current() {
    if (!active) {
        return nullptr;
    }
    std::scoped_lock lk{mutex};
    return current;
}

bool TakeChanged() {
    return changed.exchange(false);
}

Tegra::FramebufferConfig MakeLayer(const Frame& frame) {
    Tegra::FramebufferConfig layer{};
    layer.address = HostAddress;
    layer.offset = 0;
    layer.width = frame.width;
    layer.height = frame.height;
    layer.stride = frame.width;
    layer.pixel_format = Service::android::PixelFormat::Rgba8888;
    layer.transform_flags = Service::android::BufferTransformFlags::Unset;
    layer.crop_rect = {0, 0, static_cast<int>(frame.width), static_cast<int>(frame.height)};
    layer.blending = Tegra::BlendMode::Opaque;
    return layer;
}

void SetBlackout(bool value) {
    if (blackout.exchange(value) != value) {
        changed = true;
    }
}

bool Blackout() {
    return blackout.load();
}

void SetDim(float level) {
    if (dim_level.exchange(level) != level) {
        changed = true;
    }
}

float DimLevel() {
    return dim_level.load();
}

bool IsHostAddress(u64 address) {
    return address == HostAddress || address == ShadeAddress;
}

std::shared_ptr<const Frame> FrameFor(u64 address) {
    if (address != ShadeAddress) {
        return Current();
    }
    auto frame = std::make_shared<Frame>();
    frame->width = 1;
    frame->height = 1;
    // Premultiplied black: only the alpha says how much of the picture is covered.
    const float level = blackout ? 1.0f : dim_level.load();
    frame->rgba = {0, 0, 0, static_cast<u8>(level * 255.0f + 0.5f)};
    return frame;
}

Tegra::FramebufferConfig MakeShadeLayer() {
    Frame pixel{.width = 1, .height = 1, .rgba = {}};
    Tegra::FramebufferConfig layer = MakeLayer(pixel);
    layer.address = ShadeAddress;
    layer.blending = blackout ? Tegra::BlendMode::Opaque : Tegra::BlendMode::Premultiplied;
    return layer;
}

} // namespace VideoCore::HostScreen

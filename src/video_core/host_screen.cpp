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

} // namespace VideoCore::HostScreen

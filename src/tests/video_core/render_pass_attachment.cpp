// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <limits>
#include <catch2/catch_test_macros.hpp>
#include "video_core/renderer_vulkan/vk_render_pass_cache.h"

TEST_CASE("Sparse render targets do not alias the first attachment", "[video_core]") {
    using VideoCore::Surface::PixelFormat;
    Vulkan::RenderPassKey key{};
    key.color_formats.fill(PixelFormat::Invalid);
    key.color_formats[2] = PixelFormat::A8B8G8R8_UNORM;
    key.color_formats[5] = PixelFormat::R32_UINT;
    key.depth_format = PixelFormat::D32_FLOAT;
    for (size_t slot = 0; slot < key.color_formats.size(); ++slot) {
        REQUIRE(key.HasColorAttachment(slot) == (slot == 2 || slot == 5));
    }
    REQUIRE_FALSE(key.HasColorAttachment(8));
    REQUIRE_FALSE(key.HasColorAttachment(std::numeric_limits<size_t>::max()));
    key.color_formats.fill(PixelFormat::Invalid);
    REQUIRE_FALSE(key.HasColorAttachment(0)); // depth-only framebuffer
    key.color_formats.fill(PixelFormat::A8B8G8R8_UNORM);
    REQUIRE(key.HasColorAttachment(0));
    REQUIRE(key.HasColorAttachment(7));
}

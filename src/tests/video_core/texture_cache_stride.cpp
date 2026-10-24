// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <cstdio>
#include <span>
#include <vector>

#include "video_core/texture_cache/image_base.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/util.h"
#include "video_core/textures/decoders.h"

namespace {
using Tegra::Engines::Maxwell3D;
using VideoCommon::ImageBase;
using VideoCommon::ImageInfo;

constexpr GPUVAddr GpuAddress = 0x10000000;
constexpr VAddr CpuAddress = 0x20000000;
constexpr u32 Width = 128;
constexpr u32 Height = 128;
constexpr u32 LayerBytes = Width * Height * 4;
int failures = 0;

bool Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
    return condition;
}

void CheckMerge(ImageInfo target) {
    Check(target.resources.layers == 1 && target.layer_stride == 0,
          "single-layer target starts with an omitted array pitch");
    ImageInfo array = target;
    array.resources.layers = 2;
    array.layer_stride = LayerBytes;
    array.maybe_unaligned_layer_stride = LayerBytes;
    const ImageBase overlap{array, GpuAddress, CpuAddress};
    const auto result =
        VideoCommon::ResolveOverlap(target, GpuAddress, CpuAddress, overlap, true, false, true);
    if (!Check(result.has_value(), "target and array resolve to the same image")) {
        return;
    }
    target.MergeResources(result->resources, overlap.info);
    if (!Check(target.layer_stride == LayerBytes, "merged array retains its source stride")) {
        return;
    }
    Check(VideoCommon::CalculateGuestSizeInBytes(target) == 2 * LayerBytes,
          "merged source buffer covers both layers");
    const ImageBase merged{target, GpuAddress, CpuAddress};
    const auto second_layer = merged.TryFindBase(GpuAddress + LayerBytes);
    Check(second_layer && second_layer->layer == 1 && second_layer->level == 0,
          "second layer remains addressable after the merge");

    // Read back distinct layer contents using the same decoder as the crashing upload.
    std::vector<u8> linear(2 * LayerBytes);
    std::vector<u8> tiled(VideoCommon::CalculateGuestSizeInBytes(target));
    std::vector<u8> decoded(linear.size());
    for (size_t i = 0; i < linear.size(); ++i) {
        linear[i] = static_cast<u8>((i * 7 + (i / LayerBytes) * 31) & 0xff);
    }
    for (size_t layer = 0; layer < 2; ++layer) {
        const auto offset = layer * LayerBytes;
        Tegra::Texture::SwizzleTexture(std::span{tiled}.subspan(offset, LayerBytes),
                                       std::span<const u8>{linear}.subspan(offset, LayerBytes), 4,
                                       Width, Height, 1, 0, 0);
        Tegra::Texture::UnswizzleTexture(std::span{decoded}.subspan(offset, LayerBytes),
                                         std::span<const u8>{tiled}.subspan(offset, LayerBytes), 4,
                                         Width, Height, 1, 0, 0);
    }
    Check(decoded == linear, "both merged layers decode without losing their contents");
}
} // namespace

int main() {
    Maxwell3D::Regs::RenderTargetConfig color{};
    color.width = Width;
    color.height = Height;
    color.format = Tegra::RenderTargetFormat::A8B8G8R8_UNORM;
    color.depth.Assign(1);
    const auto samples = Tegra::Texture::MsaaMode::Msaa1x1;
    CheckMerge(ImageInfo{color, samples});

    Maxwell3D::Regs::Zeta depth{};
    depth.format = Tegra::DepthFormat::Z32_FLOAT;
    Maxwell3D::Regs::ZetaSize depth_size{};
    depth_size.width = Width;
    depth_size.height = Height;
    depth_size.depth.Assign(1);
    CheckMerge(ImageInfo{depth, depth_size, samples});

    // An explicit guest pitch must not be replaced by another image's inferred pitch.
    color.array_pitch = (LayerBytes + 4096) / 4;
    ImageInfo explicit_pitch{color, samples};
    ImageInfo other{color, samples};
    other.layer_stride = LayerBytes;
    other.maybe_unaligned_layer_stride = LayerBytes;
    explicit_pitch.MergeResources({1, 2}, other);
    Check(explicit_pitch.layer_stride == LayerBytes + 4096 &&
              explicit_pitch.maybe_unaligned_layer_stride == LayerBytes + 4096,
          "explicit render-target pitch is preserved");

    if (failures == 0) {
        std::puts("texture_cache_stride_selftest: all checks passed");
    }
    return failures == 0 ? 0 : 1;
}

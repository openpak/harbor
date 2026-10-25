// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <algorithm>
#include <cstdint>

namespace VideoCommon {

// Unified memory is shared with the OS. A heap's size is not a minimum amount
// available to the renderer, and must never override a smaller driver budget.
constexpr std::uint64_t IntegratedMemoryBudget(std::uint64_t budget,
                                             std::uint64_t usage,
                                             std::uint64_t limit) {
    constexpr std::uint64_t gib = 1ULL << 30;
    const auto available = budget > usage ? budget - usage : 0;
    return std::min(limit, available - std::min(available / 8, gib));
}

// Android GPUs share physical RAM with guest CPU memory and the OS. A large
// Vulkan heap budget is not permission to consume the remaining system memory.
constexpr std::uint64_t SystemAwareMemoryBudget(std::uint64_t driver_budget,
                                               std::uint64_t gpu_usage,
                                               std::uint64_t available) {
    constexpr std::uint64_t reserve = 512ULL << 20;
    const auto growth = available > reserve ? available - reserve : 0;
    // Saturate without overflowing when a driver reports a very large usage.
    return gpu_usage >= driver_budget ? driver_budget
        : gpu_usage + std::min(growth, driver_budget - gpu_usage);
}

struct TextureMemoryLimits {
    std::uint64_t expected;
    std::uint64_t critical;
    std::uint64_t minimum;
};

constexpr TextureMemoryLimits CalculateTextureMemoryLimits(std::uint64_t memory,
                                                          std::uint64_t target) {
    constexpr std::uint64_t mib = 1ULL << 20;
    constexpr std::uint64_t gib = 1ULL << 30;
    const auto threshold = std::min(memory, target);
    const auto expected_ceiling = memory - std::min(memory / 4, gib);
    const auto critical_ceiling = memory - std::min(memory / 8, 512 * mib);
    return {
        std::min(expected_ceiling, std::max(memory - threshold * 6 / 10, gib + 125 * mib)),
        std::min(critical_ceiling, std::max(memory - threshold * 2 / 10, gib + 625 * mib)),
        (memory - threshold) / 2,
    };
}

} // namespace VideoCommon

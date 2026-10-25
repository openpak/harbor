// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <array>
#include <catch2/catch_test_macros.hpp>
#include "video_core/memory_budget.h"

TEST_CASE("Integrated GPU limits respect the available budget", "[video_core]") {
    constexpr std::uint64_t gib = 1ULL << 30;
    REQUIRE(VideoCommon::IntegratedMemoryBudget(2 * gib, gib, 4 * gib) < gib);
    REQUIRE(VideoCommon::IntegratedMemoryBudget(gib, 2 * gib, 4 * gib) == 0);
    REQUIRE(VideoCommon::IntegratedMemoryBudget(16 * gib, 0, 4 * gib) == 4 * gib);
}

TEST_CASE("Texture cleanup precedes exhaustion on small heaps", "[video_core]") {
    constexpr std::uint64_t gib = 1ULL << 30;
    for (std::uint64_t memory : std::array<std::uint64_t, 6>{0, gib / 2, gib, 2 * gib, 4 * gib, 8 * gib}) {
        const auto limits = VideoCommon::CalculateTextureMemoryLimits(memory, 3 * gib);
        REQUIRE(limits.expected <= limits.critical);
        REQUIRE(limits.critical <= memory);
        REQUIRE(limits.minimum <= limits.expected);
        if (memory != 0) {
            REQUIRE(limits.critical < memory);
        }
    }
    const auto old_limits = VideoCommon::CalculateTextureMemoryLimits(4 * gib, 3 * gib);
    const auto new_limits = VideoCommon::CalculateTextureMemoryLimits(gib, 3 * gib);
    REQUIRE(new_limits.critical < old_limits.expected);
}

TEST_CASE("Android GPU budget leaves system headroom", "[video_core]") {
    constexpr std::uint64_t mib = 1ULL << 20;
    REQUIRE(VideoCommon::SystemAwareMemoryBudget(5000*mib, 500*mib, 768*mib) == 756*mib);
    REQUIRE(VideoCommon::SystemAwareMemoryBudget(5000*mib, 500*mib, 100*mib) == 500*mib);
    REQUIRE(VideoCommon::SystemAwareMemoryBudget(5000*mib, 500*mib, 0) == 500*mib);
    REQUIRE(VideoCommon::SystemAwareMemoryBudget(2000*mib, 500*mib, 8000*mib) == 2000*mib);
    REQUIRE(VideoCommon::SystemAwareMemoryBudget(400*mib, 500*mib, 0) == 400*mib);
}

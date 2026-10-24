// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <initializer_list>

#include <catch2/catch_test_macros.hpp>

#include "core/arm/nce/patcher_reach.h"

using namespace Core::NCE;

constexpr size_t MiB = size_t{1} << 20;

TEST_CASE("NCE split patch sites: images up to 256 MiB are fully reachable", "[core][nce]") {
    const size_t image = 208 * MiB;
    for (size_t off = 0; off < image; off += 4 * 1024) {
        const auto site = ClassifySplitSite(off, image);
        REQUIRE(site != SplitSite::Unreachable);
        // The section it chose is really within a B's reach.
        if (site == SplitSite::Pre) {
            REQUIRE(off < BranchReach);
        } else {
            REQUIRE(image - off < BranchReach);
        }
    }
}

TEST_CASE("NCE split patch sites: a 346 MiB module has an unreachable middle", "[core][nce]") {
    // Spelunky 2 main: text 343 MiB, image about 359 MiB with BSS.
    const size_t image = 359 * MiB;
    REQUIRE(ClassifySplitSite(0x65bc, image) == SplitSite::Pre);
    REQUIRE(ClassifySplitSite(100 * MiB, image) == SplitSite::Pre);
    // 0x8063cfc was the first site that made oaknut throw "out of range".
    REQUIRE(ClassifySplitSite(0x8063cfc, image) == SplitSite::Unreachable);
    REQUIRE(ClassifySplitSite(200 * MiB, image) == SplitSite::Unreachable);
    REQUIRE(ClassifySplitSite(image - 120 * MiB, image) == SplitSite::Post);
    REQUIRE(ClassifySplitSite(image - 4, image) == SplitSite::Post);
}

TEST_CASE("NCE split patch sites: never pick a section out of reach", "[core][nce]") {
    for (size_t image : {129 * MiB, 200 * MiB, 256 * MiB, 257 * MiB, 400 * MiB, 1024 * MiB}) {
        for (size_t off = 0; off < image; off += 256 * 1024) {
            switch (ClassifySplitSite(off, image)) {
            case SplitSite::Pre:
                REQUIRE(off < BranchReach);
                break;
            case SplitSite::Post:
                REQUIRE(image - off < BranchReach);
                break;
            case SplitSite::Unreachable:
                REQUIRE(off >= BranchReach);
                REQUIRE(image - off >= BranchReach);
                break;
            }
        }
    }
}

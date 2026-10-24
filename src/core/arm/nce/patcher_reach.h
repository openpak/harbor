// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>

namespace Core::NCE {

// A64 B/BL reach +-128 MiB.
inline constexpr size_t BranchReach = size_t{128} << 20;
// Room kept for the patch sections themselves, which are not known while sites are collected.
inline constexpr size_t BranchReachSlack = size_t{8} << 20;

enum class SplitSite { Pre, Post, Unreachable };

// In split mode (module image larger than BranchReach) the patch code is in a section before the
// module and one after it. A patch site can only use a section within BranchReach, so a site
// further than that from both (module_offset in roughly [128 MiB, image_size - 128 MiB), which
// exists for images over 256 MiB) cannot be patched at all.
constexpr SplitSite ClassifySplitSite(size_t module_offset, size_t image_size) {
    constexpr size_t strict = BranchReach - BranchReachSlack;
    const size_t to_post = image_size > module_offset ? image_size - module_offset : 0;
    if (module_offset < strict) {
        return SplitSite::Pre;
    }
    if (to_post < strict) {
        return SplitSite::Post;
    }
    if (module_offset < BranchReach) {
        return SplitSite::Pre;
    }
    if (to_post < BranchReach) {
        return SplitSite::Post;
    }
    return SplitSite::Unreachable;
}

} // namespace Core::NCE

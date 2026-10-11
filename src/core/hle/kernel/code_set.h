// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <vector>

#include "core/hle/kernel/k_typed_address.h"

namespace Kernel {

/**
 * Represents executable data that may be loaded into a kernel process.
 *
 * A code set consists of three basic segments:
 *   - A code (AKA text) segment,
 *   - A read-only data segment (rodata)
 *   - A data segment
 *
 * The code segment is the portion of the object file that contains
 * executable instructions.
 *
 * The read-only data segment in the portion of the object file that
 * contains (as one would expect) read-only data, such as fixed constant
 * values and data structures.
 *
 * The data segment is similar to the read-only data segment -- it contains
 * variables and data structures that have predefined values, however,
 * entities within this segment can be modified.
 */
struct CodeSet final {
    /// A single segment within a code set.
    struct Segment final {
        /// The byte offset that this segment is located at.
        std::size_t offset = 0;

        /// The address to map this segment to.
        KProcessAddress addr = 0;

        /// The size of this segment in bytes.
        u32 size = 0;
    };

    explicit CodeSet();
    ~CodeSet();

    CodeSet(const CodeSet&) = delete;
    CodeSet& operator=(const CodeSet&) = delete;

    CodeSet(CodeSet&&) = default;
    CodeSet& operator=(CodeSet&&) = default;

    Segment& CodeSegment() {
        return segments[0];
    }

    const Segment& CodeSegment() const {
        return segments[0];
    }

    Segment& RODataSegment() {
        return segments[1];
    }

    const Segment& RODataSegment() const {
        return segments[1];
    }

    Segment& DataSegment() {
        return segments[2];
    }

    const Segment& DataSegment() const {
        return segments[2];
    }

#ifdef HAS_NCE
    Segment& PatchSegment() {
        return patch_segment;
    }

    const Segment& PatchSegment() const {
        return patch_segment;
    }

    Segment& PostPatchSegment() {
        return post_patch_segment;
    }

    const Segment& PostPatchSegment() const {
        return post_patch_segment;
    }
#endif

    /// The image size, the zero range left out of memory included.
    std::size_t ImageSize() const {
        return memory.size() + zero_size;
    }

    /// The overall data that backs this code set.
    std::vector<u8> memory;

    /// A zero-filled range of the image (a module's BSS) that memory leaves out: memory[0,
    /// zero_offset) loads at offset 0 and memory[zero_offset, end) at zero_offset + zero_size.
    /// The process's code pages are zero when they are allocated, so the range is never copied.
    /// Some titles have a BSS of several GiB (Don't Starve Together: 3.1 GiB), which a host copy
    /// would make resident twice over during the load.
    std::size_t zero_offset = 0;
    std::size_t zero_size = 0;
    /// The segments that comprise this code set.
    std::array<Segment, 3> segments;

#ifdef HAS_NCE
    Segment patch_segment;
    Segment post_patch_segment;
#endif

    /// The entry point address for this code set.
    KProcessAddress entrypoint = 0;
};

} // namespace Kernel

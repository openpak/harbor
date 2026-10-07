// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// The Vulkan memory allocator's implementation, which each Eden executable compiles once itself. Only for
// the setup's own test executables; the emulator binaries have theirs.
#define VMA_IMPLEMENTATION
#include "video_core/vulkan_common/vma.h"

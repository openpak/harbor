// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// The Vulkan memory allocator's implementation, which each executable that links video_core
// compiles once itself (harbor-ksp-key links core, which pulls video_core in).
#define VMA_IMPLEMENTATION
#include "video_core/vulkan_common/vma.h"

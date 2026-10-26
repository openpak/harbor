// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once
#include <optional>
#include "common/common_types.h"
struct HarborTestContext { u64 regs[31]{}; u64 sp{}; u64 pc{}; };
struct fpsimd_context { u128 vregs[32]{}; };
#define mcontext_t HarborTestContext

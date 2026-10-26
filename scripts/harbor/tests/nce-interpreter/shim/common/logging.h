// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once
#include <cstdlib>
#define LOG_CRITICAL(...) ((void)0)
inline void AssertFailSoftImpl() { std::abort(); }
[[noreturn]] inline void AssertFatalImpl() { std::abort(); }

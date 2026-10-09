// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// MSVC only: what Borealis takes from unistd.h (access, chdir).

#pragma once

#include <direct.h>
#include <io.h>

#ifndef F_OK
#define F_OK 0
#endif

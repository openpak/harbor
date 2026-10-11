// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "harbor_setup/host.h"

namespace HarborDesktop {

/// Test hook for the harness: with HARBOR_TEST_DRIVER_STORE=<dir> the desktop Harbor menu gets a
/// graphics driver store in <dir>, as on the owner's ROG Phone 3 (Adreno 650, Android 12, API 31),
/// so the Graphics driver pages and the GitHub catalogue (real HTTPS downloads) can be checked on a
/// PC. The desktop never loads these drivers. Null without the variable.
HarborSetup::Host* TestDriverStore();

} // namespace HarborDesktop

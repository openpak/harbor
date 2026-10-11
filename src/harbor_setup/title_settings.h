// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// The settings a game may have for itself (Harbor Settings › Display & Performance, "This game"):
// kept in Eden's own per-game file (<config>/custom/<TITLE ID>.ini, the switchable settings'
// "use_global" form, which Eden's per-game configuration reads too) and applied as the game starts
// in the HOME session, the console-wide values coming back as it exits. HOME and every game without
// its own value use the console-wide one.

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "common/common_types.h"

namespace Core {
class System;
}

namespace Settings {
class BasicSetting;
}

namespace HarborSetup::TitleSettings {

/// The settings that can differ per game: they take effect while the game runs (asynchronous
/// shaders, frame generation). The resolution is the console's: it is fixed when the console starts.
const std::vector<Settings::BasicSetting*>& PerGame();

/// Once per process: the window system tells this module when applications start and exit.
void Install(Core::System& system);

/// The test harness: as if `title_id` started (or, with nothing, the running one exited).
void SimulateForTest(std::optional<u64> title_id);

/// The application running in the session (its program id), if one is.
std::optional<u64> RunningTitle();
/// Its name from its control data, or its program id.
std::string TitleName(Core::System& system, u64 title_id);

/// Whether `title_id` has its own value for `setting`.
bool HasOverride(u64 title_id, const Settings::BasicSetting& setting);
/// Gives `title_id` its own value (as the setting's string form), saved at once; applied now when
/// that title runs.
void SetOverride(u64 title_id, Settings::BasicSetting& setting, const std::string& value);
/// Back to the console-wide value for `title_id`.
void ClearOverride(u64 title_id, Settings::BasicSetting& setting);

/// Changes the console-wide value, also while a game's own value is in effect.
void SetGlobalValue(Settings::BasicSetting& setting, const std::string& value);
/// The console-wide value's string form.
std::string GlobalValue(const Settings::BasicSetting& setting);

} // namespace HarborSetup::TitleSettings

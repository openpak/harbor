// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <string>

/// The test harness's physical-pad stand-in: an SDL virtual gamepad that Eden's SDL driver finds as
/// it finds a real one (engine "sdl", its own GUID), so press-to-assign, per-controller mappings and
/// reconnects are tested on PC. Buttons and axes use SDL's gamepad numbering.
namespace HarborTestPad {

/// Plugs the pad in (once). Whether it is there.
bool Attach();
/// Unplugs it.
void Detach();
bool SetButton(int button, bool down);
/// `value` from -1 to 1 (triggers 0 to 1).
bool SetAxis(int axis, float value);
/// SDL's gamepad button by name (a, b, x, y, back, guide, start, leftstick, rightstick,
/// leftshoulder, rightshoulder, dpup, dpdown, dpleft, dpright, misc1) or number; -1 if unknown.
int ButtonIndex(const std::string& name);

} // namespace HarborTestPad

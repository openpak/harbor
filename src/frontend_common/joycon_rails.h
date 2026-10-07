// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// The Joy-Cons on and off the console's rails, a host action as on the Ryujinx Harbor (F7 on
// desktop, a Harbor menu item on Android, `detach`/`attach` in the test harness). The firmware's
// first-time setup waits on "Detaching Joy-Con Controllers" until they leave the rails.

#pragma once

namespace Core::HID {
class HIDCore;
}

namespace FrontendCommon {

/// Detached: the Handheld controller disconnects and Player 1 connects as a pair of Joy-Cons.
/// Attached: Player 1 goes back to its configured style (or stays disconnected when it is not
/// configured as connected) and the Handheld controller connects. Call on the thread that owns
/// input changes (the Qt GUI thread; on Android the UI thread).
void SetJoyconsDetached(Core::HID::HIDCore& hid, bool detached);

/// Whether the Joy-Cons are off the rails now (the Handheld controller is disconnected).
bool JoyconsDetached(const Core::HID::HIDCore& hid);

} // namespace FrontendCommon

// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

namespace Core::HID {
class HIDCore;
}

namespace HarborSetup {
class Host;
struct DeviceInfo;
} // namespace HarborSetup

namespace Harbor {

/// Whether this process boots the console (HOME menu) rather than a game picked from Eden's list.
/// Set by the Harbor launcher through HarborNative.setConsoleMode.
bool ConsoleMode();

/// A console starts with its Joy-Cons on the rails: the phone and whatever controller is attached
/// to it are the handheld console. Sets the input settings so that the Handheld controller is
/// connected and Player 1 is not (Player 1's style Handheld, which also sends the touch overlay to
/// the Handheld controller), both with Player 1's mappings. Call before HIDCore reloads them.
void StartWithJoyconsAttached();

/// Joy-Cons off the rails: Handheld disconnects and Player 1 connects as a pair of Joy-Cons; on the
/// rails again: the reverse. Player 1 and Handheld carry the same mappings, so every physical pad
/// and the touch overlay follow. Not saved: the next console start is attached again.
void SetJoyconsDetached(Core::HID::HIDCore& hid, bool detached);

/// The setup's driver store and network (the wizard's host, HarborSetupBridge), for the Harbor
/// menu's and the stopped screen's driver downloads. Needs HarborNative.initialize in this process.
HarborSetup::Host& DriverHost();

/// This phone, probed once: what the driver catalogue is filtered for.
const HarborSetup::DeviceInfo& Device();

} // namespace Harbor

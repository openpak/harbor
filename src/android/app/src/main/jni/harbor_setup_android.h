// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

namespace Harbor {

/// Whether this process boots the console (HOME menu) rather than a game picked from Eden's list.
/// Set by the Harbor launcher through HarborNative.setConsoleMode.
bool ConsoleMode();

} // namespace Harbor

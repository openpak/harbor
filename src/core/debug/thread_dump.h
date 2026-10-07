// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

namespace Core {

class System;

namespace Debug {

/// Logs every guest process and each of its threads (state, wait reason, core, registers and a
/// short backtrace) at Info level. Every line starts with "thread dump". Safe to call from any
/// host thread while the guest is running.
void LogGuestThreads(System& system);

} // namespace Debug
} // namespace Core

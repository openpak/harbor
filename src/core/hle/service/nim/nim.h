// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Core {
class System;
}

namespace Service::NIM {

void LoopProcess(Core::System& system);

/// [Harbor] What ntc does with the server time: sets the standard network clock to it. Harbor's
/// server time is the host's UTC. `why` is logged.
void CorrectNetworkClock(Core::System& system, const char* why);

} // namespace Service::NIM

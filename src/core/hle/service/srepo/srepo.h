// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Core {
class System;
}

namespace Service::SREPO {

/// srepo:a and srepo:u, the play-report service. The firmware's own programs (friends, npns) wait
/// for it at start-up; every report they save is dropped, as Harbor has nowhere to send it.
void LoopProcess(Core::System& system);

} // namespace Service::SREPO

// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Harbor's first-run setup: keys, firmware, game/DLC/update folders and (Android) the graphics driver,
// then the console boots HOME and the firmware's own first-time setup takes over. One Borealis
// implementation for every frontend; see docs/harbor/eden-port/host.md section 3.3 for the contract.

#pragma once

#include <string>

#include "harbor_setup/record.h"

namespace HarborSetup {

class Host;

/// Whether this data directory can boot the console without the wizard, judged from the files.
struct Readiness {
    bool keys_usable = false;
    bool firmware_present = false;
    bool qlaunch = false;
    std::string firmware_version; ///< empty when no readable system version is installed
    std::string firmware_problem; ///< why installed firmware cannot boot HOME, for the firmware page
    bool record_completed = false;

    bool FirmwareReady() const {
        return qlaunch && !firmware_version.empty();
    }
    bool Ready() const {
        return keys_usable && FirmwareReady() && record_completed;
    }
};

/// Reads the keys, the installed firmware and harbor-setup.json. Needs no Core::System.
Readiness CheckReadiness();

/// Runs the wizard in its own window until "Start Harbor" (returns 0) or the window closes (returns 1).
/// Call once per process: Borealis keeps static state. The SDL video subsystem is shut down on return.
int RunWizard(Host& host);

/// Android back: Back on the wizard's current page. Safe from any thread.
void RequestBack();

/// Desktop: before Qt starts. Runs the wizard when the data directory is not ready or `--harbor-setup`
/// is given (the flag is removed from argv).
/// Returns -1 when no setup was needed, 0 when setup completed, 1 when the window was closed: quit.
int RunDesktopIfNeeded(int& argc, char** argv);

} // namespace HarborSetup

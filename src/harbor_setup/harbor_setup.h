// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Harbor's first-run setup: keys, firmware, game/DLC/update folders and (Android) the graphics driver,
// then the console boots HOME and the firmware's own first-time setup takes over. One Borealis
// implementation for every frontend; see docs/harbor/eden-port/host.md section 3.3 for the contract.

#pragma once

#include <string>

#include "core/openpak/boot_connection.h"
#include "harbor_setup/record.h"

namespace Core {
class System;
}

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

/// What a console session needs before every boot: the firmware's own library applets (no host
/// stand-ins) and handheld mode. Changes Settings::values; the caller saves them if its config reloads.
void ApplyConsolePreset();

/// The setup's DLC and update folders as Eden's external content folders, and the add-ons beside
/// the games too. Set at every boot, before the content providers are built and the library is
/// scanned, so HOME and every title see the latest update and DLC (a new one the next boot).
void ApplyRecordFolders(const Record& record);

/// Before every console boot, off the UI thread: the platform's password store for OpenPak's
/// credentials (frontend_common/secret_store.h), then OpenPak's boot connection. Blocking: under
/// a second online, ~2 s with the site down, up to ~10 s on a first connect; later calls return the
/// first result at once. Android: call from a JNI method on a Java thread.
OpenPak::BootResult ConnectOpenPak();

/// What the HOME menu asked for when it stopped the console (its power menu).
enum class PowerRequest { None, Shutdown, Reboot };
PowerRequest ConsolePowerRequest(Core::System& system);

/// Android back: Back on the wizard's current page. Safe from any thread.
void RequestBack();

/// Desktop: before Qt starts. Runs the wizard when the data directory is not ready or `--harbor-setup`
/// is given (the flag is removed from argv). Under the test harness (HARBOR_CONTROL) or with an
/// explicit boot target (--qlaunch, a game path) it does nothing, unless `--harbor-setup` or
/// `--harbor-console` is given.
/// Returns -1 when no setup was needed, 0 when setup completed, 1 when the window was closed: quit.
int RunDesktopIfNeeded(int& argc, char** argv);

/// Desktop: whether this process may run in console mode (set by RunDesktopIfNeeded; false under the
/// test harness or with an explicit boot target unless --harbor-setup / --harbor-console was given).
bool DesktopConsoleAllowed();

} // namespace HarborSetup

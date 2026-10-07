// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// The desktop wizard on its own, exactly as the harbor binary runs it before Qt:
//   harbor_setup_run [--harbor-setup]
// Data goes to Eden's user directory: ./user when it exists (portable), else the XDG one.

#include <cstdio>

#include "harbor_setup/harbor_setup.h"

int main(int argc, char* argv[]) {
    const int result = HarborSetup::RunDesktopIfNeeded(argc, argv);
    std::printf("harbor_setup_run: %s\n", result < 0    ? "ready, no setup needed"
                                          : result == 0 ? "setup completed"
                                                        : "setup closed before it finished");
    return result < 0 ? 0 : result;
}

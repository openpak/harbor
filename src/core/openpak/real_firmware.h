// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include "common/common_types.h"

namespace Core {
class System;
}

// [OpenPak] The firmware's own system programs that Harbor runs as guest processes instead of
// re-implementing them. A program on the list is started with every session (HOME, the starter,
// a directly booted game), and every service its NPDM lets it host is left to it: Harbor's
// module of the same name registers nothing of those. Only pure-logic modules belong here (the
// ones that talk to servers and keep state); anything that drives hardware stays emulated.
//
// OPENPAK_REAL_PROGRAMS overrides the default list: "none", or a comma-separated list of program
// ids (hex) or NPDM names ("friends,npns,bcat"); "default" stands for the built-in list.
namespace OpenPak::RealFirmware {

/// The programs to run, resolved once.
const std::vector<u64>& Programs(Core::System& system);

/// Whether a program on the list hosts this service, so the HLE module must not register it.
bool Hosts(Core::System& system, std::string_view service_name);

/// The program on the list that hosts this service, if any.
std::optional<u64> ProgramHosting(Core::System& system, std::string_view service_name);

} // namespace OpenPak::RealFirmware

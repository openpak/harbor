// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <filesystem>

// [OpenPak] The public-key patch the firmware's bcat module needs to accept OpenPak's news
// articles. It patches a firmware module (program 010000000000000C, as the console does), never
// game code. The bytes are embedded; the IPS holds a public key only.
namespace OpenPak::BcatKeyPatch {

/// Writes <load_dir>/010000000000000C/openpak_bcat_key/exefs/<build id>.ips when missing or
/// different. Touches nothing else under load_dir. Returns true when the file is in place.
bool Install(const std::filesystem::path& load_dir);

} // namespace OpenPak::BcatKeyPatch

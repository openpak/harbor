// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <string>

namespace HarborSetup {

struct KeysResult {
    bool ok = false;
    std::string message; ///< what to show on the page; never contains key material
};

/// Why `text` is not a usable keys file, or an empty string. A prod.keys needs a 64-digit header_key;
/// every line must be "name = hex". Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).
std::string KeysFileProblem(const std::string& text, bool title_keys);

/// Installs the picked prod.keys (a path, or a content URI on Android). The file is copied into the
/// setup's import folder and checked there first, so a bad file never replaces installed keys. On
/// desktop a title.keys beside it is taken too.
KeysResult ImportProdKeys(const std::string& location);

/// Installs a picked title.keys the same way.
KeysResult ImportTitleKeys(const std::string& location);

/// Whether the installed keys can read the console's content (a header_key and the derived keys).
bool KeysUsable();

} // namespace HarborSetup

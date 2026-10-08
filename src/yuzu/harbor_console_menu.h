// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// The Harbor menu's console entries: back up the console (the NAND with its device key sealed by
// a passphrase), restore one, and make this a new console.

#pragma once

#include <functional>

class QWidget;

namespace HarborConsoleMenu {

/// Runs `change` with the emulated console stopped, then starts it again (the caller's shutdown
/// and restart).
using WhileStopped = std::function<void(std::function<void()> change)>;

void BackUp(QWidget* parent, const WhileStopped& while_stopped);
void Restore(QWidget* parent, const WhileStopped& while_stopped);
void MakeNew(QWidget* parent, const WhileStopped& while_stopped);

} // namespace HarborConsoleMenu

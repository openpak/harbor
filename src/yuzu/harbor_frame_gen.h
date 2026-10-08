// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <functional>

class QWidget;

/// Frame generation on desktop (Lossless Scaling's shaders from the user's own Lossless.dll, read as
/// data and never loaded). Its settings live in the Harbor menu's Display & Performance page.
namespace HarborFrameGen {

/// Whether the GPU Harbor renders with has what the frame generation shaders need.
bool GpuSupported();

/// Asks for Lossless.dll, checks it, and installs it with the console stopped: `change_dll` runs the
/// change with the console stopped and boots it again (the renderer reads the DLL when it starts).
void ChooseDll(QWidget* parent, std::function<void(std::function<void()>)> change_dll);

/// Removes the installed Lossless.dll (the console restarts) and turns frame generation off.
void RemoveDll(std::function<void(std::function<void()>)> change_dll, std::function<void()> save);

} // namespace HarborFrameGen

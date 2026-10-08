// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <vector>

#include "common/common_types.h"
#include "core/hle/service/erpt/report_builder.h"
#include "core/hle/service/erpt/report_store.h"

namespace Core {
class System;
}

namespace Service::ERPT {

void LoopProcess(Core::System& system);

/// The ApplicationInfo category for a program, as am adds it to the reports it creates: the id as
/// "%016x", the applet id and, for a title, its name and version.
Context MakeApplicationInfo(Core::System& system, u64 program_id, s32 applet_id);

/// A report created by an HLE module the way its firmware module asks erpt for one (am's
/// ReportVisibleError, a fatal error). error_code is "2xxx-yyyy"; extra_error_info holds more
/// ErrorInfo fields; additional holds other categories (ApplicationInfo...). Visible reports show
/// in Settings > Support > Error History. Returns whether the report was stored.
bool CreateHostReport(Core::System& system, u32 report_type, const std::string& error_code,
                      const Context* extra_error_info, const std::vector<Context>& additional);

} // namespace Service::ERPT

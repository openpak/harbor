// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/hle/result.h"

namespace Service::AM {

constexpr Result ResultNoDataInChannel{ErrorModule::AM, 2};
constexpr Result ResultNoMessages{ErrorModule::AM, 3};
constexpr Result ResultLibraryAppletTerminated{ErrorModule::AM, 22};
constexpr Result ResultAppletLaunchFailed{ErrorModule::AM, 35};
constexpr Result ResultApplicationRecordNotFound{ErrorModule::AM, 37};
constexpr Result ResultUnknown75Unavailable{ErrorModule::AM, 6};
constexpr Result ResultNoApplicationLogo{ErrorModule::AM, 52};
constexpr Result ResultInvalidOffset{ErrorModule::AM, 503};
constexpr Result ResultInvalidParameters{ErrorModule::AM, 506};
constexpr Result ResultInvalidStorageType{ErrorModule::AM, 511};
constexpr Result ResultFatalSectionCountImbalance{ErrorModule::AM, 512};
constexpr Result ResultNotImplemented{ErrorModule::AM, 998};

} // namespace Service::AM

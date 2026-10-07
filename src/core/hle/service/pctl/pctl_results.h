// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/hle/result.h"

namespace Service::PCTL {

constexpr Result ResultNoFreeCommunication{ErrorModule::PCTL, 101};
constexpr Result ResultStereoVisionRestricted{ErrorModule::PCTL, 104};
constexpr Result ResultNoCapability{ErrorModule::PCTL, 131};
constexpr Result ResultNoRestrictionEnabled{ErrorModule::PCTL, 181};

// [Harbor] What pctl 22.5.0/23.0.1 answers (the names are ours, the values its impls'). Behaviour
// follows the Ryujinx Harbor (MIT-licensed Ryujinx base).
constexpr Result ResultRestrictedByRating{ErrorModule::PCTL, 103};  // the launch gate (.text 0x40d28)
constexpr Result ResultWrongPin{ErrorModule::PCTL, 120};            // .text 0x4157c
constexpr Result ResultInvalidArgument{ErrorModule::PCTL, 131};     // a bad PIN, a level past 4
constexpr Result ResultPinNotSet{ErrorModule::PCTL, 132};           // .text 0x41740
constexpr Result ResultPermissionDenied{ErrorModule::PCTL, 133};
constexpr Result ResultPairingUnavailable{ErrorModule::PCTL, 135};  // PairingInfoBase state 0
constexpr Result ResultSettingsNeverSynchronized{ErrorModule::PCTL, 140};
constexpr Result ResultInvalidPairingInfo{ErrorModule::PCTL, 200};  // not the record a code found
constexpr Result ResultPairingUnlinked{ErrorModule::PCTL, 220};     // no owner left
constexpr Result ResultInvalidAsyncData{ErrorModule::PCTL, 221};    // a Finish naming no live slot
constexpr Result ResultPairingAccountMismatch{ErrorModule::PCTL, 222};
constexpr Result ResultServerUnreachable{ErrorModule::PCTL, 2003};

/// A moon result description (2142-xxxx; 0 success) as a Result.
constexpr Result PctlResult(int description) {
    return description == 0 ? ResultSuccess
                            : Result{ErrorModule::PCTL, static_cast<u32>(description)};
}

} // namespace Service::PCTL

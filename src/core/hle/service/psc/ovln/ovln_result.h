// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "core/hle/result.h"
#include "core/hle/service/psc/ovln/ovln_agent.h"

namespace Service::PSC {

// The results psc's ovln server returns (module 25). Names are ours.
constexpr Result ResultOvlnQueueFullRejected{ErrorModule::OVLN, 2};      // 0x419
constexpr Result ResultOvlnInvalidArgument{ErrorModule::OVLN, 3};        // 0x619
constexpr Result ResultOvlnQueueFull{ErrorModule::OVLN, 10};             // 0x1419
constexpr Result ResultOvlnSourceAlreadyAttached{ErrorModule::OVLN, 11}; // 0x1619
constexpr Result ResultOvlnNoMessage{ErrorModule::OVLN, 12};             // 0x1819

constexpr Result ToResult(OvlnStatus status) {
    switch (status) {
    case OvlnStatus::Success:
        return ResultSuccess;
    case OvlnStatus::QueueFullRejected:
        return ResultOvlnQueueFullRejected;
    case OvlnStatus::InvalidArgument:
        return ResultOvlnInvalidArgument;
    case OvlnStatus::QueueFull:
        return ResultOvlnQueueFull;
    case OvlnStatus::SourceAlreadyAttached:
        return ResultOvlnSourceAlreadyAttached;
    case OvlnStatus::NoMessage:
        return ResultOvlnNoMessage;
    }
    return ResultUnknown;
}

static_assert(ResultOvlnNoMessage.raw == 0x1819);

} // namespace Service::PSC

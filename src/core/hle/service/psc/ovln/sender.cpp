// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).

#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/psc/ovln/ovln_result.h"
#include "core/hle/service/psc/ovln/sender.h"

namespace Service::PSC {

ISender::ISender(Core::System& system_, std::shared_ptr<OvlnAgent::SenderState> state_)
    : ServiceFramework{system_, "ISender"}, state{std::move(state_)} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, D<&ISender::Send>, "Send"},
        {1, D<&ISender::GetUnreceivedMessageCount>, "GetUnreceivedMessageCount"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

ISender::~ISender() {
    GetOvlnAgent().CloseSender(*state);
}

Result ISender::Send(u64 option, const OverlayNotification& notification) {
    // Only the tag and the size are logged, never the message body.
    const auto status =
        GetOvlnAgent().Send(*state, option, notification, system.CoreTiming().GetClockTicks());
    LOG_DEBUG(Service_PSC, "send tag={} size={:#x} option={:#x}", GetNotificationTag(notification),
              GetNotificationPayloadSize(notification), option);
    R_RETURN(ToResult(status));
}

Result ISender::GetUnreceivedMessageCount(Out<u32> out_count) {
    *out_count = GetOvlnAgent().GetUnreceivedCount(*state);
    R_SUCCEED();
}

} // namespace Service::PSC

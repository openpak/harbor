// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).

#include "core/hle/kernel/k_event.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/psc/ovln/ovln_result.h"
#include "core/hle/service/psc/ovln/receiver.h"

namespace Service::PSC {

IReceiver::IReceiver(Core::System& system_)
    : ServiceFramework{system_, "IReceiver"}, service_context{system_, "IReceiver"} {
    // clang-format off
        static const FunctionInfo functions[] = {
            {0, D<&IReceiver::AddSource>, "AddSource"},
            {1, D<&IReceiver::RemoveSource>, "RemoveSource"},
            {2, D<&IReceiver::GetReceiveEventHandle>, "GetReceiveEventHandle"},
            {3, D<&IReceiver::Receive>, "Receive"},
            {4, D<&IReceiver::ReceiveWithTick>, "ReceiveWithTick"},
        };
    // clang-format on

    RegisterHandlers(functions);

    receive_event = service_context.CreateEvent("IReceiver::ReceiveEvent");

    // The agent signals from whichever thread sends, under its lock; the event outlives the state
    // (the state is closed in the destructor before the event is).
    state = std::make_unique<OvlnAgent::ReceiverState>(
        [this] { receive_event->Signal(system.Kernel()); }, [this] { receive_event->Clear(system.Kernel()); });
}

IReceiver::~IReceiver() {
    GetOvlnAgent().CloseReceiver(*state);
    service_context.CloseEvent(receive_event);
}

Result IReceiver::AddSource(SourceName source_name) {
    LOG_INFO(Service_PSC, "called: source_name={}", source_name.GetString());
    R_RETURN(ToResult(GetOvlnAgent().AddSource(*state, source_name)));
}

Result IReceiver::RemoveSource(SourceName source_name) {
    LOG_INFO(Service_PSC, "called: source_name={}", source_name.GetString());
    R_RETURN(ToResult(GetOvlnAgent().RemoveSource(*state, source_name)));
}

Result IReceiver::GetReceiveEventHandle(OutCopyHandle<Kernel::KReadableEvent> out_event) {
    LOG_DEBUG(Service_PSC, "called");
    *out_event = &receive_event->GetReadableEvent();
    R_SUCCEED();
}

Result IReceiver::Receive(Out<OverlayNotification> out_notification) {
    u64 tick{};
    R_RETURN(ReceiveWithTick(out_notification, Out<u64>(&tick)));
}

Result IReceiver::ReceiveWithTick(Out<OverlayNotification> out_notification, Out<u64> out_tick) {
    // An empty receive is 2025-0012; overlayDisp aborts on any other empty result.
    const auto status = GetOvlnAgent().Receive(*state, *out_notification, *out_tick);
    if (status == OvlnStatus::Success) {
        LOG_DEBUG(Service_PSC, "received tag={} size={:#x}", GetNotificationTag(*out_notification),
                  GetNotificationPayloadSize(*out_notification));
    }
    R_RETURN(ToResult(status));
}

} // namespace Service::PSC

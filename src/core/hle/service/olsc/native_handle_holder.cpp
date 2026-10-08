// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/olsc/native_handle_holder.h"

#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_readable_event.h"
#include "core/openpak/async_worker.h"

namespace Service::OLSC {

INativeHandleHolder::INativeHandleHolder(Core::System& system_)
    : ServiceFramework{system_, "INativeHandleHolder"}, service_context{system_, "OLSC"} {
    event = service_context.CreateEvent("OLSC::INativeHandleHolder");
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, D<&INativeHandleHolder::GetNativeHandle>, "GetNativeHandle"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

INativeHandleHolder::INativeHandleHolder(
    Core::System& system_, std::shared_ptr<OpenPak::AsyncWorker> worker,
    const std::function<void(std::shared_ptr<OpenPak::HostEvent>)>& attach)
    : INativeHandleHolder{system_} {
    host_event = worker->MakeHostEvent(event);
    attach(host_event);
}

INativeHandleHolder::~INativeHandleHolder() {
    if (host_event) {
        host_event->Detach();
    }
    service_context.CloseEvent(event);
    event = nullptr;
}

Result INativeHandleHolder::GetNativeHandle(OutCopyHandle<Kernel::KReadableEvent> out_event) {
    if (event) {
        // An event a host thread raises is signalled only when that happens; the plain holder
        // keeps Eden's answer (signalled at once).
        if (!host_event) {
            LOG_WARNING(Service_OLSC, "(STUBBED) called");
            event->Signal(system.Kernel());
        }
        *out_event = std::addressof(event->GetReadableEvent());
    } else {
        *out_event = nullptr;
    }
    R_SUCCEED();
}

} // namespace Service::OLSC

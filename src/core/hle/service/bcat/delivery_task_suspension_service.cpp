// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include "core/core.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/service/bcat/delivery_task_suspension_service.h"
#include "core/hle/service/cmif_serialization.h"

namespace Service::BCAT {

IDeliveryTaskSuspensionService::IDeliveryTaskSuspensionService(Core::System& system_)
    : ServiceFramework{system_, "IDeliveryTaskSuspensionService"}, service_context{
                                                                       system_,
                                                                       "IDeliveryTaskSuspensionService"} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, D<&IDeliveryTaskSuspensionService::GetEvent>, "GetEvent"},
    };
    // clang-format on

    RegisterHandlers(functions);

    suspended_event = service_context.CreateEvent("IDeliveryTaskSuspensionService::SuspendedEvent");
    suspended_event->Signal(system.Kernel());
}

IDeliveryTaskSuspensionService::~IDeliveryTaskSuspensionService() {
    service_context.CloseEvent(suspended_event);
}

Result IDeliveryTaskSuspensionService::GetEvent(OutCopyHandle<Kernel::KReadableEvent> out_event) {
    *out_event = &suspended_event->GetReadableEvent();
    R_SUCCEED();
}

} // namespace Service::BCAT

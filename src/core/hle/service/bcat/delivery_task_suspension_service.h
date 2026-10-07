// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "core/hle/service/cmif_types.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/service.h"

namespace Core {
class System;
}

namespace Kernel {
class KEvent;
class KReadableEvent;
} // namespace Kernel

namespace Service::BCAT {

// nn::bcat::ipc::IDeliveryTaskSuspensionService (8.0.0+): delivery tasks stay suspended while the
// object lives, and the event tells the holder they have stopped. No background delivery runs here,
// so they have stopped already: the event is signalled when it is created. Behaviour follows the
// Ryujinx Harbor (MIT-licensed Ryujinx base).
class IDeliveryTaskSuspensionService final
    : public ServiceFramework<IDeliveryTaskSuspensionService> {
public:
    explicit IDeliveryTaskSuspensionService(Core::System& system_);
    ~IDeliveryTaskSuspensionService() override;

private:
    Result GetEvent(OutCopyHandle<Kernel::KReadableEvent> out_event);

    KernelHelpers::ServiceContext service_context;
    Kernel::KEvent* suspended_event;
};

} // namespace Service::BCAT

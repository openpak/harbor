// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <memory>

#include "core/hle/service/cmif_types.h"
#include "core/hle/service/service.h"

#include "core/hle/service/kernel_helpers.h"

namespace Kernel {
class KReadableEvent;
class KEvent;
}

namespace OpenPak {
class AsyncWorker;
class HostEvent;
} // namespace OpenPak

namespace Service::OLSC {

class INativeHandleHolder final : public ServiceFramework<INativeHandleHolder> {
public:
    explicit INativeHandleHolder(Core::System& system_);
    /// An event host threads raise: `attach` receives it (e.g. the transfer queue's start or
    /// end event list); it is detached when the holder goes.
    INativeHandleHolder(Core::System& system_, std::shared_ptr<OpenPak::AsyncWorker> worker,
                        const std::function<void(std::shared_ptr<OpenPak::HostEvent>)>& attach);
    ~INativeHandleHolder() override;

private:
    Result GetNativeHandle(OutCopyHandle<Kernel::KReadableEvent> out_event);

    Service::KernelHelpers::ServiceContext service_context;
    Kernel::KEvent* event{};
    std::shared_ptr<OpenPak::HostEvent> host_event;
};

} // namespace Service::OLSC

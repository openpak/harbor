// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>

#include "core/hle/service/cmif_types.h"
#include "core/hle/service/service.h"

namespace Core {
class System;
}

namespace Kernel {
class KEvent;
class KReadableEvent;
} // namespace Kernel

namespace Service::News {

class NewsShared;

class INewlyArrivedEventHolder final : public ServiceFramework<INewlyArrivedEventHolder> {
public:
    // Both holders hand out an event of the shared news state, so one signal reaches every waiter.
    explicit INewlyArrivedEventHolder(Core::System& system_, std::shared_ptr<NewsShared> shared_);
    ~INewlyArrivedEventHolder() override;

private:
    Result Get(OutCopyHandle<Kernel::KReadableEvent> out_event);

    std::shared_ptr<NewsShared> shared;
};

} // namespace Service::News

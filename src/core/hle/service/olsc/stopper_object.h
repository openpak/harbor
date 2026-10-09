// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>

#include "core/hle/service/cmif_types.h"
#include "core/hle/service/service.h"

namespace Service::OLSC {

class IStopperObject final : public ServiceFramework<IStopperObject> {
public:
    /// `release` runs when the guest closes the object (the stopper's hold ends).
    explicit IStopperObject(Core::System& system_, std::function<void()> release = {});
    ~IStopperObject() override;

private:
    std::function<void()> release;
};

} // namespace Service::OLSC

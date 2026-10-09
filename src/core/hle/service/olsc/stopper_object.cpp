// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/olsc/stopper_object.h"

namespace Service::OLSC {

IStopperObject::IStopperObject(Core::System& system_, std::function<void()> release_)
    : ServiceFramework{system_, "IStopperObject"}, release{std::move(release_)} {}

IStopperObject::~IStopperObject() {
    if (release) {
        release();
    }
}

} // namespace Service::OLSC

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

#include "core/hle/service/cmif_types.h"
#include "core/hle/service/service.h"

namespace Core {
class System;
}

namespace Kernel {
class KReadableEvent;
}

namespace Service::OMM {

class IdlePolicy;

// idle:sys. The timer itself is IdlePolicy; am hands it the context of the applet in front
// directly (its SetHandlingContext over IPC on a console).
class IPolicyManagerSystem final : public ServiceFramework<IPolicyManagerSystem> {
public:
    explicit IPolicyManagerSystem(Core::System& system_);
    ~IPolicyManagerSystem() override;

private:
    Result GetAutoPowerDownEvent(OutCopyHandle<Kernel::KReadableEvent> out_event);
    Result IsAutoPowerDownRequested(Out<bool> out_is_requested);
    Result IsDimmed(Out<bool> out_is_dimmed);
    void SetHandlingContext(HLERequestContext& ctx);
    Result LoadAndApplySettings();
    Result ReportUserIsActive();

    std::unique_ptr<IdlePolicy> m_policy;
};

} // namespace Service::OMM

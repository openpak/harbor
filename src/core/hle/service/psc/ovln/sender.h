// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

#include "core/hle/service/cmif_types.h"
#include "core/hle/service/psc/ovln/ovln_agent.h"
#include "core/hle/service/psc/ovln/ovln_types.h"
#include "core/hle/service/service.h"

namespace Service::PSC {

class ISender final : public ServiceFramework<ISender> {
public:
    explicit ISender(Core::System& system_, std::shared_ptr<OvlnAgent::SenderState> state_);
    ~ISender() override;

private:
    Result Send(u64 option, const OverlayNotification& notification);
    Result GetUnreceivedMessageCount(Out<u32> out_count);

    std::shared_ptr<OvlnAgent::SenderState> state;
};

} // namespace Service::PSC

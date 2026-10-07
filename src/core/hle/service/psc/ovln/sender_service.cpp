// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).

#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/psc/ovln/ovln_result.h"
#include "core/hle/service/psc/ovln/sender.h"
#include "core/hle/service/psc/ovln/sender_service.h"

namespace Service::PSC {

ISenderService::ISenderService(Core::System& system_) : ServiceFramework{system_, "ovln:snd"} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, D<&ISenderService::OpenSender>, "OpenSender"},
    };
    // clang-format on

    RegisterHandlers(functions);

    // A new session: nothing is open yet, so whatever an earlier one left is dropped.
    GetOvlnAgent().Reset();
}

ISenderService::~ISenderService() = default;

// The source name, then a queue attribute of which only the low u32 (the queue length) is read.
Result ISenderService::OpenSender(Out<SharedPointer<ISender>> out_sender, SourceName source_name,
                                  u64 queue_attribute) {
    std::shared_ptr<OvlnAgent::SenderState> state;
    R_TRY(ToResult(GetOvlnAgent().OpenSender(source_name, static_cast<u32>(queue_attribute), state)));

    LOG_DEBUG(Service_PSC, "sender opened on \"{}\", queue {}", source_name.GetString(),
              static_cast<u32>(queue_attribute));
    *out_sender = std::make_shared<ISender>(system, std::move(state));
    R_SUCCEED();
}

} // namespace Service::PSC

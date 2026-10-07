// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/hle/service/pctl/parental_control_service_factory.h"
#include "core/hle/service/pctl/pctl.h"
#include "core/hle/service/pctl/pctl_pairing.h"
#include "core/hle/service/server_manager.h"
#include "core/openpak/async_worker.h"
#include "core/openpak/push.h"

namespace Service::PCTL {

void LoopProcess(Core::System& system) {
    auto server_manager = std::make_unique<ServerManager>(system);

    // [Harbor] Pairing work and the guest events and overlay notifications it raises run on this
    // session's OpenPak worker. The parent's pushes: a changed setting is fetched at once
    // (nx-pctl moon.go pctlPush), an unlink forgets the pairing and tells the overlay (pctl's
    // DID_UNLINK_NX handler 0x4fb30).
    const auto worker = std::make_shared<OpenPak::AsyncWorker>(system, "pctl:OpenPak");
    Pairing::SetWorker(worker);
    OpenPak::Push::SetHook("pctl", [](const std::string& kind, const std::string& body) {
        if (kind == "DID_CHANGE_PARENTAL_CONTROL_SETTING") {
            Pairing::Current().SyncSoon();
        } else if (kind == "DID_UNLINK_NX") {
            Pairing::Current().OnUnlinkPush(body);
        }
    });
    // Test runs with OPENPAK_SIMULATE_PUSH_DIR deliver the parent's pushes from files, with or
    // without a server (idempotent; does nothing when the variable is unset).
    OpenPak::Push::StartSimulated();

    server_manager->RegisterNamedService("pctl",
                                         std::make_shared<IParentalControlServiceFactory>(
                                             system, "pctl",
                                             Capability::Application | Capability::SnsPost |
                                                 Capability::Status | Capability::StereoVision,
                                             worker));
    // The system ports' capabilities as a console grants them; qlaunch holds only pctl:s and
    // fails to initialise with none.
    server_manager->RegisterNamedService("pctl:a", std::make_shared<IParentalControlServiceFactory>(
                                                       system, "pctl:a", static_cast<Capability>(0x83BE), worker));
    server_manager->RegisterNamedService("pctl:r", std::make_shared<IParentalControlServiceFactory>(
                                                       system, "pctl:r", static_cast<Capability>(0x8040), worker));
    server_manager->RegisterNamedService("pctl:s", std::make_shared<IParentalControlServiceFactory>(
                                                       system, "pctl:s", static_cast<Capability>(0x838E), worker));
    ServerManager::RunServer(std::move(server_manager));
}

} // namespace Service::PCTL

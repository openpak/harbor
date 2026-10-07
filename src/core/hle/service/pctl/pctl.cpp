// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/hle/service/pctl/parental_control_service_factory.h"
#include "core/hle/service/pctl/pctl.h"
#include "core/hle/service/server_manager.h"

namespace Service::PCTL {

void LoopProcess(Core::System& system) {
    auto server_manager = std::make_unique<ServerManager>(system);

    server_manager->RegisterNamedService("pctl",
                                         std::make_shared<IParentalControlServiceFactory>(
                                             system, "pctl",
                                             Capability::Application | Capability::SnsPost |
                                                 Capability::Status | Capability::StereoVision));
    // The system ports' capabilities as a console grants them; qlaunch holds only pctl:s and
    // fails to initialise with none.
    server_manager->RegisterNamedService("pctl:a", std::make_shared<IParentalControlServiceFactory>(
                                                       system, "pctl:a", static_cast<Capability>(0x83BE)));
    server_manager->RegisterNamedService("pctl:r", std::make_shared<IParentalControlServiceFactory>(
                                                       system, "pctl:r", static_cast<Capability>(0x8040)));
    server_manager->RegisterNamedService("pctl:s", std::make_shared<IParentalControlServiceFactory>(
                                                       system, "pctl:s", static_cast<Capability>(0x838E)));
    ServerManager::RunServer(std::move(server_manager));
}

} // namespace Service::PCTL

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/hle/service/service.h"

namespace Core {
class System;
}

namespace Service::Friend {

class FriendsSession;

class Module final {
public:
    /// [OpenPak] This emulation session's friends state, shared by every port.
    std::shared_ptr<FriendsSession> session;

    class Interface : public ServiceFramework<Interface> {
    public:
        explicit Interface(std::shared_ptr<Module> module_, Core::System& system_,
                           const char* name);
        ~Interface() override;

        void CreateFriendService(HLERequestContext& ctx);
        void CreateNotificationService(HLERequestContext& ctx);
        void CreateDaemonSuspendSessionService(HLERequestContext& ctx);

    protected:
        std::shared_ptr<Module> module;
    };
};

void LoopProcess(Core::System& system);

} // namespace Service::Friend

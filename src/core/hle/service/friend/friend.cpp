// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <string_view>

#include "common/logging.h"
#include "common/uuid.h"
#include "core/core.h"
#include "core/hle/service/friend/friend.h"
#include "core/hle/service/friend/friend_interface.h"
#include "core/hle/service/friend/friend_service.h"
#include "core/hle/service/friend/friends_session.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/server_manager.h"

namespace Service::Friend {

/// IServiceCreator 2: the module's suspend session has no commands; holding it open is what
/// suspends the daemon's background work, which has no equivalent here.
class IDaemonSuspendSessionService final : public ServiceFramework<IDaemonSuspendSessionService> {
public:
    explicit IDaemonSuspendSessionService(Core::System& system_)
        : ServiceFramework{system_, "IDaemonSuspendSessionService"} {}
};

void Module::Interface::CreateFriendService(HLERequestContext& ctx) {
    LOG_DEBUG(Service_Friend, "called");
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface(
        ctx, ::Service::Friend::CreateFriendService(system, module->session, GetServiceName()));
}

void Module::Interface::CreateNotificationService(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto uuid = rp.PopRaw<Common::UUID>();

    LOG_DEBUG(Service_Friend, "called");

    // A queue is per user, and the module refuses one for nobody (2121-0002).
    if (uuid.IsInvalid()) {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(Result{ErrorModule::Friends, 2});
        return;
    }

    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface(ctx, ::Service::Friend::CreateNotificationService(
                                 system, module->session, GetServiceName(), uuid));
}

void Module::Interface::CreateDaemonSuspendSessionService(HLERequestContext& ctx) {
    LOG_DEBUG(Service_Friend, "called");
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<IDaemonSuspendSessionService>(ctx, system);
}

Module::Interface::Interface(std::shared_ptr<Module> module_, Core::System& system_, const char* name)
    : ServiceFramework{system_, name}, module{std::move(module_)}
{}

Module::Interface::~Interface() = default;

class IServiceForApplication final : public ServiceFramework<IServiceForApplication> {
public:
    explicit IServiceForApplication(Core::System& system_)
        : ServiceFramework{system_, "nd:app"}
    {
        static const FunctionInfo functions[] = {
            {0, nullptr, "GetReceivableNeighborInfoCountMax"},
            {10, nullptr, "IsNeighborDetectionEnabled"},
        };
        RegisterHandlers(functions);
    }
};

class IServiceForSystem final : public ServiceFramework<IServiceForSystem> {
public:
    explicit IServiceForSystem(Core::System& system_)
        : ServiceFramework{system_, "nd:sys"}
    {
        static const FunctionInfo functions[] = {
            {0, nullptr, "GetReceivableNeighborInfoCountMax"},
            {10, nullptr, "IsNeighborDetectionEnabled"},
            {200, nullptr, "SetSystemData"},
            {201, nullptr, "ClearSystemData"},
            {203, nullptr, "GetReceivableNeighborInfoCountForSystem"},
            {204, nullptr, "ReceiveNeighborInfoForSystem"},
            {205, nullptr, "SetSender"},
            {206, nullptr, "GetSender"},
            {207, nullptr, "CreateScannerForSystem"},
            {208, nullptr, "CreateReceiveEventHolderForSystem"},
            {223, nullptr, "EnableNeighborDetection"},
            {224, nullptr, "DisableNeighborDetection"},
            {226, nullptr, "EnablePowerSave"},
            {227, nullptr, "DisablePowerSave"},
            {228, nullptr, "IsPowerSaveEnabled"},
            {232, nullptr, "ClearBlockedUsers"},
            {233, nullptr, "GetBlockedUserCount"},
            {234, nullptr, "BlockUserByLocalUserId"},
            {235, nullptr, "BlockUserByNetworkUserId"},
            {236, nullptr, "UnblockUserByLocalUserId"},
            {237, nullptr, "UnblockUserByNetworkUserId"},
            {240, nullptr, "DeleteApplication"},
            {250, nullptr, "InitializeApplicationInfo"},
            {260, nullptr, "CreateAccountSystemSaveDataAccessSuppressor"},
            {300, nullptr, "AddReceivedNeighborInfoForSystemForDebug"},
            {301, nullptr, "GetSendDataForDebug"},
            {302, nullptr, "ClearReceiveCounterForDebug"},
            {303, nullptr, "GetNextReceiveCounterForDebug"},
            {304, nullptr, "ListBlockedUsersForDebug"},
            {305, nullptr, "RefreshSendDataIdForDebug"},
            {306, nullptr, "ReloadFwdbgSettingsForDebug"},
            {307, nullptr, "EnableApplicationForDebug"},
            {308, nullptr, "GetNextReceiveCountersForDebug"},
            {309, nullptr, "ListApplicationInfoForDebug"},
            {310, nullptr, "SetApplicationDataForDebug"},
            {400, nullptr, "GetNetworkUserId"},
            {401, nullptr, "DeleteNetworkUserId"},
        };
        RegisterHandlers(functions);
    }
};

void LoopProcess(Core::System& system) {
    auto server_manager = std::make_unique<ServerManager>(system);
    auto module = std::make_shared<Module>();
    // [OpenPak] The module's state for this emulation session: OpenPak's hooks reach the guest
    // through it until the next session replaces it.
    module->session = std::make_shared<FriendsSession>(system);
    FriendsSession::Begin(module->session);

    server_manager->RegisterNamedService("nd:app", std::make_shared<IServiceForApplication>(system));
    server_manager->RegisterNamedService("nd:sys", std::make_shared<IServiceForSystem>(system));

    // [OpenPak] With the firmware's friends program on the real-firmware list, these register
    // nothing (server_manager leaves hosted names to it).
    server_manager->RegisterNamedService("friend:a", std::make_shared<Friend>(module, system, "friend:a"));
    server_manager->RegisterNamedService("friend:m", std::make_shared<Friend>(module, system, "friend:m"));
    server_manager->RegisterNamedService("friend:s", std::make_shared<Friend>(module, system, "friend:s"));
    server_manager->RegisterNamedService("friend:u", std::make_shared<Friend>(module, system, "friend:u"));
    server_manager->RegisterNamedService("friend:v", std::make_shared<Friend>(module, system, "friend:v"));

    ServerManager::RunServer(std::move(server_manager));
}

} // namespace Service::Friend

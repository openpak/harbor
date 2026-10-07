// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>

#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/ns/async_result.h"
#include "core/hle/service/ns/develop_interface.h"
#include "core/hle/service/os/event.h"
#include "core/hle/service/ns/ns.h"
#include "core/hle/service/ns/platform_service_manager.h"
#include "core/hle/service/ns/query_service.h"
#include "core/hle/service/ns/service_getter_interface.h"
#include "core/hle/service/ns/system_update_interface.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/cmif_types.h"
#include "core/hle/service/server_manager.h"

namespace Service::NS {

class INotifyService final : public ServiceFramework<INotifyService> {
public:
    explicit INotifyService(Core::System& system_) : ServiceFramework{system_, "pdm:ntfy"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            { 0, nullptr, "NotifyAppletEvent" },
            { 2, nullptr, "NotifyOperationModeChangeEvent" },
            { 3, nullptr, "NotifyPowerStateChangeEvent" },
            { 4, nullptr, "NotifyClearAllEvent" },
            { 5, nullptr, "NotifyEventForDebug" },
            { 6, nullptr, "SuspendUserAccountEventService" },
            { 7, nullptr, "ResumeUserAccountEventService" },
            { 8, nullptr, "NotifyLibraryAppletEvent" },
            { 9, nullptr, "Cmd9" },
            { 20, nullptr, "Cmd20" },
            { 30, nullptr, "Cmd30" },
            { 100, nullptr, "Cmd100" },
            { 101, nullptr, "Cmd101" },
        };
        // clang-format on
        RegisterHandlers(functions);
    }
};

class IVulnerabilityManagerInterface final
    : public ServiceFramework<IVulnerabilityManagerInterface> {
public:
    explicit IVulnerabilityManagerInterface(Core::System& system_)
        : ServiceFramework{system_, "ns:vm"}, service_context{system_, "ns:vm"},
          async_completed_event{service_context} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {1200, D<&IVulnerabilityManagerInterface::NeedsUpdateVulnerability>, "NeedsUpdateVulnerability"},
            {1201, nullptr, "UpdateSafeSystemVersionForDebug"},
            {1202, nullptr, "GetSafeSystemVersion"},
            {3100, D<&IVulnerabilityManagerInterface::GetSafeSystemVersionCheckInfo>, "GetSafeSystemVersionCheckInfo"},
            {3101, D<&IVulnerabilityManagerInterface::RequestUpdateSafeSystemVersionCheckInfo>, "RequestUpdateSafeSystemVersionCheckInfo"},
            {3102, D<&IVulnerabilityManagerInterface::ResetSafeSystemVersionCheckInfo>, "ResetSafeSystemVersionCheckInfo"},
        };
        // clang-format on
        RegisterHandlers(functions);
    }
    ~IVulnerabilityManagerInterface() override = default;

    Result NeedsUpdateVulnerability(Out<bool> out_needs_update_vulnerability) {
        LOG_WARNING(Service_NS, "(STUBBED)");
        *out_needs_update_vulnerability = false;
        R_SUCCEED();
    }

    Result GetSafeSystemVersionCheckInfo(Out<std::array<u8, 0x20>> out_system_version_check_info) {
        // The firmware web applets read this before they load a page. Layout from its consumer
        // (systemWeb checkForNonWifiWebAuth / checkForWifiWebAuth): { s64 state; s64 age_ns; s64
        // valid; s64 age_ns }. State 3 younger than 24 hours lets the page load; 1 and 2 end in the
        // applet's own error; anything else after a refresh (3101) is an assertion. The state names
        // are not public: 3 is taken as "checked, this system version is safe", the only true
        // answer here. UNKNOWN CONTRACT for the field meanings. Follows the Ryujinx Harbor.
        LOG_DEBUG(Service_NS, "unknown contract: ns:vm 3100 field meanings; reporting state 3");
        const std::array<s64, 4> info{3, 0, 1, 0};
        std::memcpy(out_system_version_check_info->data(), info.data(), sizeof(info));
        R_SUCCEED();
    }

    Result ResetSafeSystemVersionCheckInfo() {
        LOG_WARNING(Service_NS, "(STUBBED)");
        R_SUCCEED();
    }

    // Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base). On a console ns asks nim
    // for the lowest system version still considered safe (a network request). Nothing is asked
    // here: the request completes at once.
    Result RequestUpdateSafeSystemVersionCheckInfo(OutInterface<IAsyncResult> out_interface,
                                                   OutCopyHandle<Kernel::KReadableEvent> out_event) {
        LOG_INFO(Service_NS, "safe system version check requested: completed");
        async_completed_event.Signal(system.Kernel());
        *out_event = async_completed_event.GetHandle();
        *out_interface = std::make_shared<IAsyncResult>(system, &async_completed_event);
        R_SUCCEED();
    }

private:
    KernelHelpers::ServiceContext service_context;
    Event async_completed_event;
};

void LoopProcess(Core::System& system) {
    auto server_manager = std::make_unique<ServerManager>(system);

    server_manager->RegisterNamedService("ns:am2", std::make_shared<IServiceGetterInterface>(system, "ns:am2"), 5);
    server_manager->RegisterNamedService("ns:ec", std::make_shared<IServiceGetterInterface>(system, "ns:ec"), 5);
    server_manager->RegisterNamedService("ns:rid", std::make_shared<IServiceGetterInterface>(system, "ns:rid"), 5);
    server_manager->RegisterNamedService("ns:rt", std::make_shared<IServiceGetterInterface>(system, "ns:rt"), 5);
    server_manager->RegisterNamedService("ns:web", std::make_shared<IServiceGetterInterface>(system, "ns:web"), 5);
    server_manager->RegisterNamedService("ns:ro", std::make_shared<IServiceGetterInterface>(system, "ns:ro"), 5);
    server_manager->RegisterNamedService("ns:sweb", std::make_shared<IServiceGetterInterface>(system, "ns:sweb"), 5); // 15.0.0+, systemWeb

    server_manager->RegisterNamedService("ns:dev", std::make_shared<IDevelopInterface>(system), 5);
    server_manager->RegisterNamedService("ns:su", std::make_shared<ISystemUpdateInterface>(system), 5);
    server_manager->RegisterNamedService("ns:vm", std::make_shared<IVulnerabilityManagerInterface>(system), 5);
    server_manager->RegisterNamedService("pdm:ntfy", std::make_shared<INotifyService>(system));
    server_manager->RegisterNamedService("pdm:qry", std::make_shared<IQueryService>(system));

    server_manager->RegisterNamedService("pl:s", std::make_shared<IPlatformServiceManager>(system, "pl:s"));
    server_manager->RegisterNamedService("pl:u", std::make_shared<IPlatformServiceManager>(system, "pl:u"));
    ServerManager::RunServer(std::move(server_manager));
}

} // namespace Service::NS

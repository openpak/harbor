// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <tuple>
#include "common/assert.h"
#include "common/scope_exit.h"
#include "core/core.h"
#include "core/hle/kernel/k_client_port.h"
#include "core/hle/kernel/k_client_session.h"
#include "core/hle/kernel/k_port.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_scoped_resource_reservation.h"
#include "core/hle/kernel/k_server_port.h"
#include "core/hle/result.h"
#include "core/hle/service/cmif_types.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/sm/sm.h"
#include "core/openpak/guest_ipc_trace.h"
#include "core/hle/service/sm/sm_controller.h"

namespace Service::SM {

constexpr Result ResultInvalidClient(ErrorModule::SM, 2);
constexpr Result ResultAlreadyRegistered(ErrorModule::SM, 4);
constexpr Result ResultInvalidServiceName(ErrorModule::SM, 6);
constexpr Result ResultNotRegistered(ErrorModule::SM, 7);
constexpr Result ResultNotAllowed(ErrorModule::SM, 8);

ServiceManager::ServiceManager(Kernel::KernelCore& kernel_) : kernel{kernel_} {
    controller_interface = std::make_unique<Controller>(kernel.System());
}

ServiceManager::~ServiceManager() {
    for (auto& [name, port] : service_ports) {
        port->Close(kernel);
    }

    if (deferral_event) {
        deferral_event->Close(kernel);
    }
}

void ServiceManager::InvokeControlRequest(HLERequestContext& context) {
    controller_interface->InvokeRequest(context);
}

static Result ValidateServiceName(const std::string& name) {
    if (name.empty() || name.size() > 8) {
        LOG_ERROR(Service_SM, "Invalid service name! service={}", name);
        return Service::SM::ResultInvalidServiceName;
    }
    return ResultSuccess;
}

Result ServiceManager::RegisterService(Kernel::KServerPort** out_server_port, std::string name, u32 max_sessions, SessionRequestHandlerFactory handler) {
    R_TRY(ValidateServiceName(name));

    std::scoped_lock lk{lock};
    if (registered_services.find(name) != registered_services.end()) {
        LOG_ERROR(Service_SM, "Service is already registered! service={}", name);
        return Service::SM::ResultAlreadyRegistered;
    }

    auto* port = Kernel::KPort::Create(kernel);
    port->Initialize(kernel, max_sessions, false, 0);

    // Register the port.
    Kernel::KPort::Register(kernel, port);

    service_ports.emplace(name, std::addressof(port->GetClientPort()));
    registered_services.emplace(name, handler);
    if (deferral_event) {
        deferral_event->Signal(kernel);
    }

    // Set our output.
    *out_server_port = std::addressof(port->GetServerPort());

    // We succeeded.
    R_SUCCEED();
}

Result ServiceManager::UnregisterService(const std::string& name) {
    R_TRY(ValidateServiceName(name));

    std::scoped_lock lk{lock};
    const auto iter = registered_services.find(name);
    if (iter == registered_services.end()) {
        LOG_ERROR(Service_SM, "Server is not registered! service={}", name);
        return Service::SM::ResultNotRegistered;
    }

    registered_services.erase(iter);
    service_ports.erase(name);

    return ResultSuccess;
}

Result ServiceManager::GetServicePort(Kernel::KClientPort** out_client_port,
                                      const std::string& name) {
    R_TRY(ValidateServiceName(name));

    std::scoped_lock lk{lock};
    auto it = service_ports.find(name);
    if (it == service_ports.end()) {
        LOG_WARNING(Service_SM, "Server is not registered! service={}", name);
        return Service::SM::ResultNotRegistered;
    }

    *out_client_port = it->second;
    return ResultSuccess;
}

bool ServiceManager::HasService(const std::string& name) const {
    std::scoped_lock lk{lock};
    return registered_services.contains(name);
}

/**
 * SM::Initialize service function
 *  Inputs:
 *      0: 0x00000000
 *  Outputs:
 *      0: Result
 */
void SM::Initialize(HLERequestContext& ctx) {
    LOG_DEBUG(Service_SM, "called");

    ctx.GetManager()->SetIsInitializedForSm();

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void SM::GetServiceCmif(HLERequestContext& ctx) {
    Kernel::KClientSession* client_session{};
    auto result = GetServiceImpl(&client_session, ctx);
    if (ctx.GetIsDeferred()) {
        // Don't overwrite the command buffer.
        return;
    }

    if (result == ResultSuccess) {
        IPC::ResponseBuilder rb{ctx, 2, 0, 1, IPC::ResponseBuilder::Flags::AlwaysMoveHandles};
        rb.Push(result);
        rb.PushMoveObjects(ctx, client_session);
    } else {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(result);
    }
}

void SM::GetServiceTipc(HLERequestContext& ctx) {
    Kernel::KClientSession* client_session{};
    auto result = GetServiceImpl(&client_session, ctx);
    if (ctx.GetIsDeferred()) {
        // Don't overwrite the command buffer.
        return;
    }

    IPC::ResponseBuilder rb{ctx, 2, 0, 1, IPC::ResponseBuilder::Flags::AlwaysMoveHandles};
    rb.Push(result);
    rb.PushMoveObjects(ctx, result == ResultSuccess ? client_session : nullptr);
}

static std::string PopServiceName(IPC::RequestParser& rp) {
    auto name_buf = rp.PopRaw<std::array<char, 8>>();
    std::string result;
    for (const auto& c : name_buf) {
        if (c >= ' ' && c <= '~') {
            result.push_back(c);
        }
    }
    return result;
}

// The firmware's own programs probe ports from the least privileged up ("friend:u" before
// "friend:m") and rely on sm refusing the ones outside their NPDM service access list; granted the
// first, they run with too few rights (MyPage then fails with 2121-0090). Only firmware programs
// (0100000000000000-010000000000FFFF) are checked; games and homebrew keep an open sm.
// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).
bool SM::IsAccessAllowed(HLERequestContext& ctx, std::string_view name, bool is_host) {
    const auto* const process = ctx.GetThread().GetOwnerProcess();
    if (process == nullptr || (process->GetProgramId() & ~u64{0xFFFF}) != 0x0100000000000000) {
        return true;
    }

    const u64 program_id = process->GetProgramId();
    std::scoped_lock lk{access_lock};
    auto it = access_lists.find(program_id);
    if (it == access_lists.end()) {
        const auto metadata =
            FileSys::ProgramMetadata::LoadInstalled(system.GetContentProvider(), program_id);
        std::optional<std::vector<FileSys::ProgramMetadata::ServiceAccessEntry>> list;
        if (metadata) {
            list = metadata->GetServiceAccessList();
        }

        if (list && !list->empty()) {
            std::string names;
            std::string unserved;
            for (const auto& entry : *list) {
                names += fmt::format(" {}{}", entry.is_host ? "+" : "", entry.name);
                if (!entry.is_host && !entry.name.ends_with('*') &&
                    !service_manager.HasService(entry.name)) {
                    unserved += " " + entry.name;
                }
            }
            LOG_INFO(Service_SM, "program {:016X}: service access list (+ = host):{}", program_id,
                     names);
            if (!unserved.empty()) {
                LOG_INFO(Service_SM, "program {:016X}: may open services nothing serves:{}",
                         program_id, unserved);
            }
        } else {
            list.reset();
            LOG_WARNING(Service_SM, "program {:016X}: no NPDM service access list, not checked",
                        program_id);
        }
        it = access_lists.emplace(program_id, std::move(list)).first;
    }

    if (!it->second ||
        FileSys::ProgramMetadata::IsServiceAccessAllowed(*it->second, name, is_host)) {
        return true;
    }

    LOG_INFO(Service_SM, "program {:016X}: {} '{}' refused, not in its service access list",
             program_id, is_host ? "RegisterService" : "GetService", name);
    return false;
}

Result SM::GetServiceImpl(Kernel::KClientSession** out_client_session, HLERequestContext& ctx) {
    if (!ctx.GetManager()->GetIsInitializedForSm()) {
        return Service::SM::ResultInvalidClient;
    }

    IPC::RequestParser rp{ctx};
    std::string name(PopServiceName(rp));

    // Find the named port.
    Kernel::KClientPort* client_port{};
    auto port_result = service_manager.GetServicePort(&client_port, name);
    if (port_result == Service::SM::ResultInvalidServiceName) {
        LOG_ERROR(Service_SM, "Invalid service name '{}'", name);
        return Service::SM::ResultInvalidServiceName;
    }

    if (!IsAccessAllowed(ctx, name, false)) {
        return Service::SM::ResultNotAllowed;
    }

    if (port_result != ResultSuccess) {
        LOG_INFO(Service_SM, "Waiting for service {} to become available", name);
        ctx.SetIsDeferred();
        return Service::SM::ResultNotRegistered;
    }

    // Create a new session.
    Kernel::KClientSession* session{};
    if (const auto result = client_port->CreateSession(kernel, &session); result.IsError()) {
        LOG_ERROR(Service_SM, "called service={} -> error {:#08x}", name, result.raw);
        return result;
    }

    OpenPak::GuestIpcTrace::NoteSession(session, name);
    *out_client_session = session;
    return ResultSuccess;
}

void SM::RegisterServiceCmif(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    std::string name(PopServiceName(rp));

    const auto is_light = static_cast<bool>(rp.PopRaw<u32>());
    const auto max_session_count = rp.PopRaw<u32>();

    this->RegisterServiceImpl(ctx, name, max_session_count, is_light);
}

void SM::RegisterServiceTipc(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    std::string name(PopServiceName(rp));

    const auto max_session_count = rp.PopRaw<u32>();
    const auto is_light = static_cast<bool>(rp.PopRaw<u32>());

    this->RegisterServiceImpl(ctx, name, max_session_count, is_light);
}

void SM::RegisterServiceImpl(HLERequestContext& ctx, std::string name, u32 max_session_count,
                             bool is_light) {
    LOG_DEBUG(Service_SM, "called with name={}, max_session_count={}, is_light={}", name,
              max_session_count, is_light);

    if (!IsAccessAllowed(ctx, name, true)) {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(Service::SM::ResultNotAllowed);
        return;
    }

    Kernel::KServerPort* server_port{};
    if (const auto result = service_manager.RegisterService(std::addressof(server_port), name, max_session_count, nullptr);
        result.IsError()) {
        LOG_ERROR(Service_SM, "failed to register service with error_code={:08X}", result.raw);
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(result);
        return;
    }

    IPC::ResponseBuilder rb{ctx, 2, 0, 1, IPC::ResponseBuilder::Flags::AlwaysMoveHandles};
    rb.Push(ResultSuccess);
    rb.PushMoveObjects(ctx, server_port);
}

void SM::UnregisterService(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    std::string name(PopServiceName(rp));

    LOG_DEBUG(Service_SM, "called with name={}", name);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(service_manager.UnregisterService(name));
}

void SM::AtmosphereHasService(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    std::string name(PopServiceName(rp));
    LOG_WARNING(Service_SM, "(stubbed) called with name={}", name);
    IPC::ResponseBuilder rb{ctx, 3};
    Kernel::KClientPort* out_client_port = nullptr;
    rb.Push(ResultSuccess);
    rb.Push<bool>(service_manager.GetServicePort(&out_client_port, name) == ResultSuccess);
}

SM::SM(ServiceManager& service_manager_, Core::System& system_)
    : ServiceFramework{system_, "sm:", 64}
    , service_manager{service_manager_}
    , kernel{system_.Kernel()}
{
    RegisterHandlers({
        {0, &SM::Initialize, "Initialize"},
        {1, &SM::GetServiceCmif, "GetService"},
        {2, &SM::RegisterServiceCmif, "RegisterService"},
        {3, &SM::UnregisterService, "UnregisterService"},
        {4, nullptr, "DetachClient"},
        // TODO: are these non-TIPC as well?
        {65000, nullptr, "AtmosphereInstallMitm"},
        {65001, nullptr, "AtmosphereUninstallMitm"},
        {65002, nullptr, "Deprecated_AtmosphereAssociatePidTidForMitm"},
        {65003, nullptr, "AtmosphereAcknowledgeMitmSession"},
        {65004, nullptr, "AtmosphereHasMitm"},
        {65005, nullptr, "AtmosphereWaitMitm"},
        {65006, nullptr, "AtmosphereDeclareFutureMitm"},
        {65100, &SM::AtmosphereHasService, "AtmosphereHasService"},
        {65101, nullptr, "AtmosphereWaitService"},
    });
    RegisterHandlersTipc({
        {0, &SM::Initialize, "Initialize"},
        {1, &SM::GetServiceTipc, "GetService"},
        {2, &SM::RegisterServiceTipc, "RegisterService"},
        {3, &SM::UnregisterService, "UnregisterService"},
        {4, nullptr, "DetachClient"},
        {65000, nullptr, "AtmosphereInstallMitm"},
        {65001, nullptr, "AtmosphereUninstallMitm"},
        {65002, nullptr, "Deprecated_AtmosphereAssociatePidTidForMitm"},
        {65003, nullptr, "AtmosphereAcknowledgeMitmSession"},
        {65004, nullptr, "AtmosphereHasMitm"},
        {65005, nullptr, "AtmosphereWaitMitm"},
        {65006, nullptr, "AtmosphereDeclareFutureMitm"},
        {65100, &SM::AtmosphereHasService, "AtmosphereHasService"},
        {65101, nullptr, "AtmosphereWaitService"},
    });
}

SM::~SM() = default;

void LoopProcess(Core::System& system) {
    auto& service_manager = system.ServiceManager();
    auto server_manager = std::make_unique<ServerManager>(system);

    Kernel::KEvent* deferral_event{};
    server_manager->ManageDeferral(&deferral_event);
    service_manager.SetDeferralEvent(deferral_event);

    auto sm_service = std::make_shared<SM>(system.ServiceManager(), system);
    server_manager->ManageNamedPort("sm:", [sm_service] { return sm_service; });

    ServerManager::RunServer(std::move(server_manager));
}

} // namespace Service::SM

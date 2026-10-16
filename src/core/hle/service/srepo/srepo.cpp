// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

#include <memory>
#include <vector>

#include "common/logging.h"
#include "core/core.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_shared_memory.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/service.h"
#include "core/hle/service/srepo/srepo.h"

namespace Service::SREPO {

namespace {

// IReportService: every command of the report-saving and sending side, answered with success and
// no output. Reports are accepted and dropped; a program that asks for a statistic or a status
// gets nothing, which none of the firmware's callers here reads.
// What srepo:u 13005 hands back: a per-user report session. Every command succeeds and writes
// nothing.
class IReportSession final : public ServiceFramework<IReportSession> {
public:
    explicit IReportSession(Core::System& system_) : ServiceFramework{system_, "IReportSession"} {
        std::vector<FunctionInfo> functions;
        for (u32 id = 0; id < 64; ++id) {
            functions.push_back({id, &IReportSession::Drop, "Report"});
        }
        RegisterHandlers(functions.data(), functions.size());
    }

private:
    void Drop(HLERequestContext& ctx) {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }
};

class IReportService final : public ServiceFramework<IReportService> {
public:
    explicit IReportService(Core::System& system_, const char* name)
        : ServiceFramework{system_, name}, service_context{system_, name} {
        std::vector<FunctionInfo> functions;
        for (const u32 base : {10100u, 10200u, 10300u, 11000u, 11100u, 11200u, 11300u, 13000u,
                               20100u, 20200u, 20400u, 30100u, 30200u, 40100u, 50100u, 90100u}) {
            for (u32 id = base; id < base + 10; ++id) {
                functions.push_back({id, id == 13005 ? &IReportService::OpenSession
                                         : id == 20200 ? &IReportService::GetEvent
                                                       : &IReportService::Drop,
                                     "Report"});
            }
        }
        RegisterHandlers(functions.data(), functions.size());
    }

private:
    // 13005 (npns, friends): the client expects three u32 (shared memory size, offset of its
    // region, region size), a copy handle to a shared memory it maps read-write for its report
    // buffer, and the session object; a reply without the handle is a guest abort in nn::sf.
    // Nothing reads the buffer here.
    void OpenSession(HLERequestContext& ctx) {
        constexpr u32 size = 0x10000;
        if (!report_memory) {
            report_memory = Kernel::KSharedMemory::Create(kernel);
            report_memory->Initialize(kernel, system.DeviceMemory(), nullptr,
                                      Kernel::Svc::MemoryPermission::None,
                                      Kernel::Svc::MemoryPermission::ReadWrite, size);
            Kernel::KSharedMemory::Register(kernel, report_memory);
        }
        IPC::ResponseBuilder rb{ctx, 5, 1, 1};
        rb.Push(ResultSuccess);
        rb.Push<u32>(size);
        rb.Push<u32>(0);
        rb.Push<u32>(size);
        rb.PushCopyObjects(ctx, report_memory);
        rb.PushIpcInterface<IReportSession>(ctx, system);
    }

    // 20200 (prepo, srepo:a): one copy handle, an event the client waits on, and nothing else;
    // a reply without the handle aborts the program. It means "a report waits in the ring":
    // prepo then reads the ring (13005's memory) and aborts on anything that is not a report,
    // so it never fires here (the ring stays empty).
    void GetEvent(HLERequestContext& ctx) {
        if (event == nullptr) {
            event = service_context.CreateEvent("srepo:Event");
        }
        IPC::ResponseBuilder rb{ctx, 2, 1};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, &event->GetReadableEvent());
    }

    KernelHelpers::ServiceContext service_context;
    Kernel::KSharedMemory* report_memory{};
    Kernel::KEvent* event{};

    void Drop(HLERequestContext& ctx) {
        LOG_DEBUG(Service, "report dropped");
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }
};

} // namespace

void LoopProcess(Core::System& system) {
    auto server_manager = std::make_unique<ServerManager>(system);

    server_manager->RegisterNamedService("srepo:a", std::make_shared<IReportService>(system, "srepo:a"));
    server_manager->RegisterNamedService("srepo:u", std::make_shared<IReportService>(system, "srepo:u"));

    ServerManager::RunServer(std::move(server_manager));
}

} // namespace Service::SREPO

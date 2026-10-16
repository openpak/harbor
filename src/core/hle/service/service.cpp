// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <chrono>
#include <fmt/ranges.h>
#include <string_view>
#include <thread>
#include "common/assert.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/arm/debug.h"
#include "core/core.h"
#include "core/hle/ipc.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/service.h"
#include "core/hle/service/sm/sm.h"
#include "core/reporter.h"

namespace Service {

/// @brief Creates a function string for logging: the name (or header code, depending on what's
/// passed in), the port name and the number of argument words. The words themselves are left out:
/// they can hold user ids.
[[maybe_unused]] static std::string MakeFunctionString(std::string_view name, std::string_view port_name, const u32* cmd_buf) {
    // Number of params == bits 0-5 + bits 6-11
    const int num_params = (cmd_buf[0] & 0x3F) + ((cmd_buf[0] >> 6) & 0x3F);
    return fmt::format("fn '{}': port={}, {} argument words", name, port_name, num_params);
}

ServiceFrameworkBase::ServiceFrameworkBase(Core::System& system_, const char* service_name_, u32 max_sessions_, InvokerFn* handler_invoker_)
    : SessionRequestHandler(system_.Kernel(), service_name_)
    , system{system_}
    , service_name{service_name_}
    , is_i_storage{std::string_view{service_name_} == "IStorage"}
    , handler_invoker{handler_invoker_}
    , max_sessions{max_sessions_}
{}

ServiceFrameworkBase::~ServiceFrameworkBase() {
    // Wait for other threads to release access before destroying
    const auto guard = ServiceFrameworkBase::LockService();
}

void ServiceFrameworkBase::RegisterHandlersBase(const FunctionInfoBase* functions, std::size_t n) {
    // Usually this array is sorted by id already, so hint to insert at the end
    handlers.reserve(handlers.size() + n);
    for (std::size_t i = 0; i < n; ++i)
        handlers.emplace_hint(handlers.cend(), functions[i].expected_header, functions[i]);
}

void ServiceFrameworkBase::RegisterHandlersBaseTipc(const FunctionInfoBase* functions, std::size_t n) {
    // Usually this array is sorted by id already, so hint to insert at the end
    handlers_tipc.reserve(handlers_tipc.size() + n);
    for (std::size_t i = 0; i < n; ++i)
        handlers_tipc.emplace_hint(handlers_tipc.cend(), functions[i].expected_header, functions[i]);
}

void ServiceFrameworkBase::ReportUnimplementedFunction(HLERequestContext& ctx,
                                                       const FunctionInfoBase* info) {
    std::string function_name = info == nullptr ? "<unknown>" : info->name;

    // The command id and the port only: the argument words can hold user ids.
    const std::string description =
        fmt::format("function '{}({})': port='{}'", ctx.GetCommand(), function_name, service_name);

    if (std::getenv("OPENPAK_TRACE_FW") != nullptr) {
        const auto* const owner = ctx.GetThread().GetOwnerProcess();
        if (owner != nullptr && (owner->GetProgramId() & ~u64{0xFFFF}) == 0x0100000000000000ULL) {
            const u32* const w = ctx.CommandBuffer();
            LOG_INFO(Service, "fw-unimpl {:016X} [{}] {} request words {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x}",
                     owner->GetProgramId(), Core::GetThreadName(&ctx.GetThread()).value_or(std::string{}),
                     description, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10],
                     w[11], w[12], w[13], w[14], w[15]);
        }
    }
    system.GetReporter().SaveUnimplementedFunctionReport(ctx, ctx.GetCommand(), function_name, service_name);
    UNIMPLEMENTED_MSG("Unknown / unimplemented {}", description);
    if (Settings::values.use_auto_stub) {
        LOG_WARNING(Service, "Using auto stub fallback!");
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
        return;
    }
    // [OpenPak] A well-formed error, not an untouched command buffer: the firmware's own clients
    // read the latter as a corrupt reply and abort the whole program.
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultUnknown);
}

void ServiceFrameworkBase::InvokeRequest(HLERequestContext& ctx) {
    const auto command = ctx.GetCommand();
    auto it = handlers.find(command);
    const bool is_cmd_read = command == 0;
    FunctionInfoBase const* info = it == handlers.end() ? nullptr : &it->second;
    if (info == nullptr || info->handler_callback == nullptr)
        return ReportUnimplementedFunction(ctx, info);

    LOG_TRACE(Service, "{}", MakeFunctionString(info->name, GetServiceName(), ctx.CommandBuffer()));
    // [OpenPak] OPENPAK_TRACE_FW=1: every request the firmware's own system programs send, by port
    // and command, to find the call one of them aborts on.
    static const bool trace_fw = std::getenv("OPENPAK_TRACE_FW") != nullptr;
    bool traced = false;
    u64 traced_program = 0;
    if (trace_fw) {
        const auto* const owner = ctx.GetThread().GetOwnerProcess();
        if (owner != nullptr && (owner->GetProgramId() & ~u64{0xFFFF}) == 0x0100000000000000ULL) {
            traced = true;
            traced_program = owner->GetProgramId();
        }
    }
    // OPENPAK_TRACE_WORDS=<command>: the request and response words of that command, for a reply
    // whose shape a firmware program checks.
    static const long trace_words_cmd = [] {
        const char* v = std::getenv("OPENPAK_TRACE_WORDS");
        return v != nullptr ? std::atol(v) : -1L;
    }();
    const bool dump_words = traced && static_cast<long>(ctx.GetCommand()) == trace_words_cmd;
    if (dump_words) {
        const u32* const w = ctx.CommandBuffer();
        LOG_INFO(Service, "fw-words request {} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x}",
                 ctx.GetCommand(), w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10],
                 w[11], w[12], w[13], w[14], w[15]);
    }
    if (traced) {
        LOG_INFO(Service, "fw-ipc> {:016X} [{}] {} {}({})", traced_program,
                 Core::GetThreadName(&ctx.GetThread()).value_or(std::string{}), GetServiceName(),
                 ctx.GetCommand(), info->name);
    }
    handler_invoker(this, info->handler_callback, ctx);
    if (traced) {
        // The response words as the handler left them: "SFCO" is followed by the result code.
        const u32* const w = ctx.CommandBuffer();
        u32 result = 0xFFFFFFFF;
        for (int i = 0; i < 12; ++i) {
            if (w[i] == 0x4f434653u) {
                result = w[i + 2];
                break;
            }
        }
        if (dump_words) {
            LOG_INFO(Service, "fw-words response {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x}",
                     w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11]);
        }
        LOG_INFO(Service, "fw-ipc {:016X} [{}] {} {}({}) -> {:#x}", traced_program,
                 Core::GetThreadName(&ctx.GetThread()).value_or(std::string{}), GetServiceName(), ctx.GetCommand(), info->name, result);
    }

    if (is_i_storage && is_cmd_read) {
        const auto* const process = ctx.GetThread().GetOwnerProcess();
        if (process != nullptr && system.IsNVDECActiveForProcess(process->GetId())) {
            std::this_thread::sleep_for(std::chrono::microseconds{600});
        }
    }
}

void ServiceFrameworkBase::InvokeRequestTipc(HLERequestContext& ctx) {
    auto it = handlers_tipc.find(ctx.GetCommand());
    FunctionInfoBase const* info = it == handlers_tipc.end() ? nullptr : &it->second;
    if (info == nullptr || info->handler_callback == nullptr)
        return ReportUnimplementedFunction(ctx, info);

    LOG_TRACE(Service, "{}", MakeFunctionString(info->name, GetServiceName(), ctx.CommandBuffer()));
    handler_invoker(this, info->handler_callback, ctx);
}

Result ServiceFrameworkBase::HandleSyncRequest(Kernel::KServerSession& session,
                                               HLERequestContext& ctx) {
    const auto guard = LockService();

    Result result = ResultSuccess;

    switch (ctx.GetCommandType()) {
    case IPC::CommandType::Close:
    case IPC::CommandType::TIPC_Close: {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
        result = IPC::ResultSessionClosed;
        break;
    }
    case IPC::CommandType::ControlWithContext:
    case IPC::CommandType::Control: {
        system.ServiceManager().InvokeControlRequest(ctx);
        break;
    }
    case IPC::CommandType::RequestWithContext:
    case IPC::CommandType::Request: {
        InvokeRequest(ctx);
        break;
    }
    default:
        if (ctx.IsTipc()) {
            InvokeRequestTipc(ctx);
            break;
        }

        UNIMPLEMENTED_MSG("command_type={}", ctx.GetCommandType());
        break;
    }

    // If emulation was shutdown, we are closing service threads, do not write the response back to
    // memory that may be shutting down as well.
    if (system.IsPoweredOn()) {
        ctx.WriteToOutgoingCommandBuffer();
    }

    return result;
}

} // namespace Service

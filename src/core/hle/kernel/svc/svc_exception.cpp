// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>
#include "core/arm/arm_interface.h"
#include "core/arm/debug.h"
#include "core/core.h"
#include "core/debugger/debugger.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/svc.h"
#include "core/hle/kernel/svc_types.h"
#include "core/memory.h"
#include "core/reporter.h"

namespace Kernel::Svc {

namespace {

std::string_view BreakReasonName(BreakReason reason) {
    switch (reason) {
    case BreakReason::Panic:
        return "Panic";
    case BreakReason::Assert:
        return "Assert";
    case BreakReason::User:
        return "User";
    case BreakReason::PreLoadDll:
        return "PreLoadDll";
    case BreakReason::PostLoadDll:
        return "PostLoadDll";
    case BreakReason::PreUnloadDll:
        return "PreUnloadDll";
    case BreakReason::PostUnloadDll:
        return "PostUnloadDll";
    case BreakReason::CppException:
        return "CppException";
    default:
        return "Unknown";
    }
}

// Logs who called svcBreak and what the info buffer holds. Behaviour follows the Ryujinx Harbor
// (MIT-licensed Ryujinx base): Syscall.Break.
void LogBreakCaller(Core::System& system, BreakReason reason, bool notification_only, u64 info1,
                    u64 info2) {
    auto& kernel = system.Kernel();
    auto& process = GetCurrentProcess(kernel);
    auto& thread = GetCurrentThread(kernel);
    const auto thread_name = Core::GetThreadName(&thread);

    Svc::ThreadContext ctx{};
    if (auto* const interface = process.GetArmInterface(kernel.CurrentPhysicalCoreIndex())) {
        interface->GetContext(ctx);
    }

    // Break reasons that pass a buffer carry a Result (4 bytes), a value (8 bytes) or text.
    constexpr u64 MaxBufferSize = 0x400;
    constexpr u64 MaxHexBytes = 0x100;
    std::string buffer;
    auto& memory = process.GetMemory();
    if (info1 != 0 && info2 != 0 && info2 <= MaxBufferSize &&
        memory.IsValidVirtualAddressRange(info1, info2)) {
        if (info2 == sizeof(u32)) {
            const u32 value = memory.Read32(info1);
            buffer = fmt::format(" result={:#010x} (2{:03}-{:04})", value, value & 0x1FF,
                                 (value >> 9) & 0x1FFF);
        } else if (info2 == sizeof(u64)) {
            buffer = fmt::format(" value={:#018x}", memory.Read64(info1));
        } else {
            std::vector<u8> bytes(std::min(info2, MaxHexBytes));
            memory.ReadBlock(info1, bytes.data(), bytes.size());
            buffer = " buffer=";
            for (const u8 byte : bytes) {
                buffer += fmt::format("{:02x}", byte);
            }
        }
    }

    LOG_INFO(Debug_Emulated,
             "svcBreak: reason={:#x} ({}){} info1={:#x} info2={:#x}{} process pid={} "
             "program={:016x} name=\"{}\" thread id={} name=\"{}\" pc={:#x} lr={:#x}",
             static_cast<u32>(reason), BreakReasonName(reason & ~BreakReason::NotificationOnlyFlag),
             notification_only ? " notification-only" : "", info1, info2, buffer,
             process.GetProcessId(), process.GetProgramId(), process.GetName(),
             thread.GetThreadId(), thread_name.value_or(std::string{}), ctx.pc, ctx.lr);
}

} // namespace

/// Break program execution
void Break(Core::System& system, BreakReason reason, u64 info1, u64 info2) {
    LogBreakCaller(system, reason, True(reason & BreakReason::NotificationOnlyFlag), info1, info2);

    BreakReason break_reason =
        reason & static_cast<BreakReason>(~BreakReason::NotificationOnlyFlag);
    bool notification_only = True(reason & BreakReason::NotificationOnlyFlag);

    bool has_dumped_buffer{};
    std::vector<u8> debug_buffer;

    const auto handle_debug_buffer = [&](u64 addr, u64 sz) {
        if (sz == 0 || addr == 0 || has_dumped_buffer) {
            return;
        }

        auto& memory = GetCurrentMemory(system.Kernel());

        // This typically is an error code so we're going to assume this is the case
        if (sz == sizeof(u32)) {
            LOG_CRITICAL(Debug_Emulated, "debug_buffer_err_code={:X}", memory.Read32(addr));
        } else {
            // We don't know what's in here so we'll hexdump it
            debug_buffer.resize(sz);
            memory.ReadBlock(addr, debug_buffer.data(), sz);
            std::string hexdump;
            for (std::size_t i = 0; i < debug_buffer.size(); i++) {
                hexdump += fmt::format("{:02X} ", debug_buffer[i]);
                if ((i + 1) % 32 == 0) {
                    hexdump += '\n';
                }
            }
            LOG_CRITICAL(Debug_Emulated, "debug_buffer=\n{}", hexdump);
        }
        has_dumped_buffer = true;
    };
    switch (break_reason) {
    case BreakReason::Panic:
        LOG_CRITICAL(Debug_Emulated, "Userspace PANIC! info1={:#016x}, info2={:#016x}", info1,
                     info2);
        handle_debug_buffer(info1, info2);
        break;
    case BreakReason::Assert:
        LOG_CRITICAL(Debug_Emulated, "Userspace Assertion failed! info1={:#016x}, info2={:#016x}",
                     info1, info2);
        handle_debug_buffer(info1, info2);
        break;
    case BreakReason::User:
        LOG_WARNING(Debug_Emulated, "Userspace Break! {:#016x} with size {:#016x}", info1, info2);
        handle_debug_buffer(info1, info2);
        break;
    case BreakReason::PreLoadDll:
        LOG_INFO(Debug_Emulated,
                 "Userspace Attempting to load an NRO at {:#016x} with size {:#016x}", info1,
                 info2);
        break;
    case BreakReason::PostLoadDll:
        LOG_INFO(Debug_Emulated, "Userspace Loaded an NRO at {:#016x} with size {:#016x}", info1,
                 info2);
        break;
    case BreakReason::PreUnloadDll:
        LOG_INFO(Debug_Emulated,
                 "Userspace Attempting to unload an NRO at {:#016x} with size {:#016x}", info1,
                 info2);
        break;
    case BreakReason::PostUnloadDll:
        LOG_INFO(Debug_Emulated, "Userspace Unloaded an NRO at {:#016x} with size {:#016x}",
                 info1, info2);
        break;
    case BreakReason::CppException:
        LOG_CRITICAL(Debug_Emulated, "Signalling debugger. Uncaught C++ exception encountered.");
        break;
    default:
        LOG_WARNING(
            Debug_Emulated,
            "Signalling debugger, Unknown break reason {:#x}, info1={:#016x}, info2={:#016x}",
            reason, info1, info2);
        handle_debug_buffer(info1, info2);
        break;
    }

    system.GetReporter().SaveSvcBreakReport(
        static_cast<u32>(reason), notification_only, info1, info2,
        has_dumped_buffer ? std::make_optional(debug_buffer) : std::nullopt);

    if (!notification_only) {
        LOG_CRITICAL(
            Debug_Emulated,
            "Emulated program broke execution! reason={:#016x}, info1={:#016x}, info2={:#016x}",
            reason, info1, info2);

        handle_debug_buffer(info1, info2);

        system.CurrentPhysicalCore().LogBacktrace(system.Kernel());
    }

    const bool should_break = !notification_only;
    if (system.DebuggerEnabled() && should_break) {
        auto* thread = system.Kernel().GetCurrentEmuThread();
        system.GetDebugger().NotifyThreadStopped(thread);
        thread->RequestSuspend(system.Kernel(), Kernel::SuspendType::Debug);
    }
}

void ReturnFromException(Core::System& system, Result result) {
    UNIMPLEMENTED();
}

void Break64(Core::System& system, BreakReason break_reason, uint64_t arg, uint64_t size) {
    Break(system, break_reason, arg, size);
}

void Break64From32(Core::System& system, BreakReason break_reason, uint32_t arg, uint32_t size) {
    Break(system, break_reason, arg, size);
}

void ReturnFromException64(Core::System& system, Result result) {
    ReturnFromException(system, result);
}

void ReturnFromException64From32(Core::System& system, Result result) {
    ReturnFromException(system, result);
}

} // namespace Kernel::Svc

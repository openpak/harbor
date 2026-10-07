// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base): Horizon.LogGuestThreads.

#include <algorithm>
#include <string>

#include <fmt/format.h>

#include "common/logging.h"
#include "core/arm/arm_interface.h"
#include "core/arm/debug.h"
#include "core/core.h"
#include "core/debug/thread_dump.h"
#include "core/hardware_properties.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_scheduler.h"
#include "core/hle/kernel/k_scoped_lock.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"

namespace Core::Debug {

namespace {

constexpr std::size_t MaxBacktraceFrames = 16;

// Same lookup as Core::GetThreadName, but every guest pointer is checked first: a thread that is
// starting or exiting can have a garbage TLS block, and this runs on a host thread.
std::string GuestThreadName(Kernel::KProcess& process, const Kernel::KThread& thread) {
    auto& memory = process.GetMemory();
    const bool is_64 = process.Is64Bit();
    const u64 tls = GetInteger(thread.GetTlsAddress());
    const u64 ptr_size = is_64 ? 8 : 4;
    const u64 type_offset = is_64 ? 0x1f8 : 0x1fc;
    if (tls == 0 || !memory.IsValidVirtualAddressRange(tls + type_offset, ptr_size)) {
        return {};
    }
    const u64 thread_type = is_64 ? memory.Read64(tls + type_offset) : memory.Read32(tls + type_offset);
    const u64 version_offset = is_64 ? 0x46 : 0x26;
    const u64 name_offset = is_64 ? 0x1a0 : 0xe4;
    if (thread_type == 0 || !memory.IsValidVirtualAddressRange(thread_type, name_offset + 8 + ptr_size)) {
        return {};
    }
    const u64 name_slot = thread_type + name_offset + (memory.Read16(thread_type + version_offset) == 1 ? 0 : ptr_size);
    const u64 name_ptr = is_64 ? memory.Read64(name_slot) : memory.Read32(name_slot);
    if (name_ptr == 0 || !memory.IsValidVirtualAddressRange(name_ptr, 1)) {
        return {};
    }
    std::string name = memory.ReadCString(name_ptr, 64);
    for (auto& c : name) {
        if (c < 0x20 || c > 0x7e) {
            c = '?';
        }
    }
    return name;
}

std::string DescribeThread(Kernel::KernelCore& kernel, Kernel::KProcess& process,
                           Kernel::KThread& thread) {
    const s32 core = thread.GetActiveCore();

    // A thread that is executing right now has no saved context; read the live one from its core.
    Kernel::Svc::ThreadContext ctx = thread.GetContext();
    bool live = false;
    if (core >= 0 && core < static_cast<s32>(Core::Hardware::NUM_CPU_CORES) &&
        kernel.Scheduler(static_cast<std::size_t>(core)).GetSchedulerCurrentThread() == &thread) {
        if (auto* const interface = process.GetArmInterface(static_cast<std::size_t>(core))) {
            interface->GetContext(ctx);
            live = true;
        }
    }

    std::string out = fmt::format(
        "thread dump:   thread {} \"{}\" prio {} (base {}) core {} state {}{} pc={:#x} lr={:#x} "
        "sp={:#x}{}",
        thread.GetThreadId(), GuestThreadName(process, thread), thread.GetPriority(),
        thread.GetBasePriority(), core, Core::GetThreadState(&thread),
        thread.IsSuspended() ? " suspended" : "", ctx.pc, ctx.lr, ctx.sp,
        live ? " (live context)" : "");

    const auto backtrace = Core::GetBacktraceFromContext(&process, ctx);
    const std::size_t count = std::min(backtrace.size(), MaxBacktraceFrames);
    for (std::size_t i = 0; i < count; ++i) {
        const auto& entry = backtrace[i];
        out += fmt::format("\nthread dump:       #{} {:#x} {}+{:#x} {}", i, entry.original_address,
                           entry.module, entry.offset, entry.name);
    }
    return out;
}

} // namespace

void LogGuestThreads(System& system) {
    auto& kernel = system.Kernel();

    // The kernel's light locks need a registered host thread to identify the owner.
    thread_local bool registered = false;
    if (!registered && kernel.GetCurrentEmuThread() == nullptr) {
        kernel.RegisterHostThread();
        registered = true;
    }

    LOG_INFO(Core, "thread dump: begin");
    auto processes = kernel.GetProcessList();
    for (auto& process : processes) {
        Kernel::KScopedLightLock ll{process->GetListLock()};

        const auto& threads = process->GetThreadList();
        LOG_INFO(Core, "thread dump: process pid {} program {:016x} name \"{}\" threads {}",
                 process->GetProcessId(), process->GetProgramId(), process->GetName(),
                 threads.size());
        for (auto& thread : process->GetThreadList()) {
            LOG_INFO(Core, "{}", DescribeThread(kernel, *process, thread));
        }
    }
    LOG_INFO(Core, "thread dump: end");
}

} // namespace Core::Debug

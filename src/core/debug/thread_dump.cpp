// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base): Horizon.LogGuestThreads.

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "common/logging.h"
#include "core/arm/arm_interface.h"
#include "core/arm/debug.h"
#include "core/core.h"
#include "core/debug/thread_dump.h"
#include "core/hardware_properties.h"
#include "core/hle/kernel/k_page_table_base.h"
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

// [OpenPak] OPENPAK_DUMP_SCAN=<program id hex>:<u64 hex>[:<bytes before>:<bytes after>][;...]:
// with the thread dump, find every aligned copy of the value in the program's readable memory
// and log the non-zero words around it. For looking at what a firmware module parsed, without a
// debugger.
void ScanGuestMemory(Kernel::KProcess& process, const char* spec) {
    u64 program_id = 0, needle = 0, before = 0x40, after = 0x40;
    char* end = nullptr;
    program_id = std::strtoull(spec, &end, 16);
    if (*end != ':' || program_id != process.GetProgramId()) {
        return;
    }
    needle = std::strtoull(end + 1, &end, 16);
    if (*end == ':') {
        before = std::strtoull(end + 1, &end, 16);
    }
    if (*end == ':') {
        after = std::strtoull(end + 1, &end, 16);
    }
    auto& page_table = process.GetPageTable();
    auto& memory = process.GetMemory();
    u64 addr = GetInteger(page_table.GetAddressSpaceStart());
    const u64 space_end = addr + page_table.GetAddressSpaceSize();
    std::vector<u8> buf;
    u64 scanned = 0;
    while (addr < space_end) {
        Kernel::KMemoryInfo info{};
        Kernel::Svc::PageInfo page_info{};
        if (page_table.QueryInfo(&info, &page_info, addr).IsError()) {
            break;
        }
        const auto svc = info.GetSvcMemoryInfo();
        const u64 next = svc.base_address + svc.size;
        if (svc.state != Kernel::Svc::MemoryState::Free &&
            svc.state != Kernel::Svc::MemoryState::Inaccessible &&
            True(svc.permission & Kernel::Svc::MemoryPermission::Read)) {
            scanned += svc.size;
            buf.resize(svc.size);
            if (memory.ReadBlock(svc.base_address, buf.data(), svc.size)) {
                for (u64 off = 0; off + 8 <= svc.size; off += 8) {
                    u64 value;
                    std::memcpy(&value, buf.data() + off, 8);
                    if (value != needle) {
                        continue;
                    }
                    const u64 hit = svc.base_address + off;
                    const u64 lo = hit - std::min(before, off);
                    const u64 hi = std::min(hit + after, svc.base_address + svc.size);
                    std::string words;
                    for (u64 a = lo; a + 8 <= hi; a += 8) {
                        u64 w;
                        std::memcpy(&w, buf.data() + (a - svc.base_address), 8);
                        if (w != 0) {
                            words += fmt::format(" {:+#x}={:#x}", static_cast<s64>(a - hit), w);
                        }
                    }
                    LOG_INFO(Core, "memory scan: {:016x} {:#x} hit at {:#x}:{}", program_id, needle,
                             hit, words);
                }
            }
        }
        if (next <= addr) {
            break;
        }
        addr = next;
    }
    LOG_INFO(Core, "memory scan: {:016x} scanned {:#x} readable bytes for {:#x}", program_id,
             scanned, needle);
}

void ScanGuestMemory(Kernel::KProcess& process) {
    const char* specs = std::getenv("OPENPAK_DUMP_SCAN");
    if (specs == nullptr) {
        return;
    }
    std::string all{specs};
    std::size_t start = 0;
    while (start < all.size()) {
        const auto stop = all.find(';', start);
        const std::string one = all.substr(start, stop == std::string::npos ? std::string::npos : stop - start);
        ScanGuestMemory(process, one.c_str());
        if (stop == std::string::npos) {
            break;
        }
        start = stop + 1;
    }
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
        ScanGuestMemory(*process);
    }
    LOG_INFO(Core, "thread dump: end");
}

} // namespace Core::Debug

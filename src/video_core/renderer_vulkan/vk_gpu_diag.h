// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Harbor diagnostics for GPU hangs: an in-memory ring of the last draws/dispatches (dumped when the
// device is lost) and a periodic resource line (memory heaps, process memory, submit counter).
// Nothing is logged per draw: pushing an entry is a few stores and one atomic increment.

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "common/common_types.h"
#include <fmt/format.h>

#include "common/logging.h"

namespace Vulkan::GpuDiag {

enum class Kind : u8 { Draw, DrawIndexed, DrawIndirect, Compute };

struct Entry {
    // Graphics: VertexA, VertexB, TessControl, TessEval, Geometry, Fragment. Compute: [0] only.
    std::array<u64, 6> hashes{};
    u64 ms{};
    u32 topology{};
    Kind kind{};
};

constexpr u32 RING_SIZE = 64;

inline std::array<Entry, RING_SIZE> ring{};
inline std::atomic<u32> head{0};
inline std::atomic<u64> total_draws{0};
inline std::atomic<u64> total_dispatches{0};

[[nodiscard]] inline u64 NowMs() {
    using namespace std::chrono;
    return static_cast<u64>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

inline void Push(Kind kind, const std::array<u64, 6>& hashes, u32 topology) {
    const u32 slot = head.fetch_add(1, std::memory_order_relaxed) % RING_SIZE;
    Entry& e = ring[slot];
    e.hashes = hashes;
    e.ms = NowMs();
    e.topology = topology;
    e.kind = kind;
    (kind == Kind::Compute ? total_dispatches : total_draws).fetch_add(1, std::memory_order_relaxed);
}

/// Logs the ring oldest to newest. Called from the device-loss handler.
inline void DumpRing() {
    static constexpr std::array<const char*, 6> STAGES{"VA", "VB", "TC", "TE", "GS", "FS"};
    static constexpr std::array<const char*, 4> KINDS{"draw", "draw-idx", "draw-ind", "compute"};
    const u32 end = head.load(std::memory_order_relaxed);
    const u32 count = end < RING_SIZE ? end : RING_SIZE;
    const u64 now = NowMs();
    LOG_CRITICAL(Render_Vulkan,
                 "harbor-diag: last {} draws/dispatches before the loss (oldest first; {} draws, {} "
                 "dispatches in total)",
                 count, total_draws.load(), total_dispatches.load());
    for (u32 i = 0; i < count; ++i) {
        const Entry& e = ring[(end - count + i) % RING_SIZE];
        std::string line;
        if (e.kind == Kind::Compute) {
            line = fmt::format(" CS={:016x}", e.hashes[0]);
        } else {
            for (size_t s = 0; s < e.hashes.size(); ++s) {
                if (e.hashes[s] != 0) {
                    line += fmt::format(" {}={:016x}", STAGES[s], e.hashes[s]);
                }
            }
        }
        LOG_CRITICAL(Render_Vulkan, "harbor-diag: -{:>5} ms {:<8} topo={}{}", now - e.ms,
                     KINDS[static_cast<size_t>(e.kind)], e.topology, line);
    }
}

/// "Name: value kB" from a /proc file, 0 when absent.
[[nodiscard]] inline u64 ProcKb(const char* path, const char* key) {
    std::ifstream file{path};
    std::string name;
    u64 value{};
    std::string rest;
    const size_t key_len = std::strlen(key);
    while (file >> name) {
        if (name.size() == key_len + 1 && name.compare(0, key_len, key) == 0) {
            file >> value;
            return value;
        }
        std::getline(file, rest);
    }
    return 0;
}

/// Process and system memory in MiB, plus thread and file descriptor counts.
[[nodiscard]] inline std::string ProcessSummary() {
    const auto mib = [](u64 kb) { return kb / 1024; };
    u64 fds = 0;
    std::error_code ec;
    for (std::filesystem::directory_iterator it{"/proc/self/fd", ec}, end; !ec && it != end;
         it.increment(ec)) {
        ++fds;
    }
    return fmt::format("rss={}M swap={}M threads={} fds={} | sys free={}M avail={}M cached={}M",
                       mib(ProcKb("/proc/self/status", "VmRSS")),
                       mib(ProcKb("/proc/self/status", "VmSwap")),
                       ProcKb("/proc/self/status", "Threads"), fds,
                       mib(ProcKb("/proc/meminfo", "MemFree")),
                       mib(ProcKb("/proc/meminfo", "MemAvailable")),
                       mib(ProcKb("/proc/meminfo", "Cached")));
}

} // namespace Vulkan::GpuDiag

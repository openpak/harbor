// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <variant>
#include <vector>

#include "common/common_types.h"
#include "common/socket_types.h"
#include "core/hle/service/service.h"
#include "core/hle/service/sockets/sockets.h"
#include "network/network.h"

namespace Core {
class System;
}

namespace Kernel {
class KernelCore;
}

namespace Service {
class ServerManager;
}

namespace Network {
class SocketBase;
class Socket;
} // namespace Network

namespace Service::Sockets {

/// Running totals of the guest's UDP traffic through bsd, kept even after the 600-line datagram
/// trace has gone quiet. last_ms is steady-clock milliseconds of the last datagram (0 = none).
struct DatagramStats {
    u64 sent;
    u64 received;
    u64 failed;
    s64 last_ms;
};
DatagramStats GetDatagramStats();


/// [Harbor] A guest eventfd (bsd EventFd, 7.0.0+): a 64-bit counter that never blocks. Other
/// services may hold one (BSD_USA::GetEventFd) and Write() to it to wake a guest poller.
/// Written from: Linux eventfd(2); emulator-parity-ledger §2 Sockets ("Event fd is a counter,
/// never blocking"); Ryujinx master EventFileDescriptor.cs (counter, semaphore mode).
class BsdEventFd {
public:
    static constexpr u64 MaxValue = 0xffff'ffff'ffff'fffeULL;

    explicit BsdEventFd(u64 initval, bool semaphore_);

    /// write(2): adds `value` to the counter. INVAL for 0xffffffffffffffff, AGAIN if the counter
    /// would pass MaxValue. Parked bsd polls see it within one deferral heartbeat (1 ms).
    Errno Write(u64 value);

    /// read(2): the counter (then zero), or 1 (then counter - 1) in semaphore mode; AGAIN when
    /// the counter is zero.
    std::pair<u64, Errno> Read();

    bool IsReadable() const;
    bool IsWritable() const;
    u64 Value() const;

private:
    mutable std::mutex mutex;
    u64 counter;
    const bool semaphore;
};

/// [Harbor] One open file description: a host socket or an eventfd. Every descriptor duplicated
/// from it shares it (and its O_NONBLOCK); the host socket closes with the last descriptor.
/// Written from: emulator-parity-ledger §2 Sockets ("A duplicate shares its host socket, which
/// closes with the last descriptor"), POSIX dup(2)/fcntl(2).
struct OpenFile {
    ~OpenFile();

    std::shared_ptr<Network::SocketBase> socket; ///< null for an eventfd
    std::shared_ptr<BsdEventFd> event;              ///< null for a socket
    std::atomic<bool> non_blocking{false};
    bool is_connection_based = false;
    bool is_datagram = false;
    bool is_inet6 = false; ///< guest AF_INET6, served over host IPv4 with v4-mapped addresses
    bool is_proxy = false; ///< LDN room proxy socket: no host descriptor, no parking

    std::mutex state_mutex;
    std::optional<Network::SockAddrIn> pending_peer; ///< target of a connect still in progress
    u32 rcv_timeout_ms = 0;
};

/// [Harbor] One client process's descriptors. Horizon gives every process its own table; qlaunch,
/// the overlay, applets and the game each open sockets at once.
/// Written from: Ryujinx master BsdContext.cs (one context per process id, lowest free fd).
class DescriptorTable {
public:
    static constexpr s32 MaxFd = 128;

    explicit DescriptorTable(u64 process_id_) : process_id{process_id_} {}

    s32 Add(std::shared_ptr<OpenFile> file);
    std::shared_ptr<OpenFile> Get(s32 fd) const;
    bool Remove(s32 fd);
    s32 Duplicate(s32 fd);
    std::vector<std::shared_ptr<OpenFile>> All() const;
    size_t Count() const;

    u64 ProcessId() const {
        return process_id;
    }

private:
    mutable std::mutex mutex;
    std::array<std::shared_ptr<OpenFile>, MaxFd> files{};
    const u64 process_id;
};

/// [Harbor] Bookkeeping of a request parked on host state. Lives while the request is deferred.
struct ParkSlot {
    /// The request this slot belongs to: a context address can be reused by a later request.
    u64 thread_id = 0;
    u32 command = 0;
    bool parked = false;
    bool connecting = false;
    bool deadline_overridden = false; ///< RecvMMsg's own timeval bounds the park
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
};

/// [Harbor] State shared by bsd:u, bsd:s and bsd:a: the per-process tables and parked requests.
class BsdState {
public:
    explicit BsdState(Kernel::KernelCore* kernel_) : kernel{kernel_} {}

    /// The table of `process_id`, created on first use. Tables of processes that have exited
    /// are dropped (closing their sockets) on the way.
    std::shared_ptr<DescriptorTable> GetTable(u64 process_id);
    std::shared_ptr<DescriptorTable> FindTable(u64 process_id) const;
    std::vector<std::shared_ptr<DescriptorTable>> Tables() const;

    ParkSlot* FindSlot(const void* key);
    void StoreSlot(const void* key, const ParkSlot& slot);
    void ReleaseSlot(const void* key);

    /// Server managers that hold parked bsd requests; WakeParked re-runs them at once.
    void AddManager(ServerManager* manager);
    void RemoveManagers();
    void WakeParked();

private:
    void SweepExitedLocked(u64 keep);

    Kernel::KernelCore* const kernel; ///< null in the self-check: nothing is swept
    mutable std::mutex mutex;
    std::map<u64, std::shared_ptr<DescriptorTable>> tables;
    std::chrono::steady_clock::time_point last_sweep{};

    std::mutex slot_mutex;
    std::unordered_map<const void*, ParkSlot> slots;

    std::mutex manager_mutex;
    std::vector<ServerManager*> managers;
};

/// [Harbor] Adds 1 to the counter of eventfd `fd` in the descriptor table of `process_id` (the
/// caller's KProcess::GetProcessId()), and returns false when that descriptor is not an eventfd
/// of that process. Thread-safe; parked bsd polls and selects are re-run at once. For the fw23
/// resolver (sfdnsres 100/102/104).
bool SignalGuestEventFd(u64 process_id, s32 fd);

/// [Harbor] SignalGuestEventFd as a callback.
std::function<bool(u64 process_id, s32 fd)> GuestEventFdSignalHook();

/// [Harbor] Hands GuestEventFdSignalHook to the resolver (SFDNSRES::SetEventFdSignal) when this
/// build's sfdnsres has that hook; returns whether it did. Called by every bsd service at start.
bool InstallResolverEventFdSignal();

class BSD_USA final : public ServiceFramework<BSD_USA> {
public:
    explicit BSD_USA(Core::System& system_, const char* name, bool is_user);
    ~BSD_USA() override;

    // These are called from SSL; the first two are also called from this class for the
    // corresponding IPC methods. On the real device, the SSL service makes IPC calls to this
    // service. [Harbor] Descriptors are per process: the overloads without a process id use the
    // process whose request the calling host thread is serving (GetCurrentRequestProcessId).
    std::variant<s32, Errno> DuplicateSocketImpl(s32 fd);
    Errno CloseImpl(s32 fd);
    std::optional<std::shared_ptr<Network::SocketBase>> GetSocket(s32 fd);

    std::variant<s32, Errno> DuplicateSocketImpl(u64 process_id, s32 fd);
    Errno CloseImpl(u64 process_id, s32 fd);
    std::optional<std::shared_ptr<Network::SocketBase>> GetSocket(u64 process_id, s32 fd);

    /// [Harbor] The eventfd behind guest descriptor `fd` of `process_id`, or null. For services
    /// that are handed an eventfd by the guest and signal it (the fw23 resolver).
    std::shared_ptr<BsdEventFd> GetEventFd(u64 process_id, s32 fd);

    /// [Harbor] Descriptor-table and parking self-check (HARBOR_BSD_SELFCHECK=1). Logs one
    /// "bsd self-check:" line per case and a summary; returns true when every case passed.
    static bool RunSelfCheck();

private:
    void RegisterClient(HLERequestContext& ctx);
    void StartMonitoring(HLERequestContext& ctx);
    void Open(HLERequestContext& ctx);
    void Ioctl(HLERequestContext& ctx);
    void Socket(HLERequestContext& ctx);
    void SocketExempt(HLERequestContext& ctx);
    void Select(HLERequestContext& ctx);
    void Poll(HLERequestContext& ctx);
    void Sysctl(HLERequestContext& ctx);
    void Accept(HLERequestContext& ctx);
    void Bind(HLERequestContext& ctx);
    void Connect(HLERequestContext& ctx);
    void GetPeerName(HLERequestContext& ctx);
    void GetSockName(HLERequestContext& ctx);
    void GetSockOpt(HLERequestContext& ctx);
    void Listen(HLERequestContext& ctx);
    void Fcntl(HLERequestContext& ctx);
    void SetSockOpt(HLERequestContext& ctx);
    void Shutdown(HLERequestContext& ctx);
    void ShutdownAllSockets(HLERequestContext& ctx);
    void Recv(HLERequestContext& ctx);
    void RecvFrom(HLERequestContext& ctx);
    void Send(HLERequestContext& ctx);
    void SendTo(HLERequestContext& ctx);
    void Write(HLERequestContext& ctx);
    void Read(HLERequestContext& ctx);
    void Close(HLERequestContext& ctx);
    void DuplicateSocket(HLERequestContext& ctx);
    void RecvMMsg(HLERequestContext& ctx);
    void SendMMsg(HLERequestContext& ctx);
    void EventFd(HLERequestContext& ctx);

    /// The calling process's table.
    std::shared_ptr<DescriptorTable> TableFor(HLERequestContext& ctx);

    /// Runs `step` with the request's park slot. A step that answers nullopt parks the request
    /// (deferred, no response written); otherwise `respond` writes the response.
    template <typename Step, typename Respond>
    void RunParkable(HLERequestContext& ctx, Step&& step, Respond&& respond);

    void BuildErrnoResponse(HLERequestContext& ctx, Errno bsd_errno) const noexcept;

    /// Callback to parse and handle a received wifi packet.
    void OnProxyPacketReceived(const Network::ProxyPacket& packet);

    // Callback identifier for the OnProxyPacketReceived event.
    Network::RoomMember::CallbackHandle<Network::ProxyPacket> proxy_packet_received;

    std::shared_ptr<BsdState> state;

protected:
    std::unique_lock<std::mutex> LockService() noexcept override;
    bool is_user = false;
};

class BSDCFG final : public ServiceFramework<BSDCFG> {
public:
    explicit BSDCFG(Core::System& system_, const char *name);
    ~BSDCFG() override;
};

class BSD_NU final : public ServiceFramework<BSD_NU> {
public:
    explicit BSD_NU(Core::System& system_);
    ~BSD_NU() override;
};

} // namespace Service::Sockets

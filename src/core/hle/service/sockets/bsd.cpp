// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// [Harbor] The bsd service keeps one descriptor table per client process and parks a blocking
// poll, select, recv, accept or connect (Eden's deferral, re-run by the server's heartbeat) instead
// of holding an IPC thread inside a host wait. Written from: Ryujinx master (OpenPak's own rewrite,
// 154744710) IClient.cs, ManagedSocketPollManager.cs, BsdContext.cs, InterfaceList.cs and the
// ServerBase.cs deferral (cd462aa69); its OPENPAK.md; docs/switch/emulator-parity-ledger.md §2
// Sockets and §3; POSIX/Linux/FreeBSD man pages. Each function names the item it follows.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/ranges.h>

#include "common/logging.h"
#include "common/socket_types.h"
#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/sockets/bsd.h"
#include "core/hle/service/sockets/sfdnsres.h"
#include "core/hle/service/sockets/sockets_translate.h"
#include "core/internal_network/network.h"
#include "core/internal_network/network_interface.h"
#include "core/internal_network/socket_proxy.h"
#include "core/internal_network/sockets.h"
#include "network/network.h"
#include <common/settings.h>

namespace Service::Sockets {

namespace {

using Clock = std::chrono::steady_clock;
using Result2 = std::pair<s32, Errno>;
/// nullopt: park the request; otherwise the (ret, errno) to answer.
using Outcome = std::optional<Result2>;

constexpr auto DeferralHeartbeat = std::chrono::milliseconds{1};
constexpr auto SelectEventFdParkWindow = std::chrono::milliseconds{100};
constexpr s32 UdpReceiveBufferSize = 1 << 20;
constexpr u32 GuestSockNonBlock = 0x20000000;
constexpr u32 GuestSockCloExec = 0x10000000;
constexpr u16 KnownPollEvents = 0x1ff;
constexpr u32 EventFdSemaphore = 1u << 0;

template <typename T>
T GetValue(std::span<const u8> buffer) {
    T t{};
    std::memcpy(&t, buffer.data(), (std::min)(sizeof(T), buffer.size()));
    return t;
}

template <typename T>
std::vector<u8> AsBytes(const T& t) {
    std::vector<u8> out(sizeof(T));
    std::memcpy(out.data(), &t, sizeof(T));
    return out;
}

constexpr u16 Swap16(u16 v) {
    return static_cast<u16>(v >> 8 | v << 8);
}

/// Writes `data` into output buffer `index`, cut to the buffer's size. Returns the bytes written.
size_t WriteOut(HLERequestContext& ctx, std::span<const u8> data, size_t index = 0) {
    if (data.empty() || !ctx.CanWriteBuffer(index)) {
        return 0;
    }
    const size_t size = (std::min)(data.size(), ctx.GetWriteBufferSize(index));
    if (size == 0) {
        return 0;
    }
    return ctx.WriteBuffer(data.data(), size, index);
}

std::span<const u8> InBuffer(HLERequestContext& ctx, size_t index = 0) {
    if (!ctx.CanReadBuffer(index)) {
        return {};
    }
    return ctx.ReadBuffer(index);
}

size_t OutSize(HLERequestContext& ctx, size_t index = 0) {
    return ctx.CanWriteBuffer(index) ? ctx.GetWriteBufferSize(index) : 0;
}

void PushRetErrno(HLERequestContext& ctx, Result2 r) {
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(r.first);
    rb.PushEnum(r.second);
}

void PushRetErrnoLen(HLERequestContext& ctx, Result2 r, u32 length) {
    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.Push<s32>(r.first);
    rb.PushEnum(r.second);
    rb.Push<u32>(length);
}

Result2 ErrnoResult(Errno e) {
    return {e == Errno::SUCCESS ? 0 : -1, e};
}

// Written from: FreeBSD netinet/in.h and netinet6/in6.h layouts; RFC 4291 §2.5.5.2 (v4-mapped);
// emulator-parity-ledger §2 Sockets ("IPv4-mapped addresses on an INET6 socket").
// A sockaddr_in6 is accepted for ::ffff:a.b.c.d, :: (any) and ::1 (loopback); anything else is
// an IPv6 destination the console's IPv4-only stack cannot reach.
std::optional<Network::SockAddrIn> ParseGuestAddr(std::span<const u8> addr) {
    if (addr.size() >= 24 && addr[1] == static_cast<u8>(Domain::INET6)) {
        const auto in6 = GetValue<SockAddrIn6>(addr);
        const auto& a = in6.addr;
        Network::SockAddrIn out{
            .family = Network::Domain::INET,
            .ip = {},
            .portno = Swap16(in6.portno),
        };
        const bool zero10 = std::all_of(a.begin(), a.begin() + 10, [](u8 b) { return b == 0; });
        const bool zero15 = zero10 && a[10] == 0 && a[11] == 0 && a[12] == 0 && a[13] == 0 &&
                            a[14] == 0;
        if (zero10 && a[10] == 0xff && a[11] == 0xff) {
            out.ip = {a[12], a[13], a[14], a[15]};
        } else if (zero15 && a[15] == 0) {
            out.ip = {0, 0, 0, 0};
        } else if (zero15 && a[15] == 1) {
            out.ip = {127, 0, 0, 1};
        } else {
            return std::nullopt;
        }
        return out;
    }
    if (addr.size() < 8) {
        return std::nullopt;
    }
    Network::SockAddrIn out = Translate(GetValue<SockAddrIn>(addr));
    out.family = Network::Domain::INET;
    return out;
}

// Written from: emulator-parity-ledger §2 Sockets ("Address length written back is 16";
// "IPv4-mapped addresses on an INET6 socket").
std::vector<u8> GuestAddrBytes(const OpenFile& file, const Network::SockAddrIn& addr) {
    if (file.is_inet6) {
        SockAddrIn6 out{};
        out.len = sizeof(SockAddrIn6);
        out.family = static_cast<u8>(Domain::INET6);
        out.portno = Swap16(addr.portno);
        if (addr.ip != Network::IPv4Address{0, 0, 0, 0}) {
            out.addr[10] = 0xff;
            out.addr[11] = 0xff;
            std::copy(addr.ip.begin(), addr.ip.end(), out.addr.begin() + 12);
        }
        return AsBytes(out);
    }
    const SockAddrIn guest = Translate(addr);
    std::vector<u8> out(SockAddrInSize);
    std::memcpy(out.data(), &guest, SockAddrInSize);
    return out;
}

/// The host's revents for one socket, probed without waiting.
Network::PollEvents HostRevents(const OpenFile& file, Network::PollEvents want) {
    std::vector<Network::PollFD> pfd{{file.socket.get(), want, Network::PollEvents{}}};
    Network::Poll(pfd, 0);
    return pfd[0].revents;
}

bool HostReady(const OpenFile& file, Network::PollEvents want) {
    return static_cast<u16>(HostRevents(file, want)) != 0;
}

// Written from: Ryujinx master ServerBase.cs/IClient.cs (a parked request carries its deadline;
// the re-check answers once ready or overdue) and poll(2) ("a timeout answers 0").
Outcome ParkUntil(ParkSlot& slot, std::optional<std::chrono::milliseconds> timeout,
                  Result2 on_timeout) {
    const auto now = Clock::now();
    if (!slot.parked) {
        slot.parked = true;
        slot.deadline = timeout ? now + *timeout : Clock::time_point::max();
    }
    if (now >= slot.deadline) {
        return on_timeout;
    }
    return std::nullopt;
}

std::optional<std::chrono::milliseconds> MsOrInfinite(u32 ms) {
    if (ms == 0) {
        return std::nullopt;
    }
    return std::chrono::milliseconds{ms};
}

// Written from: emulator-parity-ledger §2 Sockets (type 0 is DGRAM; SOCK_NONBLOCK applied at
// creation; 1 MiB UDP receive buffer; bsd:u refuses RAW and SEQPACKET with ENOENT, ICMP is a ping
// socket) and Eden's own SocketImpl (room proxy, airplane mode, failed Initialize is -1).
Result2 OpSocket(DescriptorTable& table, bool is_user, u32 domain_raw, u32 type_raw,
                 u32 protocol_raw) {
    const bool non_blocking = (type_raw & GuestSockNonBlock) != 0;
    u32 base_type = type_raw & ~(GuestSockNonBlock | GuestSockCloExec);
    if (base_type == 0) {
        // Ryujinx master IClient.cs: socket + bind + recvfrom with no accept is a UDP server.
        LOG_INFO(Service, "socket type 0 -> DGRAM (base type unset by the guest)");
        base_type = static_cast<u32>(Type::DGRAM);
    }
    const Domain domain{domain_raw};
    const Type type{base_type};
    Protocol protocol{protocol_raw};

    if (domain != Domain::INET && domain != Domain::INET6) {
        return {-1, Errno::AFNOSUPPORT};
    }
    if (type != Type::STREAM && type != Type::DGRAM && type != Type::RAW &&
        type != Type::SEQPACKET) {
        return {-1, Errno::INVAL};
    }
    if (is_user && (type == Type::SEQPACKET || type == Type::RAW) &&
        !(type == Type::RAW && domain == Domain::INET && protocol == Protocol::ICMP)) {
        return {-1, Errno::NOENT};
    }
    if (protocol == Protocol::IP) {
        if (type == Type::STREAM) {
            protocol = Protocol::TCP;
        } else if (type == Type::DGRAM) {
            protocol = Protocol::UDP;
        }
    }

    auto file = std::make_shared<OpenFile>();
    auto room_member = Network::GetRoomMember().lock();
    if (room_member && room_member->IsConnected()) {
        file->socket = std::make_shared<Network::ProxySocket>();
        file->is_proxy = true;
    } else {
        file->socket = std::make_shared<Network::Socket>();
    }

    const Network::Errno init_errno =
        file->socket->Initialize(Translate(domain), Translate(type), Translate(protocol));
    if (init_errno != Network::Errno::SUCCESS) {
        LOG_ERROR(Service, "Socket creation failed: domain={} type={} protocol={}", domain_raw,
                  base_type, protocol_raw);
        file->socket.reset();
        return {-1, Translate(init_errno)};
    }
    file->is_connection_based = type == Type::STREAM || type == Type::SEQPACKET;
    file->is_datagram = type == Type::DGRAM || type == Type::RAW;
    file->is_inet6 = domain == Domain::INET6;

    if (Settings::values.airplane_mode.GetValue() && file->is_connection_based) {
        LOG_ERROR(Service, "Airplane mode is enabled, cannot create socket");
        return {-1, Errno::NOTCONN};
    }
    if (non_blocking) {
        file->socket->SetNonBlock(true);
        file->non_blocking = true;
    }
    if (type == Type::DGRAM && !file->is_proxy) {
        file->socket->SetIntOption(Network::SockOpt::RcvBuf, UdpReceiveBufferSize);
    }

    const s32 fd = table.Add(file);
    if (fd < 0) {
        LOG_ERROR(Service, "No more file descriptors available (process {})", table.ProcessId());
        return {-1, Errno::MFILE};
    }
    LOG_INFO(Service, "New socket fd={} (process {}, {}{}{})", fd, table.ProcessId(),
             file->is_connection_based ? "stream" : "datagram", file->is_inet6 ? ", inet6" : "",
             non_blocking ? ", nonblocking" : "");
    return {fd, Errno::SUCCESS};
}

std::shared_ptr<OpenFile> SocketFile(DescriptorTable& table, s32 fd, Errno& err) {
    auto file = table.Get(fd);
    if (!file) {
        err = Errno::BADF;
        return nullptr;
    }
    if (!file->socket) {
        err = Errno::NOTSOCK;
        return nullptr;
    }
    err = Errno::SUCCESS;
    return file;
}

// Written from: emulator-parity-ledger §2 Sockets ("UDP bind sets SO_REUSEADDR") and Ryujinx
// master ManagedSocket.Bind (titles rebind a fixed source port across retries).
Errno OpBind(DescriptorTable& table, s32 fd, std::span<const u8> addr) {
    Errno err;
    auto file = SocketFile(table, fd, err);
    if (!file) {
        return err;
    }
    const auto host_addr = ParseGuestAddr(addr);
    if (!host_addr) {
        return file->is_inet6 ? Errno::ADDRNOTAVAIL : Errno::INVAL;
    }
    if (file->is_datagram) {
        file->socket->SetReuseAddr(true);
    }
    return Translate(file->socket->Bind(*host_addr));
}

// Written from: Ryujinx master ManagedSocket.Connect (the pending peer of a connect in flight is
// what getpeername reports) and POSIX connect(2) (a blocking connect waits for the handshake; the
// wait is a park here, so the IPC thread is free meanwhile).
Outcome OpConnect(DescriptorTable& table, s32 fd, std::span<const u8> addr, ParkSlot& slot) {
    Errno err;
    auto file = SocketFile(table, fd, err);
    if (!file) {
        return ErrnoResult(err);
    }
    auto& socket = *file->socket;
    if (!slot.parked) {
        const auto host_addr = ParseGuestAddr(addr);
        if (!host_addr) {
            return ErrnoResult(file->is_inet6 ? Errno::NETUNREACH : Errno::INVAL);
        }
        const bool blocking = !file->non_blocking && !file->is_proxy;
        if (blocking) {
            socket.SetNonBlock(true);
        }
        Errno e = Translate(socket.Connect(*host_addr));
        if (e == Errno::INPROGRESS) {
            std::scoped_lock lk{file->state_mutex};
            file->pending_peer = *host_addr;
        }
        if (!blocking || e != Errno::INPROGRESS) {
            if (blocking) {
                socket.SetNonBlock(false);
            }
            if (e == Errno::ISCONN) {
                e = Errno::SUCCESS;
            }
            return ErrnoResult(e);
        }
        slot.connecting = true;
        return ParkUntil(slot, std::nullopt, {});
    }
    if (!HostReady(*file, Network::PollEvents::Out)) {
        return std::nullopt;
    }
    const auto [pending, getsockopt_err] = socket.GetPendingError();
    socket.SetNonBlock(false);
    const Errno e =
        getsockopt_err != Network::Errno::SUCCESS ? Translate(getsockopt_err) : Translate(pending);
    if (e != Errno::SUCCESS) {
        std::scoped_lock lk{file->state_mutex};
        file->pending_peer.reset();
    }
    return ErrnoResult(e);
}

// Written from: Ryujinx master IClient.DeferBlockingReceive (a blocking recv with nothing to
// read parks instead of holding the service thread; readable or closed answers in this pass),
// extended to streams because Eden's deferral has no pointer-buffer hazard; SO_RCVTIMEO bounds
// the park (EAGAIN on expiry, socket(7)).
Outcome OpRecv(DescriptorTable& table, s32 fd, u32 flags, std::vector<u8>& message,
               std::vector<u8>* addr_out, ParkSlot& slot) {
    Errno err;
    auto file = SocketFile(table, fd, err);
    if (!file) {
        return ErrnoResult(err);
    }
    auto& socket = *file->socket;
    const bool dontwait = (flags & Network::FLAG_MSG_DONTWAIT) != 0;
    const bool may_park = !file->is_proxy && !file->non_blocking && !dontwait;

    Network::SockAddrIn from{};
    const bool want_from = addr_out != nullptr && !file->is_connection_based;
    const auto do_recv = [&](u32 recv_flags) {
        if (want_from) {
            return Translate(socket.RecvFrom(static_cast<int>(recv_flags), message, &from));
        }
        return Translate(socket.Recv(static_cast<int>(recv_flags), message));
    };

    const auto timeout = [&] {
        std::scoped_lock lk{file->state_mutex};
        return MsOrInfinite(file->rcv_timeout_ms);
    };

    Result2 r;
    if (may_park) {
        if (!HostReady(*file, Network::PollEvents::In)) {
            return ParkUntil(slot, timeout(), {-1, Errno::AGAIN});
        }
        // Readable, but another thread may take the datagram first: never block on the host.
        r = do_recv(flags | Network::FLAG_MSG_DONTWAIT);
        if (r.second == Errno::AGAIN) {
            return ParkUntil(slot, timeout(), {-1, Errno::AGAIN});
        }
    } else if (file->is_proxy && dontwait) {
        socket.SetNonBlock(true);
        r = do_recv(flags & ~Network::FLAG_MSG_DONTWAIT);
        socket.SetNonBlock(file->non_blocking);
    } else if (file->is_proxy) {
        r = do_recv(flags);
    } else {
        r = do_recv(flags);
    }

    if (addr_out != nullptr) {
        if (!want_from || r.first < 0) {
            addr_out->clear();
        } else {
            *addr_out = GuestAddrBytes(*file, from);
        }
    }
    return r;
}

// Written from: emulator-parity-ledger §2 Sockets ("recv/send on an unconnected UDP socket reach
// the host") and POSIX sendto(2).
Result2 OpSend(DescriptorTable& table, s32 fd, u32 flags, std::span<const u8> message,
               std::span<const u8> addr) {
    Errno err;
    auto file = SocketFile(table, fd, err);
    if (!file) {
        return {-1, err};
    }
    if (file->is_proxy) {
        flags &= ~Network::FLAG_MSG_DONTWAIT;
    }
    if (addr.empty()) {
        if (file->is_connection_based) {
            return Translate(file->socket->Send(message, static_cast<int>(flags)));
        }
        return Translate(file->socket->SendTo(flags, message, nullptr));
    }
    const auto host_addr = ParseGuestAddr(addr);
    if (!host_addr) {
        return {-1, file->is_inet6 ? Errno::NETUNREACH : Errno::INVAL};
    }
    return Translate(file->socket->SendTo(flags, message, &*host_addr));
}

// Written from: POSIX accept(2), FreeBSD accept(2) (the new socket inherits O_NONBLOCK), and the
// recv parking above.
Outcome OpAccept(DescriptorTable& table, s32 fd, std::vector<u8>& addr_out, ParkSlot& slot) {
    Errno err;
    auto file = SocketFile(table, fd, err);
    if (!file) {
        return ErrnoResult(err);
    }
    const bool may_park = !file->is_proxy && !file->non_blocking;
    if (may_park && !HostReady(*file, Network::PollEvents::In)) {
        return ParkUntil(slot, std::nullopt, {});
    }
    auto [result, accept_err] = file->socket->Accept();
    if (accept_err != Network::Errno::SUCCESS) {
        if (accept_err == Network::Errno::AGAIN && may_park) {
            return ParkUntil(slot, std::nullopt, {});
        }
        return ErrnoResult(Translate(accept_err));
    }
    auto accepted = std::make_shared<OpenFile>();
    accepted->socket = std::shared_ptr<Network::SocketBase>(std::move(result.socket));
    accepted->is_connection_based = file->is_connection_based;
    accepted->is_inet6 = file->is_inet6;
    accepted->is_proxy = file->is_proxy;
    if (file->non_blocking) {
        accepted->socket->SetNonBlock(true);
        accepted->non_blocking = true;
    }
    const s32 new_fd = table.Add(accepted);
    if (new_fd < 0) {
        return Result2{-1, Errno::MFILE};
    }
    addr_out = GuestAddrBytes(*accepted, result.sockaddr_in);
    return Result2{new_fd, Errno::SUCCESS};
}

// Written from: Ryujinx master IClient.Poll + ManagedSocketPollManager.cs and the commit message
// of 154744710 (a zero-timeout probe that finds nothing answers 0 ready, never -1/ETIMEDOUT; a
// datagram socket never reports POLLHUP; a readable eventfd is drained as it is reported);
// emulator-parity-ledger §2/§3 (zero event mask watches
// errors only; a dead fd is POLLNVAL on its entry and the rest is evaluated; poll with no fds
// sleeps; a deferred poll that runs out of time answers 0 / success); POSIX poll(2) (a negative fd
// is ignored).
Outcome OpPoll(DescriptorTable& table, std::span<PollFD> fds, s32 timeout, ParkSlot& slot) {
    if (timeout < -1) {
        return Result2{-1, Errno::INVAL};
    }
    std::vector<Network::PollFD> host;
    std::vector<size_t> host_index;
    std::vector<std::shared_ptr<OpenFile>> keep;

    for (size_t i = 0; i < fds.size(); ++i) {
        PollFD& entry = fds[i];
        entry.revents = PollEvents{};
        if (entry.fd < 0) {
            continue;
        }
        const auto want = static_cast<PollEvents>(static_cast<u16>(entry.events) & KnownPollEvents);
        auto file = table.Get(entry.fd);
        if (!file) {
            entry.revents = PollEvents::Nval;
            continue;
        }
        if (file->event) {
            PollEvents r{};
            if (True(want & (PollEvents::In | PollEvents::RdNorm)) && file->event->IsReadable()) {
                r |= want & (PollEvents::In | PollEvents::RdNorm);
                // Drained as it is reported (Ryujinx master IClient.Poll: gRPC writes its wakeup
                // fd and never reads it, so a level-triggered fd would spin its poller).
                file->event->Read();
            }
            if (True(want & PollEvents::Out) && file->event->IsWritable()) {
                r |= PollEvents::Out;
            }
            entry.revents = r;
            continue;
        }
        host.push_back(Network::PollFD{file->socket.get(), Translate(want), Network::PollEvents{}});
        host_index.push_back(i);
        keep.push_back(std::move(file));
    }

    if (!host.empty()) {
        const auto [res, poll_err] = Network::Poll(host, 0);
        if (res < 0) {
            return Result2{-1, Translate(poll_err)};
        }
        for (size_t k = 0; k < host.size(); ++k) {
            PollEvents r = Translate(host[k].revents);
            if (keep[k]->is_datagram) {
                r &= ~PollEvents::Hup;
            }
            fds[host_index[k]].revents = r;
        }
    }

    s32 ready = 0;
    for (const PollFD& entry : fds) {
        if (static_cast<u16>(entry.revents) != 0) {
            ++ready;
        }
    }
    if (ready > 0 || timeout == 0) {
        return Result2{ready, Errno::SUCCESS};
    }
    return ParkUntil(slot,
                     timeout < 0 ? std::nullopt
                                 : std::optional{std::chrono::milliseconds{timeout}},
                     {0, Errno::SUCCESS});
}

// Written from: Ryujinx master IClient.Select (a select parks; with an event fd in the set the
// park is at most 100 ms, and empty sets are left behind on a timeout), emulator-parity-ledger §2
// ("Select is implemented and parks for at most 100 ms on an event fd") and POSIX select(2)
// (EBADF for a closed descriptor; read-ready includes EOF and errors).
Outcome OpSelect(DescriptorTable& table, u32 nfds, const std::array<std::span<const u8>, 3>& in,
                 std::array<std::vector<u8>, 3>& out,
                 std::optional<std::chrono::milliseconds> timeout, ParkSlot& slot) {
    for (auto& o : out) {
        std::fill(o.begin(), o.end(), u8{0});
    }
    const s32 limit = static_cast<s32>((std::min)(nfds, static_cast<u32>(DescriptorTable::MaxFd)));
    const auto is_set = [](std::span<const u8> set, s32 fd) {
        const size_t byte = static_cast<size_t>(fd) / 8;
        return byte < set.size() && (set[byte] & (1u << (fd % 8))) != 0;
    };
    const auto mark = [](std::vector<u8>& set, s32 fd) {
        const size_t byte = static_cast<size_t>(fd) / 8;
        if (byte < set.size()) {
            set[byte] = static_cast<u8>(set[byte] | (1u << (fd % 8)));
        }
    };

    struct Entry {
        s32 fd;
        std::array<bool, 3> want;
        std::shared_ptr<OpenFile> file;
    };
    std::vector<Entry> entries;
    bool has_event_fd = false;
    for (s32 fd = 0; fd < limit; ++fd) {
        const std::array<bool, 3> want{is_set(in[0], fd), is_set(in[1], fd), is_set(in[2], fd)};
        if (!want[0] && !want[1] && !want[2]) {
            continue;
        }
        auto file = table.Get(fd);
        if (!file) {
            return Result2{-1, Errno::BADF};
        }
        has_event_fd |= file->event != nullptr;
        entries.push_back(Entry{fd, want, std::move(file)});
    }

    std::vector<Network::PollFD> host;
    std::vector<size_t> host_entry;
    for (size_t i = 0; i < entries.size(); ++i) {
        const Entry& e = entries[i];
        if (e.file->event) {
            continue;
        }
        Network::PollEvents events{};
        if (e.want[0]) {
            events |= Network::PollEvents::In;
        }
        if (e.want[1]) {
            events |= Network::PollEvents::Out;
        }
        if (e.want[2]) {
            events |= Network::PollEvents::Pri;
        }
        host.push_back(Network::PollFD{e.file->socket.get(), events, Network::PollEvents{}});
        host_entry.push_back(i);
    }
    std::vector<Network::PollEvents> revents(entries.size());
    if (!host.empty()) {
        const auto [res, poll_err] = Network::Poll(host, 0);
        if (res < 0) {
            return Result2{-1, Translate(poll_err)};
        }
        for (size_t k = 0; k < host.size(); ++k) {
            revents[host_entry[k]] = host[k].revents;
        }
    }

    s32 ready = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
        const Entry& e = entries[i];
        bool r = false;
        bool w = false;
        bool x = false;
        if (e.file->event) {
            r = e.want[0] && e.file->event->IsReadable();
            w = e.want[1] && e.file->event->IsWritable();
            if (r) {
                // Ryujinx master IClient.Select: the reported wakeup is drained (edge-triggered),
                // or a fd left readable masks the socket half of the next round.
                e.file->event->Read();
            }
        } else {
            const auto rev = revents[i];
            const bool failed = True(rev & (Network::PollEvents::Err | Network::PollEvents::Hup));
            r = e.want[0] && (True(rev & Network::PollEvents::In) || failed);
            w = e.want[1] && (True(rev & Network::PollEvents::Out) || failed);
            x = e.want[2] && True(rev & Network::PollEvents::Pri);
        }
        if (r) {
            mark(out[0], e.fd);
            ++ready;
        }
        if (w) {
            mark(out[1], e.fd);
            ++ready;
        }
        if (x) {
            mark(out[2], e.fd);
            ++ready;
        }
    }
    if (ready > 0 || (timeout && timeout->count() == 0)) {
        return Result2{ready, Errno::SUCCESS};
    }
    if (has_event_fd) {
        timeout = timeout ? (std::min)(*timeout, std::chrono::milliseconds{SelectEventFdParkWindow})
                          : std::chrono::milliseconds{SelectEventFdParkWindow};
    }
    return ParkUntil(slot, timeout, {0, Errno::SUCCESS});
}

// Written from: Ryujinx master IClient.EventFd (flags, padding, initval) and emulator-parity-ledger
// §2 Sockets ("Event fd is a counter, never blocking, argument order flags/padding/initval").
Result2 OpEventFd(DescriptorTable& table, u64 initval, u32 flags) {
    auto file = std::make_shared<OpenFile>();
    file->event = std::make_shared<BsdEventFd>(initval, (flags & EventFdSemaphore) != 0);
    file->non_blocking = true;
    const s32 fd = table.Add(file);
    if (fd < 0) {
        return {-1, Errno::MFILE};
    }
    LOG_INFO(Service, "EventFd fd={} (process {}, flags={:#x}, initval={})", fd,
             table.ProcessId(), flags, initval);
    return {fd, Errno::SUCCESS};
}

// Written from: FreeBSD sys/socket.h, netinet/in.h, netinet/tcp.h option numbers.
std::optional<Network::SockOpt> MapIntOption(u32 level, u32 name) {
    using Network::SockOpt;
    switch (static_cast<SocketLevel>(level)) {
    case SocketLevel::SOCKET:
        switch (static_cast<OptName>(name)) {
        case OptName::REUSEADDR:
            return SockOpt::ReuseAddr;
        case OptName::REUSEPORT:
            return SockOpt::ReusePort;
        case OptName::KEEPALIVE:
            return SockOpt::KeepAlive;
        case OptName::BROADCAST:
            return SockOpt::Broadcast;
        case OptName::OOBINLINE:
            return SockOpt::OobInline;
        case OptName::SNDBUF:
            return SockOpt::SndBuf;
        case OptName::RCVBUF:
            return SockOpt::RcvBuf;
        case OptName::SNDLOWAT:
            return SockOpt::SndLowat;
        case OptName::RCVLOWAT:
            return SockOpt::RcvLowat;
        case OptName::TYPE:
            return SockOpt::Type;
        default:
            return std::nullopt;
        }
    case SocketLevel::TCP:
        switch (name) {
        case 0x1:
            return SockOpt::TcpNoDelay;
        case 0x2:
            return SockOpt::TcpMaxSeg;
        case 0x100:
            return SockOpt::TcpKeepIdle;
        case 0x200:
            return SockOpt::TcpKeepIntvl;
        case 0x400:
            return SockOpt::TcpKeepCnt;
        default:
            return std::nullopt;
        }
    case SocketLevel::IP:
        switch (name) {
        case 3:
            return SockOpt::IpTos;
        case 4:
            return SockOpt::IpTtl;
        case 10:
            return SockOpt::IpMulticastTtl;
        case 11:
            return SockOpt::IpMulticastLoop;
        default:
            return std::nullopt;
        }
    default:
        return std::nullopt;
    }
}

constexpr u32 IpMulticastIf = 9;
constexpr u32 IpAddMembership = 12;
constexpr u32 IpDropMembership = 13;

s32 IntOptionValue(std::span<const u8> v) {
    if (v.size() >= sizeof(s32)) {
        return GetValue<s32>(v);
    }
    return v.empty() ? 0 : v[0];
}

/// A guest timeval (two s64 on the Switch; two s32 if the guest passed 8 bytes) in milliseconds.
u32 TimevalMs(std::span<const u8> v) {
    s64 sec = 0;
    s64 usec = 0;
    if (v.size() >= 16) {
        sec = GetValue<s64>(v);
        usec = GetValue<s64>(v.subspan(8));
    } else if (v.size() >= 8) {
        sec = GetValue<s32>(v);
        usec = GetValue<s32>(v.subspan(4));
    }
    if (sec < 0 || usec < 0) {
        return 0;
    }
    const s64 ms = sec * 1000 + usec / 1000;
    return static_cast<u32>((std::min)(ms, static_cast<s64>(0xffffffffu)));
}

// Written from: emulator-parity-ledger §2 Sockets ("setsockopt never asserts or refuses a value";
// "Timeouts are timevals off Windows") and Ryujinx master ManagedSocket.SetSocketOption
// (SO_NOSIGPIPE needs nothing: sends never raise SIGPIPE here).
Errno OpSetSockOpt(OpenFile& file, u32 level, u32 name, std::span<const u8> v) {
    auto& socket = *file.socket;
    Network::Errno host = Network::Errno::SUCCESS;
    if (level == static_cast<u32>(SocketLevel::SOCKET)) {
        switch (static_cast<OptName>(name)) {
        case OptName::NOSIGPIPE:
            return Errno::SUCCESS;
        case OptName::LINGER: {
            const auto linger = GetValue<Linger>(v);
            host = socket.SetLinger(linger.onoff != 0, linger.linger);
            break;
        }
        case OptName::SNDTIMEO:
            host = socket.SetSndTimeo(TimevalMs(v));
            break;
        case OptName::RCVTIMEO: {
            const u32 ms = TimevalMs(v);
            {
                std::scoped_lock lk{file.state_mutex};
                file.rcv_timeout_ms = ms;
            }
            host = socket.SetRcvTimeo(ms);
            break;
        }
        default:
            break;
        }
        const auto opt_name = static_cast<OptName>(name);
        if (opt_name == OptName::LINGER || opt_name == OptName::SNDTIMEO ||
            opt_name == OptName::RCVTIMEO) {
            if (host != Network::Errno::SUCCESS) {
                LOG_DEBUG(Service, "setsockopt {:#x} not taken by the host: {}", name, host);
            }
            return Errno::SUCCESS;
        }
    }
    if (level == static_cast<u32>(SocketLevel::IP)) {
        if (name == IpAddMembership || name == IpDropMembership) {
            Network::IPv4Address group{};
            Network::IPv4Address iface{};
            if (v.size() >= 8) {
                std::memcpy(group.data(), v.data(), 4);
                std::memcpy(iface.data(), v.data() + 4, 4);
            }
            host = socket.SetMulticastMembership(name == IpAddMembership, group, iface);
            if (host != Network::Errno::SUCCESS) {
                LOG_DEBUG(Service, "multicast membership not taken by the host: {}", host);
            }
            return Errno::SUCCESS;
        }
        if (name == IpMulticastIf) {
            Network::IPv4Address iface{};
            if (v.size() >= 4) {
                std::memcpy(iface.data(), v.data(), 4);
            }
            socket.SetMulticastInterface(iface);
            return Errno::SUCCESS;
        }
    }
    if (level == static_cast<u32>(SocketLevel::IPV6)) {
        // IPV6_V6ONLY and friends: an INET6 socket here is IPv4 underneath, v4-mapped only.
        LOG_DEBUG(Service, "setsockopt IPV6 {:#x} accepted without effect", name);
        return Errno::SUCCESS;
    }
    if (const auto opt = MapIntOption(level, name)) {
        host = socket.SetIntOption(*opt, IntOptionValue(v));
        if (host != Network::Errno::SUCCESS) {
            LOG_DEBUG(Service, "setsockopt level={:#x} optname={:#x} not taken by the host: {}",
                      level, name, host);
        }
        return Errno::SUCCESS;
    }
    LOG_WARNING(Service, "setsockopt level={:#x} optname={:#x} ({} bytes) accepted without effect",
                level, name, v.size());
    return Errno::SUCCESS;
}

// Written from: emulator-parity-ledger §2 Sockets ("getters read the host") and FreeBSD
// getsockopt(2) value layouts.
std::pair<std::vector<u8>, Errno> OpGetSockOpt(OpenFile& file, u32 level, u32 name) {
    auto& socket = *file.socket;
    if (level == static_cast<u32>(SocketLevel::SOCKET)) {
        switch (static_cast<OptName>(name)) {
        case OptName::ERROR_: {
            const auto [pending, getsockopt_err] = socket.GetPendingError();
            if (getsockopt_err != Network::Errno::SUCCESS) {
                return {{}, Translate(getsockopt_err)};
            }
            const Errno value = Translate(pending);
            if (value != Errno::SUCCESS) {
                std::scoped_lock lk{file.state_mutex};
                file.pending_peer.reset();
            }
            return {AsBytes(static_cast<u32>(value)), Errno::SUCCESS};
        }
        case OptName::LINGER: {
            const auto [value, e] = socket.GetLinger();
            return {AsBytes(Linger{value.first ? 1u : 0u, value.second}), Translate(e)};
        }
        case OptName::SNDTIMEO:
        case OptName::RCVTIMEO: {
            const bool send = name == static_cast<u32>(OptName::SNDTIMEO);
            const auto [ms, e] = socket.GetTimeoutOption(send);
            const std::array<s64, 2> tv{ms / 1000, (ms % 1000) * 1000};
            return {AsBytes(tv), Translate(e)};
        }
        case OptName::NOSIGPIPE:
            return {AsBytes(s32{0}), Errno::SUCCESS};
        case OptName::TYPE: {
            const auto [value, e] = socket.GetIntOption(Network::SockOpt::Type);
            return {AsBytes(static_cast<u32>(Translate(static_cast<Network::Type>(value)))),
                    Translate(e)};
        }
        default:
            break;
        }
    }
    if (const auto opt = MapIntOption(level, name)) {
        const auto [value, e] = socket.GetIntOption(*opt);
        return {AsBytes(value), Translate(e)};
    }
    LOG_WARNING(Service, "getsockopt level={:#x} optname={:#x} answered 0", level, name);
    return {AsBytes(s32{0}), Errno::SUCCESS};
}

// Written from: Ryujinx master InterfaceList.cs, the answer to sysctl {CTL_NET, PF_ROUTE, 0, 0,
// NET_RT_IFLISTL, 0} (FreeBSD getifaddrs): one RTM_IFINFO message with the link-level address,
// one RTM_NEWADDR with netmask, address and broadcast. Every message carries its own length and
// data offset; every sockaddr is a multiple of 8 bytes.
namespace InterfaceList {
constexpr s32 CtlNet = 4;
constexpr s32 PfRoute = 17;
constexpr s32 NetRtIfListL = 5;
constexpr u8 RtmVersion = 5;
constexpr u8 RtmIfInfo = 0x0e;
constexpr u8 RtmNewAddr = 0x0c;
constexpr s32 RtaNetmask = 0x04;
constexpr s32 RtaIfp = 0x10;
constexpr s32 RtaIfa = 0x20;
constexpr s32 RtaBrd = 0x80;
constexpr s32 IffUpBroadcastRunningMulticast = 0x1 | 0x2 | 0x40 | 0x8000;
constexpr size_t HeaderSize = 24;
constexpr size_t IfDataSize = 152;
constexpr size_t SockaddrDlSize = 24;
constexpr size_t SockaddrInSize = 16;
constexpr u16 InterfaceIndex = 1;

bool Matches(std::span<const s32> mib) {
    return mib.size() >= 5 && mib[0] == CtlNet && mib[1] == PfRoute && mib[4] == NetRtIfListL;
}

void Put16(std::span<u8> m, size_t at, u16 v) {
    std::memcpy(m.data() + at, &v, sizeof(v));
}
void Put32(std::span<u8> m, size_t at, u32 v) {
    std::memcpy(m.data() + at, &v, sizeof(v));
}

void WriteHeader(std::span<u8> m, u8 type, s32 addrs, s32 flags) {
    Put16(m, 0, static_cast<u16>(m.size()));
    m[2] = RtmVersion;
    m[3] = type;
    Put32(m, 4, static_cast<u32>(addrs));
    Put32(m, 8, static_cast<u32>(flags));
    Put16(m, 12, InterfaceIndex);
    Put16(m, 16, static_cast<u16>(HeaderSize + IfDataSize));
    Put16(m, 18, static_cast<u16>(HeaderSize));
}

void WriteIfData(std::span<u8> d) {
    d[0] = 6; // IFT_ETHER
    d[2] = 6; // addrlen
    d[3] = 14; // hdrlen
    d[4] = 2; // LINK_STATE_UP
    Put16(d, 6, static_cast<u16>(IfDataSize));
    Put32(d, 8, 1500); // mtu
}

void WriteSockaddrIn(std::span<u8> s, const Network::IPv4Address& address) {
    s[0] = static_cast<u8>(SockaddrInSize);
    s[1] = 2; // AF_INET
    std::copy(address.begin(), address.end(), s.begin() + 4);
}

std::vector<u8> Build(const Network::IPv4Address& ip, const Network::IPv4Address& mask) {
    Network::IPv4Address broadcast{};
    for (size_t i = 0; i < 4; ++i) {
        broadcast[i] = static_cast<u8>(ip[i] | static_cast<u8>(~mask[i]));
    }
    const size_t info_length = HeaderSize + IfDataSize + SockaddrDlSize;
    const size_t addr_length = HeaderSize + IfDataSize + 3 * SockaddrInSize;
    std::vector<u8> buffer(info_length + addr_length);

    std::span<u8> info{buffer.data(), info_length};
    WriteHeader(info, RtmIfInfo, RtaIfp, IffUpBroadcastRunningMulticast);
    WriteIfData(info.subspan(HeaderSize));
    std::span<u8> dl = info.subspan(HeaderSize + IfDataSize);
    dl[0] = static_cast<u8>(SockaddrDlSize);
    dl[1] = 18; // AF_LINK
    Put16(dl, 2, InterfaceIndex);
    dl[4] = 6; // IFT_ETHER
    dl[5] = 4; // sdl_nlen: "eth0"
    dl[6] = 6; // sdl_alen
    std::memcpy(dl.data() + 8, "eth0", 4);
    const std::array<u8, 6> mac{0x02, 0x00, ip[0], ip[1], ip[2], ip[3]}; // locally administered
    std::copy(mac.begin(), mac.end(), dl.begin() + 12);

    std::span<u8> addr{buffer.data() + info_length, addr_length};
    WriteHeader(addr, RtmNewAddr, RtaNetmask | RtaIfa | RtaBrd, 0);
    WriteIfData(addr.subspan(HeaderSize));
    std::span<u8> sa = addr.subspan(HeaderSize + IfDataSize);
    WriteSockaddrIn(sa, mask);
    WriteSockaddrIn(sa.subspan(SockaddrInSize), ip);
    WriteSockaddrIn(sa.subspan(2 * SockaddrInSize), broadcast);
    return buffer;
}
} // namespace InterfaceList

// Written from: Ryujinx master BsdMMsgHdr.cs / BsdMsgHdr.cs (wire format: one header byte, then
// per message u32 name length + name, u32 iov count + (u64 length + data) per iov, u32 control
// length + control, u32 flags, u32 length).
struct MsgHdr {
    std::vector<u8> name;
    std::vector<std::vector<u8>> iov;
    std::vector<u8> control;
    u32 flags = 0;
    u32 length = 0;
};

std::optional<std::vector<MsgHdr>> ParseMMsg(std::span<const u8> raw, u32 vlen) {
    if (raw.empty()) {
        return std::nullopt;
    }
    raw = raw.subspan(1); // header byte, ignored
    const auto take = [&](size_t n) -> std::optional<std::span<const u8>> {
        if (raw.size() < n) {
            return std::nullopt;
        }
        const auto part = raw.first(n);
        raw = raw.subspan(n);
        return part;
    };
    const auto take32 = [&]() -> std::optional<u32> {
        const auto part = take(4);
        return part ? std::optional{GetValue<u32>(*part)} : std::nullopt;
    };
    std::vector<MsgHdr> out(vlen);
    for (auto& m : out) {
        const auto name_len = take32();
        const auto name = name_len ? take(*name_len) : std::nullopt;
        const auto iov_count = name ? take32() : std::nullopt;
        if (!iov_count || *iov_count > 1024) {
            return std::nullopt;
        }
        m.name.assign(name->begin(), name->end());
        for (u32 i = 0; i < *iov_count; ++i) {
            const auto len_part = take(8);
            if (!len_part) {
                return std::nullopt;
            }
            const auto data = take(static_cast<size_t>(GetValue<u64>(*len_part)));
            if (!data) {
                return std::nullopt;
            }
            m.iov.emplace_back(data->begin(), data->end());
        }
        const auto control_len = take32();
        const auto control = control_len ? take(*control_len) : std::nullopt;
        const auto flags = control ? take32() : std::nullopt;
        const auto length = flags ? take32() : std::nullopt;
        if (!length) {
            return std::nullopt;
        }
        m.control.assign(control->begin(), control->end());
        m.flags = *flags;
        m.length = *length;
    }
    return out;
}

std::vector<u8> SerializeMMsg(const std::vector<MsgHdr>& messages) {
    std::vector<u8> out{0x8};
    const auto put = [&](const void* p, size_t n) {
        const auto* b = static_cast<const u8*>(p);
        out.insert(out.end(), b, b + n);
    };
    for (const auto& m : messages) {
        const u32 name_len = static_cast<u32>(m.name.size());
        put(&name_len, 4);
        put(m.name.data(), m.name.size());
        const u32 iov_count = static_cast<u32>(m.iov.size());
        put(&iov_count, 4);
        for (const auto& v : m.iov) {
            const u64 len = v.size();
            put(&len, 8);
            put(v.data(), v.size());
        }
        const u32 control_len = static_cast<u32>(m.control.size());
        put(&control_len, 4);
        put(m.control.data(), m.control.size());
        put(&m.flags, 4);
        put(&m.length, 4);
    }
    return out;
}

/// The type-0x6 buffer of RecvMMsg/SendMMsg (B descriptor).
std::vector<u8> ReadBufferB(HLERequestContext& ctx) {
    if (ctx.BufferDescriptorB().empty()) {
        return {};
    }
    const auto& desc = ctx.BufferDescriptorB()[0];
    std::vector<u8> out(desc.Size());
    if (!out.empty()) {
        ctx.GetMemory().ReadBlock(desc.Address(), out.data(), out.size());
    }
    return out;
}

std::mutex g_shared_state_mutex;
std::weak_ptr<BsdState> g_shared_state;
Kernel::KernelCore* g_shared_state_kernel = nullptr;

// One BsdState for bsd:u, bsd:s and bsd:a (a process's table is the same whichever it uses).
std::shared_ptr<BsdState> AcquireSharedState(Kernel::KernelCore& kernel) {
    std::scoped_lock lk{g_shared_state_mutex};
    if (auto existing = g_shared_state.lock(); existing && g_shared_state_kernel == &kernel) {
        return existing;
    }
    auto created = std::make_shared<BsdState>(&kernel);
    g_shared_state = created;
    g_shared_state_kernel = &kernel;
    return created;
}

} // Anonymous namespace

// ---- BsdEventFd ---------------------------------------------------------------------------

// Written from: Linux eventfd(2); Ryujinx master EventFileDescriptor.cs.
BsdEventFd::BsdEventFd(u64 initval, bool semaphore_) : counter{initval}, semaphore{semaphore_} {}

Errno BsdEventFd::Write(u64 value) {
    if (value == 0xffff'ffff'ffff'ffffULL) {
        return Errno::INVAL;
    }
    std::scoped_lock lk{mutex};
    if (value > MaxValue - counter) {
        return Errno::AGAIN;
    }
    counter += value;
    return Errno::SUCCESS;
}

std::pair<u64, Errno> BsdEventFd::Read() {
    std::scoped_lock lk{mutex};
    if (counter == 0) {
        return {0, Errno::AGAIN};
    }
    if (semaphore) {
        --counter;
        return {1, Errno::SUCCESS};
    }
    return {std::exchange(counter, 0), Errno::SUCCESS};
}

bool BsdEventFd::IsReadable() const {
    std::scoped_lock lk{mutex};
    return counter > 0;
}

bool BsdEventFd::IsWritable() const {
    std::scoped_lock lk{mutex};
    return counter < MaxValue;
}

u64 BsdEventFd::Value() const {
    std::scoped_lock lk{mutex};
    return counter;
}

// ---- OpenFile / DescriptorTable / BsdState ------------------------------------------------

// Written from: emulator-parity-ledger §2 Sockets (the host socket closes with the last
// descriptor).
OpenFile::~OpenFile() {
    if (socket) {
        socket->Close();
    }
}

// Written from: Ryujinx master BsdContext.RegisterFileDescriptor (lowest free descriptor).
s32 DescriptorTable::Add(std::shared_ptr<OpenFile> file) {
    std::scoped_lock lk{mutex};
    for (s32 fd = 0; fd < MaxFd; ++fd) {
        if (!files[fd]) {
            files[fd] = std::move(file);
            return fd;
        }
    }
    return -1;
}

std::shared_ptr<OpenFile> DescriptorTable::Get(s32 fd) const {
    if (fd < 0 || fd >= MaxFd) {
        return nullptr;
    }
    std::scoped_lock lk{mutex};
    return files[fd];
}

bool DescriptorTable::Remove(s32 fd) {
    if (fd < 0 || fd >= MaxFd) {
        return false;
    }
    std::shared_ptr<OpenFile> closing;
    {
        std::scoped_lock lk{mutex};
        closing = std::move(files[fd]);
    }
    // The host socket closes here when this was its last descriptor, outside the lock.
    return closing != nullptr;
}

// Written from: Ryujinx master BsdContext.DuplicateFileDescriptor; POSIX dup(2).
s32 DescriptorTable::Duplicate(s32 fd) {
    auto file = Get(fd);
    if (!file) {
        return -1;
    }
    return Add(std::move(file));
}

std::vector<std::shared_ptr<OpenFile>> DescriptorTable::All() const {
    std::scoped_lock lk{mutex};
    std::vector<std::shared_ptr<OpenFile>> out;
    for (const auto& file : files) {
        if (file) {
            out.push_back(file);
        }
    }
    return out;
}

size_t DescriptorTable::Count() const {
    std::scoped_lock lk{mutex};
    return static_cast<size_t>(
        std::count_if(files.begin(), files.end(), [](const auto& f) { return f != nullptr; }));
}

// Written from: Ryujinx master BsdContext.GetOrRegister/DeleteContext (one context per process
// id, dropped with the process).
std::shared_ptr<DescriptorTable> BsdState::GetTable(u64 process_id) {
    std::scoped_lock lk{mutex};
    const auto now = Clock::now();
    auto it = tables.find(process_id);
    if (it == tables.end() || now - last_sweep > std::chrono::seconds{1}) {
        SweepExitedLocked(process_id);
        last_sweep = now;
        it = tables.find(process_id);
    }
    if (it == tables.end()) {
        it = tables.emplace(process_id, std::make_shared<DescriptorTable>(process_id)).first;
        LOG_INFO(Service, "bsd: new descriptor table for process {} ({} tables)", process_id,
                 tables.size());
    }
    return it->second;
}

std::shared_ptr<DescriptorTable> BsdState::FindTable(u64 process_id) const {
    std::scoped_lock lk{mutex};
    const auto it = tables.find(process_id);
    return it == tables.end() ? nullptr : it->second;
}

std::vector<std::shared_ptr<DescriptorTable>> BsdState::Tables() const {
    std::scoped_lock lk{mutex};
    std::vector<std::shared_ptr<DescriptorTable>> out;
    for (const auto& [pid, table] : tables) {
        out.push_back(table);
    }
    return out;
}

void BsdState::SweepExitedLocked(u64 keep) {
    if (kernel == nullptr) {
        return;
    }
    for (auto it = tables.begin(); it != tables.end();) {
        if (it->first == keep) {
            ++it;
            continue;
        }
        auto process = kernel->GetProcessByProcessId(it->first);
        if (process.IsNull() || process->IsTerminated()) {
            LOG_INFO(Service, "bsd: process {} has exited, closing its {} descriptors", it->first,
                     it->second->Count());
            it = tables.erase(it);
        } else {
            ++it;
        }
    }
}

ParkSlot* BsdState::FindSlot(const void* key) {
    std::scoped_lock lk{slot_mutex};
    const auto it = slots.find(key);
    return it == slots.end() ? nullptr : &it->second;
}

void BsdState::StoreSlot(const void* key, const ParkSlot& slot) {
    std::scoped_lock lk{slot_mutex};
    slots[key] = slot;
}

void BsdState::ReleaseSlot(const void* key) {
    std::scoped_lock lk{slot_mutex};
    slots.erase(key);
}

// Written from: the Harbor-on-Eden bsd brief (a resolver signal wakes the parked waiters) and
// Ryujinx master ServerBase.cs (an eventfd write completes the park at once).
void BsdState::AddManager(ServerManager* manager) {
    std::scoped_lock lk{manager_mutex};
    if (std::find(managers.begin(), managers.end(), manager) == managers.end()) {
        managers.push_back(manager);
    }
}

void BsdState::RemoveManagers() {
    std::scoped_lock lk{manager_mutex};
    managers.clear();
}

void BsdState::WakeParked() {
    // Held while signalling: a bsd service clears the list in its destructor, before its server
    // manager closes the deferral event.
    std::scoped_lock lk{manager_mutex};
    for (auto* manager : managers) {
        manager->SignalDeferral();
    }
}

// ---- IPC ----------------------------------------------------------------------------------

std::shared_ptr<DescriptorTable> BSD_USA::TableFor(HLERequestContext& ctx) {
    const auto* process = ctx.GetThread().GetOwnerProcess();
    return state->GetTable(process != nullptr ? process->GetProcessId() : 0);
}

// Written from: Ryujinx master ServerBase.cs (cd462aa69): a request that would block is parked
// without a reply and re-run until it is ready or overdue; Eden's SetIsDeferred is that park.
template <typename Step, typename Respond>
void BSD_USA::RunParkable(HLERequestContext& ctx, Step&& step, Respond&& respond) {
    const u64 thread_id = ctx.GetThread().GetThreadId();
    const u32 command = ctx.GetCommand();
    ParkSlot* existing = state->FindSlot(&ctx);
    if (existing != nullptr && (existing->thread_id != thread_id || existing->command != command)) {
        // Left behind by a request whose session went away while parked; this context is new.
        state->ReleaseSlot(&ctx);
        existing = nullptr;
    }
    ParkSlot local{.thread_id = thread_id, .command = command};
    ParkSlot& slot = existing != nullptr ? *existing : local;

    Outcome outcome;
    if (existing != nullptr) {
        // A client that died while parked gets an answer nobody reads, which ends the park.
        const auto* process = ctx.GetThread().GetOwnerProcess();
        if (process == nullptr || process->IsTerminated()) {
            outcome = Result2{-1, Errno::INTR};
        }
    }
    if (!outcome) {
        outcome = step(slot);
    }
    if (!outcome) {
        if (existing == nullptr) {
            state->StoreSlot(&ctx, slot);
        }
        ctx.SetIsDeferred(true);
        if (auto manager = ctx.GetManager()) {
            auto& server_manager = manager->GetServerManager();
            server_manager.EnableDeferralHeartbeat(DeferralHeartbeat);
            state->AddManager(&server_manager);
        }
        return;
    }
    if (existing != nullptr) {
        state->ReleaseSlot(&ctx);
    }
    respond(*outcome);
}

void BSD_USA::RegisterClient(HLERequestContext& ctx) {
    LOG_DEBUG(Service, "called");
    const auto table = TableFor(ctx);
    const auto* process = ctx.GetThread().GetOwnerProcess();
    LOG_INFO(Service, "{} RegisterClient: process {} ({})", GetServiceName(), table->ProcessId(),
             process != nullptr ? process->GetName() : "?");

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<s32>(0); // bsd errno
}

void BSD_USA::StartMonitoring(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 2};

    rb.Push(ResultSuccess);
}

// Open(u32 flags, buffer<0x21> path) -> (i32 ret, u32 bsd_errno). Only /dev/bpf on a console.
// Written from: switchbrew Sockets services (Open is limited to /dev/bpf) and Ryujinx master
// IClient.Open (EOPNOTSUPP).
void BSD_USA::Open(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) Open answered EOPNOTSUPP");
    PushRetErrno(ctx, {-1, Errno::OPNOTSUPP});
}

// Ioctl(u32 fd, u32 request, u32 bufcount, buffer<0x21> x4) -> (i32 ret, u32 bsd_errno,
// buffer<0x22> x4). Written from: Ryujinx master IClient.Ioctl (SIOCATMARK answers 0, no
// out-of-band data; anything else EOPNOTSUPP) and FreeBSD sys/sockio.h (SIOCATMARK 0x40047307).
void BSD_USA::Ioctl(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();
    const u32 request = rp.Pop<u32>();
    constexpr u32 SiocAtMark = 0x40047307;

    Errno err;
    if (!SocketFile(*TableFor(ctx), fd, err)) {
        PushRetErrno(ctx, {-1, err});
        return;
    }
    if (request == SiocAtMark) {
        WriteOut(ctx, AsBytes(s32{0}));
        PushRetErrno(ctx, {0, Errno::SUCCESS});
        return;
    }
    LOG_WARNING(Service, "ioctl request={:#x} answered EOPNOTSUPP", request);
    PushRetErrno(ctx, {-1, Errno::OPNOTSUPP});
}

void BSD_USA::Socket(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const u32 domain = rp.Pop<u32>();
    const u32 type = rp.Pop<u32>();
    const u32 protocol = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. domain={} type={:#x} protocol={}", domain, type, protocol);

    PushRetErrno(ctx, OpSocket(*TableFor(ctx), is_user, domain, type, protocol));
}

void BSD_USA::SocketExempt(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const u32 domain = rp.Pop<u32>();
    const u32 type = rp.Pop<u32>();
    const u32 protocol = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. domain={} type={:#x} protocol={}", domain, type, protocol);

    const auto table = TableFor(ctx);
    auto [fd, bsd_errno] = OpSocket(*table, is_user, domain, type, protocol);
    if (bsd_errno == Errno::SUCCESS) {
        if (auto file = table->Get(fd); file && file->socket) {
            bsd_errno = Translate(file->socket->Shutdown(Network::ShutdownHow::RD));
        }
    }
    PushRetErrno(ctx, {fd, bsd_errno});
}

// Select(u32 nfds, nn::socket::TimeVal timeout, buffer<fd_set, 0x21> x3) -> (i32 ret, u32 errno,
// buffer<fd_set, 0x22> x3). Written from: Ryujinx master IClient.Select (layout, park) with the
// timeval decoded instead of read as one int.
void BSD_USA::Select(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const u32 nfds = rp.Pop<u32>();
    [[maybe_unused]] const u32 padding = rp.Pop<u32>();
    const s64 tv_sec = rp.Pop<s64>();
    const s64 tv_usec = rp.Pop<s64>();

    if (tv_sec < 0 || tv_usec < 0) {
        PushRetErrno(ctx, {-1, Errno::INVAL});
        return;
    }
    std::optional<std::chrono::milliseconds> timeout;
    if (tv_sec < 100'000'000) {
        timeout = std::chrono::milliseconds{tv_sec * 1000 + tv_usec / 1000};
    }

    const auto table = TableFor(ctx);
    const std::array<std::span<const u8>, 3> in{InBuffer(ctx, 0), InBuffer(ctx, 1),
                                                InBuffer(ctx, 2)};
    std::array<std::vector<u8>, 3> out{std::vector<u8>(OutSize(ctx, 0)),
                                       std::vector<u8>(OutSize(ctx, 1)),
                                       std::vector<u8>(OutSize(ctx, 2))};
    RunParkable(
        ctx, [&](ParkSlot& slot) { return OpSelect(*table, nfds, in, out, timeout, slot); },
        [&](Result2 r) {
            for (size_t i = 0; i < out.size(); ++i) {
                WriteOut(ctx, out[i], i);
            }
            PushRetErrno(ctx, r);
        });
}

void BSD_USA::Poll(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 nfds = rp.Pop<s32>();
    const s32 timeout = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. nfds={} timeout={}", nfds, timeout);

    const auto in = InBuffer(ctx);
    if (nfds < 0 || in.size() < static_cast<size_t>(nfds) * sizeof(PollFD)) {
        PushRetErrno(ctx, {-1, Errno::INVAL});
        return;
    }
    std::vector<PollFD> fds(static_cast<size_t>(nfds));
    if (nfds > 0) {
        std::memcpy(fds.data(), in.data(), fds.size() * sizeof(PollFD));
    }
    const auto table = TableFor(ctx);
    RunParkable(
        ctx, [&](ParkSlot& slot) { return OpPoll(*table, fds, timeout, slot); },
        [&](Result2 r) {
            if (!fds.empty()) {
                WriteOut(ctx,
                         std::span<const u8>{reinterpret_cast<const u8*>(fds.data()),
                                             fds.size() * sizeof(PollFD)});
            }
            PushRetErrno(ctx, r);
        });
}

// Sysctl(buffer<0x21> mib, buffer<0x21> new) -> (i32 ret, u32 errno, u32 oldlen,
// buffer<0x22> old). Written from: Ryujinx master IClient.Sysctl and InterfaceList.cs.
void BSD_USA::Sysctl(HLERequestContext& ctx) {
    const auto mib_bytes = InBuffer(ctx, 0);
    const size_t new_size = ctx.CanReadBuffer(1) ? ctx.GetReadBufferSize(1) : 0;
    const size_t old_size = OutSize(ctx, 0);
    std::vector<s32> mib((std::min)(mib_bytes.size() / 4, size_t{16}));
    if (!mib.empty()) {
        std::memcpy(mib.data(), mib_bytes.data(), mib.size() * 4);
    }

    if (InterfaceList::Matches(mib) && new_size == 0) {
        if (const auto iface = Network::GetSelectedNetworkInterface()) {
            const auto list = InterfaceList::Build(Network::TranslateIPv4(iface->ip_address),
                                                   Network::TranslateIPv4(iface->subnet_mask));
            const u32 length = static_cast<u32>(list.size());
            // No buffer is the size probe; a short buffer is ENOMEM, as on FreeBSD.
            if (old_size != 0 && old_size < list.size()) {
                PushRetErrnoLen(ctx, {-1, Errno::NOMEM}, length);
                return;
            }
            if (old_size != 0) {
                WriteOut(ctx, list, 0);
            }
            PushRetErrnoLen(ctx, {0, Errno::SUCCESS}, length);
            return;
        }
    }
    LOG_WARNING(Service, "(STUBBED) sysctl mib=[{}] old={} new={}", fmt::join(mib, ","),
                old_size, new_size);
    PushRetErrnoLen(ctx, {-1, Errno::OPNOTSUPP}, 0);
}

void BSD_USA::Accept(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={}", fd);

    const auto table = TableFor(ctx);
    std::vector<u8> addr;
    RunParkable(
        ctx, [&](ParkSlot& slot) { return OpAccept(*table, fd, addr, slot); },
        [&](Result2 r) {
            const size_t written = WriteOut(ctx, addr);
            PushRetErrnoLen(ctx, r, r.first >= 0 ? static_cast<u32>(written) : 0);
        });
}

void BSD_USA::Bind(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} addrlen={}", fd, ctx.GetReadBufferSize());
    BuildErrnoResponse(ctx, OpBind(*TableFor(ctx), fd, InBuffer(ctx)));
}

void BSD_USA::Connect(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} addrlen={}", fd, ctx.GetReadBufferSize());

    const auto table = TableFor(ctx);
    const auto addr = InBuffer(ctx);
    RunParkable(
        ctx, [&](ParkSlot& slot) { return OpConnect(*table, fd, addr, slot); },
        [&](Result2 r) { PushRetErrno(ctx, r); });
}

// Written from: Ryujinx master ManagedSocket.RemoteEndPoint (the pending peer stands in for the
// real one while a connect is in flight; ENOTCONN reads as a fatal abort to some titles).
void BSD_USA::GetPeerName(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={}", fd);

    Errno err;
    auto file = SocketFile(*TableFor(ctx), fd, err);
    if (!file) {
        PushRetErrnoLen(ctx, {-1, err}, 0);
        return;
    }
    auto [addr, host_err] = file->socket->GetPeerName();
    {
        std::scoped_lock lk{file->state_mutex};
        if (host_err == Network::Errno::SUCCESS) {
            file->pending_peer.reset();
        } else if (host_err == Network::Errno::NOTCONN && file->pending_peer) {
            addr = *file->pending_peer;
            host_err = Network::Errno::SUCCESS;
        }
    }
    if (host_err != Network::Errno::SUCCESS) {
        PushRetErrnoLen(ctx, {-1, Translate(host_err)}, 0);
        return;
    }
    const size_t written = WriteOut(ctx, GuestAddrBytes(*file, addr));
    PushRetErrnoLen(ctx, {0, Errno::SUCCESS}, static_cast<u32>(written));
}

void BSD_USA::GetSockName(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={}", fd);

    Errno err;
    auto file = SocketFile(*TableFor(ctx), fd, err);
    if (!file) {
        PushRetErrnoLen(ctx, {-1, err}, 0);
        return;
    }
    const auto [addr, host_err] = file->socket->GetSockName();
    if (host_err != Network::Errno::SUCCESS) {
        PushRetErrnoLen(ctx, {-1, Translate(host_err)}, 0);
        return;
    }
    const size_t written = WriteOut(ctx, GuestAddrBytes(*file, addr));
    PushRetErrnoLen(ctx, {0, Errno::SUCCESS}, static_cast<u32>(written));
}

void BSD_USA::GetSockOpt(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();
    const u32 level = rp.Pop<u32>();
    const u32 optname = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. fd={} level={:#x} optname={:#x} len={:#x}", fd, level, optname,
              OutSize(ctx));

    Errno err;
    auto file = SocketFile(*TableFor(ctx), fd, err);
    if (!file) {
        PushRetErrnoLen(ctx, {-1, err}, 0);
        return;
    }
    const auto [value, get_err] = OpGetSockOpt(*file, level, optname);
    if (get_err != Errno::SUCCESS) {
        PushRetErrnoLen(ctx, {-1, get_err}, 0);
        return;
    }
    const size_t written = WriteOut(ctx, value);
    PushRetErrnoLen(ctx, {0, Errno::SUCCESS}, static_cast<u32>(written));
}

void BSD_USA::Listen(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();
    const s32 backlog = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} backlog={}", fd, backlog);

    Errno err;
    auto file = SocketFile(*TableFor(ctx), fd, err);
    BuildErrnoResponse(ctx, file ? Translate(file->socket->Listen(backlog)) : err);
}

// Written from: POSIX fcntl(2) and Ryujinx master IClient.Fcntl (F_GETFL 3, F_SETFL 4,
// O_NONBLOCK 0x800).
void BSD_USA::Fcntl(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();
    const s32 cmd = rp.Pop<s32>();
    const s32 arg = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} cmd={} arg={}", fd, cmd, arg);

    auto file = TableFor(ctx)->Get(fd);
    if (!file) {
        PushRetErrno(ctx, {-1, Errno::BADF});
        return;
    }
    switch (static_cast<FcntlCmd>(cmd)) {
    case FcntlCmd::GETFL:
        PushRetErrno(ctx, {file->non_blocking ? static_cast<s32>(Network::FLAG_O_NONBLOCK) : 0,
                           Errno::SUCCESS});
        return;
    case FcntlCmd::SETFL: {
        const bool enable = (arg & Network::FLAG_O_NONBLOCK) != 0;
        if (file->socket) {
            const Errno bsd_errno = Translate(file->socket->SetNonBlock(enable));
            if (bsd_errno != Errno::SUCCESS) {
                PushRetErrno(ctx, {-1, bsd_errno});
                return;
            }
        }
        file->non_blocking = enable || file->event != nullptr;
        PushRetErrno(ctx, {0, Errno::SUCCESS});
        return;
    }
    default:
        LOG_WARNING(Service, "fcntl cmd={} arg={:#x} not supported", cmd, arg);
        PushRetErrno(ctx, {-1, Errno::INVAL});
        return;
    }
}

void BSD_USA::SetSockOpt(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};

    const s32 fd = rp.Pop<s32>();
    const u32 level = rp.Pop<u32>();
    const u32 optname = rp.Pop<u32>();
    const auto optval = InBuffer(ctx);

    LOG_DEBUG(Service, "called. fd={} level={:#x} optname={:#x} optlen={}", fd, level, optname,
              optval.size());

    Errno err;
    auto file = SocketFile(*TableFor(ctx), fd, err);
    BuildErrnoResponse(ctx, file ? OpSetSockOpt(*file, level, optname, optval) : err);
}

void BSD_USA::Shutdown(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};

    const s32 fd = rp.Pop<s32>();
    const s32 how = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} how={}", fd, how);

    Errno err;
    auto file = SocketFile(*TableFor(ctx), fd, err);
    if (!file) {
        BuildErrnoResponse(ctx, err);
        return;
    }
    if (how < 0 || how > 2) {
        BuildErrnoResponse(ctx, Errno::INVAL);
        return;
    }
    BuildErrnoResponse(
        ctx, Translate(file->socket->Shutdown(Translate(static_cast<ShutdownHow>(how)))));
}

// Written from: Ryujinx master IClient.ShutdownAllSockets / BsdContext.ShutdownAllSockets.
void BSD_USA::ShutdownAllSockets(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 how = rp.Pop<s32>();

    if (how < 0 || how > 2) {
        BuildErrnoResponse(ctx, Errno::INVAL);
        return;
    }
    for (const auto& file : TableFor(ctx)->All()) {
        if (file->socket) {
            file->socket->Shutdown(Translate(static_cast<ShutdownHow>(how)));
        }
    }
    BuildErrnoResponse(ctx, Errno::SUCCESS);
}

void BSD_USA::Recv(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};

    const s32 fd = rp.Pop<s32>();
    const u32 flags = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. fd={} flags={:#x} len={}", fd, flags, OutSize(ctx));

    const auto table = TableFor(ctx);
    std::vector<u8> message(OutSize(ctx));
    RunParkable(
        ctx, [&](ParkSlot& slot) { return OpRecv(*table, fd, flags, message, nullptr, slot); },
        [&](Result2 r) {
            if (r.first > 0) {
                WriteOut(ctx, std::span<const u8>{message.data(), static_cast<size_t>(r.first)});
            }
            PushRetErrno(ctx, r);
        });
}

void BSD_USA::RecvFrom(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};

    const s32 fd = rp.Pop<s32>();
    const u32 flags = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. fd={} flags={:#x} len={} addrlen={}", fd, flags, OutSize(ctx, 0),
              OutSize(ctx, 1));

    const auto table = TableFor(ctx);
    std::vector<u8> message(OutSize(ctx, 0));
    std::vector<u8> addr;
    RunParkable(
        ctx, [&](ParkSlot& slot) { return OpRecv(*table, fd, flags, message, &addr, slot); },
        [&](Result2 r) {
            if (r.first > 0) {
                WriteOut(ctx, std::span<const u8>{message.data(), static_cast<size_t>(r.first)},
                         0);
            }
            const size_t written = WriteOut(ctx, addr, 1);
            PushRetErrnoLen(ctx, r, static_cast<u32>(written));
        });
}

void BSD_USA::Send(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};

    const s32 fd = rp.Pop<s32>();
    const u32 flags = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. fd={} flags={:#x} len={}", fd, flags, ctx.GetReadBufferSize());

    PushRetErrno(ctx, OpSend(*TableFor(ctx), fd, flags, InBuffer(ctx), {}));
}

void BSD_USA::SendTo(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();
    const u32 flags = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. fd={} flags={:#x} len={} addrlen={}", fd, flags,
              ctx.GetReadBufferSize(0), ctx.GetReadBufferSize(1));

    PushRetErrno(ctx, OpSend(*TableFor(ctx), fd, flags, InBuffer(ctx, 0), InBuffer(ctx, 1)));
}

// Written from: Linux eventfd(2) (an 8-byte counter write) and Ryujinx master IClient.Write.
void BSD_USA::Write(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();
    const auto data = InBuffer(ctx);

    LOG_DEBUG(Service, "called. fd={} len={}", fd, data.size());

    const auto table = TableFor(ctx);
    auto file = table->Get(fd);
    if (file && file->event) {
        if (data.size() < sizeof(u64)) {
            PushRetErrno(ctx, {-1, Errno::INVAL});
            return;
        }
        const Errno e = file->event->Write(GetValue<u64>(data));
        if (e == Errno::SUCCESS) {
            // A write is the wakeup of a parked poll: re-run the parked requests now.
            if (auto manager = ctx.GetManager()) {
                manager->GetServerManager().SignalDeferral();
            }
        }
        PushRetErrno(ctx, {e == Errno::SUCCESS ? static_cast<s32>(sizeof(u64)) : -1, e});
        return;
    }
    PushRetErrno(ctx, OpSend(*table, fd, 0, data, {}));
}

// Written from: Linux eventfd(2) (an 8-byte counter read; EAGAIN when zero) and the recv parking.
void BSD_USA::Read(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} len={}", fd, OutSize(ctx));

    const auto table = TableFor(ctx);
    auto file = table->Get(fd);
    if (file && file->event) {
        if (OutSize(ctx) < sizeof(u64)) {
            PushRetErrno(ctx, {-1, Errno::INVAL});
            return;
        }
        const auto [value, e] = file->event->Read();
        if (e != Errno::SUCCESS) {
            PushRetErrno(ctx, {-1, e});
            return;
        }
        WriteOut(ctx, AsBytes(value));
        PushRetErrno(ctx, {static_cast<s32>(sizeof(u64)), Errno::SUCCESS});
        return;
    }
    std::vector<u8> message(OutSize(ctx));
    RunParkable(
        ctx, [&](ParkSlot& slot) { return OpRecv(*table, fd, 0, message, nullptr, slot); },
        [&](Result2 r) {
            if (r.first > 0) {
                WriteOut(ctx, std::span<const u8>{message.data(), static_cast<size_t>(r.first)});
            }
            PushRetErrno(ctx, r);
        });
}

void BSD_USA::Close(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    const auto table = TableFor(ctx);
    LOG_DEBUG(Service, "called. fd={} (process {})", fd, table->ProcessId());

    BuildErrnoResponse(ctx, table->Remove(fd) ? Errno::SUCCESS : Errno::BADF);
}

/// @brief Only bsd:s is able to dup()
// Written from: emulator-parity-ledger §2 Sockets ("bsd:u refuses ... DuplicateSocket with
// ENOENT").
void BSD_USA::DuplicateSocket(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();
    [[maybe_unused]] const u32 padding = rp.Pop<u32>();
    [[maybe_unused]] const u64 reserved = rp.Pop<u64>();

    if (is_user) {
        PushRetErrno(ctx, {-1, Errno::NOENT});
        return;
    }
    const s32 new_fd = TableFor(ctx)->Duplicate(fd);
    PushRetErrno(ctx, new_fd < 0 ? Result2{-1, Errno::BADF} : Result2{new_fd, Errno::SUCCESS});
}

// [Harbor] A named message carries its peer's address, as msg_name does for sendmmsg(2) and
// recvmmsg(2): a send goes to it, a receive writes the source into it. The name keeps the length
// the guest gave (an IPv4 address fills its start, the rest is zeroed): whether the guest reads a
// shorter name length back is not established, so the layout it sent is the one it gets.
static void FillName(MsgHdr& m, const std::vector<u8>& addr) {
    std::fill(m.name.begin(), m.name.end(), u8{0});
    std::copy_n(addr.begin(), (std::min)(addr.size(), m.name.size()), m.name.begin());
}

static void WarnNamedMMsg(const char* command) {
    static std::atomic_bool warned{false};
    if (!warned.exchange(true)) {
        LOG_WARNING(Service, "unknown contract: bsd {} named message: the name is the peer address, "
                             "its length is kept",
                    command);
    }
}

// RecvMMsg(u32 fd, u32 vlen, u32 flags, u32 reserved, nn::socket::TimeVal timeout) -> (i32 ret,
// u32 bsd_errno, buffer<0x6> messages). Written from: emulator-parity-ledger §2 Sockets
// ("SendMMsg/RecvMMsg are one concatenated call"), Ryujinx master IClient.RecvMMsg (layout) and
// recvmmsg(2) (the first message may wait, the rest do not; msg_name gets the source).
void BSD_USA::RecvMMsg(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();
    const u32 vlen = rp.Pop<u32>();
    const u32 flags = rp.Pop<u32>();
    [[maybe_unused]] const u32 reserved = rp.Pop<u32>();
    const s64 tv_sec = rp.Pop<s64>();
    const s64 tv_usec = rp.Pop<s64>();

    auto messages = ParseMMsg(ReadBufferB(ctx), (std::min)(vlen, 1024u));
    if (!messages) {
        PushRetErrno(ctx, {-1, Errno::FAULT});
        return;
    }
    for (const auto& m : *messages) {
        if (!m.name.empty()) {
            WarnNamedMMsg("RecvMMsg");
            break;
        }
    }
    const auto table = TableFor(ctx);
    std::vector<u8> first;
    std::vector<u8> first_addr;
    RunParkable(
        ctx,
        [&](ParkSlot& slot) -> Outcome {
            if (messages->empty()) {
                return Result2{0, Errno::SUCCESS};
            }
            size_t capacity = 0;
            for (const auto& v : (*messages)[0].iov) {
                capacity += v.size();
            }
            first.assign(capacity, 0);
            auto r = OpRecv(*table, fd, flags, first,
                            (*messages)[0].name.empty() ? nullptr : &first_addr, slot);
            if (!r && tv_sec >= 0 && tv_usec >= 0 && (tv_sec | tv_usec) != 0 &&
                !slot.deadline_overridden) {
                // The timeval bounds the wait for the first message.
                slot.deadline_overridden = true;
                slot.deadline = Clock::now() + std::chrono::milliseconds{tv_sec * 1000 +
                                                                        tv_usec / 1000};
            }
            return r;
        },
        [&](Result2 r) {
            if (r.first < 0 || messages->empty()) {
                PushRetErrno(ctx, r);
                return;
            }
            s32 count = 0;
            for (size_t i = 0; i < messages->size(); ++i) {
                auto& m = (*messages)[i];
                std::vector<u8> data;
                std::vector<u8> addr;
                s32 got;
                if (i == 0) {
                    data = std::move(first);
                    addr = std::move(first_addr);
                    got = r.first;
                } else {
                    size_t capacity = 0;
                    for (const auto& v : m.iov) {
                        capacity += v.size();
                    }
                    data.assign(capacity, 0);
                    ParkSlot none;
                    const auto more = OpRecv(*table, fd, flags | Network::FLAG_MSG_DONTWAIT, data,
                                             m.name.empty() ? nullptr : &addr, none);
                    if (!more || more->first < 0) {
                        break;
                    }
                    got = more->first;
                }
                size_t offset = 0;
                for (auto& v : m.iov) {
                    const size_t n = (std::min)(v.size(), static_cast<size_t>(got) - offset);
                    std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(offset), n, v.begin());
                    offset += n;
                }
                if (!m.name.empty()) {
                    FillName(m, addr);
                }
                m.length = static_cast<u32>(got);
                ++count;
            }
            ctx.WriteBuffer(SerializeMMsg(*messages));
            PushRetErrno(ctx, {count, Errno::SUCCESS});
        });
}

// SendMMsg(u32 fd, u32 vlen, u32 flags) -> (i32 ret, u32 bsd_errno, buffer<0x6> messages).
// Written from: emulator-parity-ledger §2 Sockets (one concatenated call per message) and
// sendmmsg(2) (the count sent; an error only if none was; a named message goes to its name).
void BSD_USA::SendMMsg(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();
    const u32 vlen = rp.Pop<u32>();
    const u32 flags = rp.Pop<u32>();

    auto messages = ParseMMsg(ReadBufferB(ctx), (std::min)(vlen, 1024u));
    if (!messages) {
        PushRetErrno(ctx, {-1, Errno::FAULT});
        return;
    }
    const auto table = TableFor(ctx);
    s32 count = 0;
    Errno first_error = Errno::SUCCESS;
    for (auto& m : *messages) {
        if (!m.name.empty()) {
            WarnNamedMMsg("SendMMsg");
        }
        std::vector<u8> data;
        for (const auto& v : m.iov) {
            data.insert(data.end(), v.begin(), v.end());
        }
        const auto r = OpSend(*table, fd, flags, data, m.name);
        if (r.first < 0) {
            first_error = r.second;
            break;
        }
        m.length = static_cast<u32>(r.first);
        ++count;
    }
    if (count == 0 && first_error != Errno::SUCCESS) {
        PushRetErrno(ctx, {-1, first_error});
        return;
    }
    ctx.WriteBuffer(SerializeMMsg(*messages));
    PushRetErrno(ctx, {count, Errno::SUCCESS});
}

// EventFd(u32 flags, u32 padding, u64 initval) -> (i32 ret, u32 bsd_errno)
void BSD_USA::EventFd(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const u32 flags = rp.Pop<u32>();
    [[maybe_unused]] const u32 padding = rp.Pop<u32>();
    const u64 initval = rp.Pop<u64>();

    PushRetErrno(ctx, OpEventFd(*TableFor(ctx), initval, flags));
}

void BSD_USA::BuildErrnoResponse(HLERequestContext& ctx, Errno bsd_errno) const noexcept {
    PushRetErrno(ctx, ErrnoResult(bsd_errno));
}

void BSD_USA::OnProxyPacketReceived(const Network::ProxyPacket& packet) {
    for (const auto& table : state->Tables()) {
        for (const auto& file : table->All()) {
            if (file->is_proxy && file->socket) {
                file->socket->HandleProxyPacket(packet);
            }
        }
    }
}

// ---- Entry points for other services (ssl, the resolver) ----------------------------------

namespace {

// Written from: the Harbor-on-Eden bsd brief (ssl reaches the caller's table through the process
// whose request is being served).
std::shared_ptr<DescriptorTable> CallerTable(BsdState& state, s32 fd) {
    if (const auto pid = Service::GetCurrentRequestProcessId()) {
        return state.GetTable(*pid);
    }
    std::shared_ptr<DescriptorTable> found;
    size_t candidates = 0;
    for (const auto& table : state.Tables()) {
        if (table->Get(fd)) {
            found = table;
            ++candidates;
        }
    }
    if (candidates == 1) {
        return found;
    }
    LOG_ERROR(Service, "bsd: fd {} named outside a request ({} candidate processes)", fd,
              candidates);
    return nullptr;
}

} // Anonymous namespace

std::variant<s32, Errno> BSD_USA::DuplicateSocketImpl(s32 fd) {
    const auto table = CallerTable(*state, fd);
    if (!table) {
        return Errno::BADF;
    }
    return DuplicateSocketImpl(table->ProcessId(), fd);
}

Errno BSD_USA::CloseImpl(s32 fd) {
    const auto table = CallerTable(*state, fd);
    return table ? CloseImpl(table->ProcessId(), fd) : Errno::BADF;
}

std::optional<std::shared_ptr<Network::SocketBase>> BSD_USA::GetSocket(s32 fd) {
    const auto table = CallerTable(*state, fd);
    if (!table) {
        return std::nullopt;
    }
    return GetSocket(table->ProcessId(), fd);
}

std::variant<s32, Errno> BSD_USA::DuplicateSocketImpl(u64 process_id, s32 fd) {
    auto file = state->GetTable(process_id)->Get(fd);
    if (!file || !file->socket) {
        return Errno::BADF;
    }
    const s32 new_fd = state->GetTable(process_id)->Duplicate(fd);
    if (new_fd < 0) {
        return Errno::MFILE;
    }
    return new_fd;
}

Errno BSD_USA::CloseImpl(u64 process_id, s32 fd) {
    return state->GetTable(process_id)->Remove(fd) ? Errno::SUCCESS : Errno::BADF;
}

std::optional<std::shared_ptr<Network::SocketBase>> BSD_USA::GetSocket(u64 process_id, s32 fd) {
    auto file = state->GetTable(process_id)->Get(fd);
    if (!file || !file->socket) {
        return std::nullopt;
    }
    return file->socket;
}

std::shared_ptr<BsdEventFd> BSD_USA::GetEventFd(u64 process_id, s32 fd) {
    const auto table = state->FindTable(process_id);
    if (!table) {
        return nullptr;
    }
    auto file = table->Get(fd);
    return file ? file->event : nullptr;
}

namespace {

// Written from: the net stream's request (fw23 resolver signals a guest eventfd by process and
// fd) and Linux eventfd(2) (a write of 1).
bool SignalEventFdIn(BsdState& st, u64 process_id, s32 fd) {
    const auto table = st.FindTable(process_id);
    if (!table) {
        return false;
    }
    const auto file = table->Get(fd);
    if (!file || !file->event) {
        return false;
    }
    if (file->event->Write(1) != Errno::SUCCESS) {
        return false;
    }
    st.WakeParked();
    return true;
}

} // Anonymous namespace

bool SignalGuestEventFd(u64 process_id, s32 fd) {
    std::shared_ptr<BsdState> shared;
    {
        std::scoped_lock lk{g_shared_state_mutex};
        shared = g_shared_state.lock();
    }
    return shared && SignalEventFdIn(*shared, process_id, fd);
}

std::function<bool(u64 process_id, s32 fd)> GuestEventFdSignalHook() {
    return [](u64 process_id, s32 fd) { return SignalGuestEventFd(process_id, fd); };
}

namespace {

// Installs the hook on a resolver that has SetEventFdSignal; compiles to nothing on one that
// does not (sfdnsres before the net stream's resolver lands).
template <typename Resolver>
bool InstallEventFdSignalOn() {
    if constexpr (requires { Resolver::SetEventFdSignal(GuestEventFdSignalHook()); }) {
        Resolver::SetEventFdSignal(GuestEventFdSignalHook());
        return true;
    } else {
        return false;
    }
}

} // Anonymous namespace

// Written from: the Harbor-on-Eden bsd brief (bsd installs the resolver's eventfd signal).
bool InstallResolverEventFdSignal() {
    static const bool installed = [] {
        const bool ok = InstallEventFdSignalOn<SFDNSRES>();
        LOG_INFO(Service, "bsd: resolver eventfd signal {}",
                 ok ? "installed" : "not available in this sfdnsres");
        return ok;
    }();
    return installed;
}

// ---- Self-check ---------------------------------------------------------------------------

// Written from: the Harbor-on-Eden bsd brief (two fake client processes, fd isolation, a parked
// recv woken by data, a zero-timeout poll) plus the behaviours above. Host loopback UDP only.
bool BSD_USA::RunSelfCheck() {
    BsdState st{nullptr};
    int passed = 0;
    int failed = 0;
    const auto check = [&](bool ok, std::string_view name) {
        if (ok) {
            ++passed;
            LOG_INFO(Service, "bsd self-check: PASS {}", name);
        } else {
            ++failed;
            LOG_ERROR(Service, "bsd self-check: FAIL {}", name);
        }
    };
    const auto v4 = [](Network::IPv4Address ip, u16 port) {
        SockAddrIn g{};
        g.len = SockAddrInSize;
        g.family = static_cast<u8>(Domain::INET);
        g.portno = Swap16(port);
        g.ip = ip;
        std::vector<u8> out(SockAddrInSize);
        std::memcpy(out.data(), &g, SockAddrInSize);
        return out;
    };
    const auto port_of = [](DescriptorTable& t, s32 fd) {
        auto file = t.Get(fd);
        return file ? file->socket->GetSockName().first.portno : u16{0};
    };
    constexpr Network::IPv4Address loopback{127, 0, 0, 1};
    constexpr u32 AfInet = 2;
    constexpr u32 AfInet6 = 28;
    constexpr u32 SockDgram = 2;
    constexpr u32 IpUdp = 17;

    auto a = st.GetTable(0xA001);
    auto b = st.GetTable(0xB002);
    check(a != b && st.FindTable(0xA001) == a && st.GetTable(0xA001) == a,
          "one descriptor table per process");

    const auto [fa, ea] = OpSocket(*a, false, AfInet, SockDgram, IpUdp);
    const auto [fb, eb] = OpSocket(*b, false, AfInet, SockDgram, IpUdp);
    check(fa == 0 && fb == 0 && ea == Errno::SUCCESS && eb == Errno::SUCCESS,
          "each process numbers its descriptors from 0");
    check(a->Get(fa) && b->Get(fb) && a->Get(fa) != b->Get(fb) &&
              a->Get(fa)->socket->GetFD() != b->Get(fb)->socket->GetFD(),
          "the same fd names different sockets in different processes");

    const auto [fa1, ea1] = OpSocket(*a, false, AfInet, SockDgram | GuestSockNonBlock, IpUdp);
    check(fa1 == 1 && ea1 == Errno::SUCCESS && a->Get(1)->non_blocking && !b->Get(1),
          "fd isolation: process B has no fd 1; SOCK_NONBLOCK applied at creation");
    check(a->Get(fa)->socket->GetIntOption(Network::SockOpt::RcvBuf).first >= (1 << 20),
          "UDP receive buffer is at least 1 MiB");

    check(OpBind(*a, fa, v4(loopback, 0)) == Errno::SUCCESS && port_of(*a, fa) != 0,
          "bind on loopback");
    const u16 port_a = port_of(*a, fa);

    {
        std::array<PollFD, 1> p{PollFD{fa, PollEvents::In, PollEvents{}}};
        ParkSlot slot;
        const auto t0 = Clock::now();
        const auto r = OpPoll(*a, p, 0, slot);
        check(r && r->first == 0 && r->second == Errno::SUCCESS && !slot.parked &&
                  Clock::now() - t0 < std::chrono::milliseconds{50},
              "a zero-timeout poll that finds nothing answers 0 at once");
    }
    {
        std::array<PollFD, 1> p{PollFD{fa, PollEvents::In, PollEvents{}}};
        ParkSlot slot;
        const auto r1 = OpPoll(*a, p, 30, slot);
        std::this_thread::sleep_for(std::chrono::milliseconds{40});
        const auto r2 = OpPoll(*a, p, 30, slot);
        check(!r1 && slot.parked && r2 && r2->first == 0 && r2->second == Errno::SUCCESS,
              "a blocking poll parks and its timeout answers 0 / success");
    }
    {
        std::array<PollFD, 2> p{PollFD{fa, PollEvents::In, PollEvents{}},
                                PollFD{77, PollEvents::In, PollEvents{}}};
        ParkSlot slot;
        const auto r = OpPoll(*a, p, -1, slot);
        check(r && r->first == 1 && p[1].revents == PollEvents::Nval &&
                  p[0].revents == PollEvents{},
              "a dead fd is POLLNVAL on its entry, the rest is evaluated");
    }
    {
        std::vector<u8> message(64);
        std::vector<u8> from;
        ParkSlot slot;
        const auto r1 = OpRecv(*a, fa, 0, message, &from, slot);
        check(!r1 && slot.parked, "a blocking recv on an empty datagram socket parks");

        const std::array<u8, 6> payload{'h', 'a', 'r', 'b', 'o', 'r'};
        const auto sent = OpSend(*b, fb, 0, payload, v4(loopback, port_a));
        Outcome r2;
        for (int i = 0; i < 1000 && !r2; ++i) {
            std::this_thread::sleep_for(DeferralHeartbeat);
            r2 = OpRecv(*a, fa, 0, message, &from, slot);
        }
        check(sent.first == 6 && r2 && r2->first == 6 &&
                  std::memcmp(message.data(), payload.data(), payload.size()) == 0 &&
                  from.size() == SockAddrInSize,
              "the parked recv is woken by data from the other process");
    }
    check(b->Remove(fb) && !b->Get(fb) && a->Get(fa) != nullptr,
          "closing fd 0 in one process leaves the other's fd 0 open");

    {
        const auto [fe, ee] = OpEventFd(*a, 0, 0);
        std::array<PollFD, 1> p{PollFD{fe, PollEvents::In, PollEvents{}}};
        ParkSlot slot;
        const auto r1 = OpPoll(*a, p, -1, slot);
        auto event = a->Get(fe)->event;
        const Errno w1 = event->Write(3);
        const auto r2 = OpPoll(*a, p, -1, slot);
        const u64 after_poll = event->Value();
        event->Write(2);
        const auto read1 = event->Read();
        const auto read2 = event->Read();
        check(ee == Errno::SUCCESS && !r1 && w1 == Errno::SUCCESS && r2 && r2->first == 1 &&
                  True(p[0].revents & PollEvents::In) && after_poll == 0 && read1.first == 2 &&
                  read1.second == Errno::SUCCESS && read2.second == Errno::AGAIN,
              "eventfd: a parked poll wakes on a write and drains it; read takes the count, "
              "then EAGAIN");
        const bool signalled = SignalEventFdIn(st, 0xA001, fe);
        const u64 after_signal = event->Value();
        event->Read();
        check(signalled && after_signal == 1 && !SignalEventFdIn(st, 0xB002, fe) &&
                  !SignalEventFdIn(st, 0xA001, fa) && !SignalEventFdIn(st, 0xC003, fe),
              "the resolver signal adds 1 to that process's eventfd only; a socket or another "
              "process's fd is refused");
        BsdEventFd sem{2, true};
        check(sem.Read().first == 1 && sem.Read().first == 1 && sem.Read().second == Errno::AGAIN &&
                  sem.Write(0xffff'ffff'ffff'ffffULL) == Errno::INVAL,
              "eventfd semaphore mode and the invalid write");
    }
    {
        // A closed port: bind a socket, note its port, close it.
        const auto [fc, ec] = OpSocket(*b, false, AfInet, SockDgram, IpUdp);
        OpBind(*b, fc, v4(loopback, 0));
        const u16 closed_port = port_of(*b, fc);
        b->Remove(fc);
        ParkSlot slot;
        const auto c = OpConnect(*a, fa1, v4(loopback, closed_port), slot);
        const std::array<u8, 1> probe{0};
        OpSend(*a, fa1, 0, probe, {});
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        std::array<PollFD, 1> p{PollFD{fa1, PollEvents::In | PollEvents::Out, PollEvents{}}};
        const auto r = OpPoll(*a, p, 0, slot);
        check(c && c->second == Errno::SUCCESS && r && !True(p[0].revents & PollEvents::Hup),
              "a datagram socket never reports POLLHUP (ICMP unreachable pending)");
    }
    {
        SockAddrIn6 in6{};
        in6.len = sizeof(SockAddrIn6);
        in6.family = static_cast<u8>(Domain::INET6);
        in6.portno = Swap16(4242);
        in6.addr[10] = 0xff;
        in6.addr[11] = 0xff;
        in6.addr[12] = 127;
        in6.addr[15] = 1;
        const auto mapped = ParseGuestAddr(AsBytes(in6));
        SockAddrIn6 other = in6;
        other.addr = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        check(mapped && mapped->ip == loopback && mapped->portno == 4242 &&
                  !ParseGuestAddr(AsBytes(other)),
              "v4-mapped addresses translate, a real IPv6 address does not");

        const auto [f6, e6] = OpSocket(*b, false, AfInet6, SockDgram, IpUdp);
        in6.portno = 0;
        const Errno bind6 = OpBind(*b, f6, AsBytes(in6));
        auto file6 = b->Get(f6);
        const auto bytes = file6 ? GuestAddrBytes(*file6, file6->socket->GetSockName().first)
                                 : std::vector<u8>{};
        // sockaddr_in6: len, family, port, flowinfo, then the address at offset 8.
        check(e6 == Errno::SUCCESS && bind6 == Errno::SUCCESS && bytes.size() == 28 &&
                  bytes[1] == AfInet6 && bytes[18] == 0xff && bytes[19] == 0xff &&
                  bytes[20] == 127 && bytes[23] == 1,
              "an INET6 socket binds ::ffff:127.0.0.1 and names itself v4-mapped");
    }
    {
        std::vector<u8> read_set(16);
        read_set[0] = 1; // fd 0 of process A
        const std::array<std::span<const u8>, 3> in{read_set, {}, {}};
        std::array<std::vector<u8>, 3> out{std::vector<u8>(16), std::vector<u8>(16),
                                           std::vector<u8>(16)};
        ParkSlot slot;
        const auto r1 = OpSelect(*a, 1, in, out, std::chrono::milliseconds{20}, slot);
        std::this_thread::sleep_for(std::chrono::milliseconds{30});
        const auto r2 = OpSelect(*a, 1, in, out, std::chrono::milliseconds{20}, slot);
        check(!r1 && r2 && r2->first == 0 && out[0][0] == 0,
              "select parks and answers 0 with empty sets on its timeout");
    }
    {
        // Type 0 is DGRAM; bsd:u refuses RAW with ENOENT but lets ICMP through as a ping socket.
        const auto [f0, e0] = OpSocket(*b, true, AfInet, 0x10000000, 0);
        const auto [fr, er] = OpSocket(*b, true, AfInet, 3, 6);
        check(e0 == Errno::SUCCESS && b->Get(f0)->is_datagram && fr == -1 && er == Errno::NOENT,
              "type 0 + SOCK_CLOEXEC is DGRAM; bsd:u refuses RAW TCP with ENOENT");
    }

    LOG_INFO(Service, "bsd self-check: {} passed, {} failed", passed, failed);
    return failed == 0;
}

// ---- Construction -------------------------------------------------------------------------

BSD_USA::BSD_USA(Core::System& system_, const char* name, bool is_user_)
    : ServiceFramework{system_, name}
    , is_user{is_user_} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, &BSD_USA::RegisterClient, "RegisterClient"},
        {1, &BSD_USA::StartMonitoring, "StartMonitoring"},
        {2, &BSD_USA::Socket, "Socket"},
        {3, &BSD_USA::SocketExempt, "SocketExempt"},
        {4, &BSD_USA::Open, "Open"},
        {5, &BSD_USA::Select, "Select"},
        {6, &BSD_USA::Poll, "Poll"},
        {7, &BSD_USA::Sysctl, "Sysctl"},
        {8, &BSD_USA::Recv, "Recv"},
        {9, &BSD_USA::RecvFrom, "RecvFrom"},
        {10, &BSD_USA::Send, "Send"},
        {11, &BSD_USA::SendTo, "SendTo"},
        {12, &BSD_USA::Accept, "Accept"},
        {13, &BSD_USA::Bind, "Bind"},
        {14, &BSD_USA::Connect, "Connect"},
        {15, &BSD_USA::GetPeerName, "GetPeerName"},
        {16, &BSD_USA::GetSockName, "GetSockName"},
        {17, &BSD_USA::GetSockOpt, "GetSockOpt"},
        {18, &BSD_USA::Listen, "Listen"},
        {19, &BSD_USA::Ioctl, "Ioctl"},
        {20, &BSD_USA::Fcntl, "Fcntl"},
        {21, &BSD_USA::SetSockOpt, "SetSockOpt"},
        {22, &BSD_USA::Shutdown, "Shutdown"},
        {23, &BSD_USA::ShutdownAllSockets, "ShutdownAllSockets"},
        {24, &BSD_USA::Write, "Write"},
        {25, &BSD_USA::Read, "Read"},
        {26, &BSD_USA::Close, "Close"},
        {27, &BSD_USA::DuplicateSocket, "DuplicateSocket"},
        {28, nullptr, "GetResourceStatistics"},
        {29, &BSD_USA::RecvMMsg, "RecvMMsg"}, //3.0.0+
        {30, &BSD_USA::SendMMsg, "SendMMsg"}, //3.0.0+
        {31, &BSD_USA::EventFd, "EventFd"}, //7.0.0+
        {32, nullptr, "RegisterResourceStatisticsName"}, //7.0.0+
        {33, nullptr, "RegisterClientShared"}, //10.0.0+
        {34, nullptr, "GetSocketStatistics"}, //15.0.0+
        {35, nullptr, "NifIoctl"}, //17.0.0+
        {36, nullptr, "Unknown36"}, //18.0.0+
        {37, nullptr, "Unknown37"}, //18.0.0+
        {38, nullptr, "Unknown38"}, //18.0.0+
        {39, nullptr, "Unknown39"}, //20.0.0+
        {40, nullptr, "Unknown40"}, //20.0.0+
        {41, nullptr, "Unknown41"}, //21.0.0+
        {42, nullptr, "Unknown42"}, //21.0.0+
        {43, nullptr, "Unknown43"}, //21.0.0+
        {200, nullptr, "SetThreadCoreMask"}, //15.0.0+
        {201, nullptr, "GetThreadCoreMask"}, //15.0.0+
    };
    // clang-format on

    RegisterHandlers(functions);

    state = AcquireSharedState(system_.Kernel());
    InstallResolverEventFdSignal();

    if (auto room_member = Network::GetRoomMember().lock()) {
        proxy_packet_received = room_member->BindOnProxyPacketReceived(
            [this](const Network::ProxyPacket& packet) { OnProxyPacketReceived(packet); });
    } else {
        LOG_ERROR(Service, "Network isn't initialized");
    }

    if (!is_user) {
        if (const char* env = std::getenv("HARBOR_BSD_SELFCHECK"); env && env[0] == '1') {
            RunSelfCheck();
        }
    }
}

BSD_USA::~BSD_USA() {
    state->RemoveManagers();
    if (auto room_member = Network::GetRoomMember().lock()) {
        room_member->Unbind(proxy_packet_received);
    }
}

std::unique_lock<std::mutex> BSD_USA::LockService() noexcept {
    return {};
}

BSDCFG::BSDCFG(Core::System& system_, const char *name)
    : ServiceFramework{system_, name} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, nullptr, "SetIfUp"},
        {1, nullptr, "SetIfUpWithEvent"},
        {2, nullptr, "CancelIf"},
        {3, nullptr, "SetIfDown"},
        {4, nullptr, "GetIfState"},
        {5, nullptr, "DhcpRenew"},
        {6, nullptr, "AddStaticArpEntry"},
        {7, nullptr, "RemoveArpEntry"},
        {8, nullptr, "LookupArpEntry"},
        {9, nullptr, "LookupArpEntry2"},
        {10, nullptr, "ClearArpEntries"},
        {11, nullptr, "ClearArpEntries2"},
        {12, nullptr, "PrintArpEntries"},
        {13, nullptr, "Unknown13"},
        {14, nullptr, "Unknown14"},
        {15, nullptr, "Unknown15"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

BSDCFG::~BSDCFG() = default;

BSD_NU::BSD_NU(Core::System& system_)
    : ServiceFramework{system_, "bsd:nu"} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, nullptr, "CreateUserService"},
    };
    // clang-format on
    RegisterHandlers(functions);
}

BSD_NU::~BSD_NU() = default;

} // namespace Service::Sockets

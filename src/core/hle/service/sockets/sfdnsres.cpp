// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "common/fs/path_util.h"
#include "common/string_util.h"
#include "common/swap.h"
#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/sockets/sfdnsres.h"
#include "core/hle/service/sockets/sfdnsres_redirect.h"
#include "core/hle/service/sockets/sockets.h"
#include "core/hle/service/sockets/sockets_translate.h"
#include "core/hle/service/ssl/ssl_backend.h"
#include "core/internal_network/network.h"
#include "core/internal_network/sockets.h"
#include "core/memory.h"
#include "core/openpak/boot_connection.h"

namespace Service::Sockets {

namespace {

// One TLS handshake to `address`:443 through the guest's own TLS backend, verifying `name`.
void ProbeTls(const std::string& address, const std::string& name) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(address.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
        LOG_WARNING(Service, "net test: no IPv4 address to probe TLS against");
        return;
    }
    auto socket = std::make_shared<Network::Socket>();
    if (socket->Initialize(Network::Domain::INET, Network::Type::STREAM,
                           Network::Protocol::TCP) != Network::Errno::SUCCESS ||
        socket->Connect({Network::Domain::INET,
                         {static_cast<u8>(a), static_cast<u8>(b), static_cast<u8>(c),
                          static_cast<u8>(d)},
                         443}) != Network::Errno::SUCCESS) {
        LOG_WARNING(Service, "net test: could not connect to the console server on 443");
        return;
    }
    std::unique_ptr<SSL::SSLConnectionBackend> backend;
    if (SSL::CreateSSLConnectionBackend(&backend) != ResultSuccess) {
        return;
    }
    backend->SetSocket(socket);
    backend->SetHostName(name.c_str());
    const Result result = backend->DoHandshake();
    LOG_INFO(Service, "net test: TLS to the console server as {}: {}", name,
             result == ResultSuccess ? "verified" : "refused");
}

// Test only, for the harness, until the frontends call OpenPak::BootConnection::Run before boot
// (wizard stream): under the harness (HARBOR_CONTROL set) and with a `harbor-net-test` file in
// the user directory, the boot connection runs here, then one TLS handshake to the console server
// under a redirected name and one under a name its chain does not carry.
void TestBootConnection() {
    static std::once_flag once;
    std::call_once(once, [] {
        const auto user_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir);
        std::error_code ec;
        if (std::getenv("HARBOR_CONTROL") == nullptr ||
            !std::filesystem::exists(user_dir / "harbor-net-test", ec)) {
            return;
        }
        const auto result = OpenPak::BootConnection::Run(user_dir);
        LOG_INFO(Service, "net test: OpenPak {}, profile {} v{}, ceiling {} v{}",
                 result.enabled ? "on" : "off", result.profile_source, result.profile_version,
                 result.ceiling_source, result.ceiling_version);
        const auto address = OpenPak::Network::RedirectFor("dauth-lp1.ndas.srv.nintendo.net");
        if (!result.enabled || !address) {
            return;
        }
        std::thread([address = *address] {
            ProbeTls(address, "dauth-lp1.ndas.srv.nintendo.net");
            // The server mints a leaf for any name it is asked for (SNI), so a wrong name is the
            // address itself: no SNI goes out, and the default chain, rooted at the pinned CA,
            // carries no IP SAN.
            ProbeTls(address, address);
        }).detach();
    });
}

} // namespace

SFDNSRES::SFDNSRES(Core::System& system_) : ServiceFramework{system_, "sfdnsres"} {
    static const FunctionInfo functions[] = {
        {0, nullptr, "SetDnsAddressesPrivateRequest"},
        {1, nullptr, "GetDnsAddressPrivateRequest"},
        {2, &SFDNSRES::GetHostByNameRequest, "GetHostByNameRequest"},
        {3, nullptr, "GetHostByAddrRequest"},
        {4, nullptr, "GetHostStringErrorRequest"},
        {5, &SFDNSRES::GetGaiStringErrorRequest, "GetGaiStringErrorRequest"},
        {6, &SFDNSRES::GetAddrInfoRequest, "GetAddrInfoRequest"},
        {7, nullptr, "GetNameInfoRequest"},
        {8, nullptr, "RequestCancelHandleRequest"},
        {9, nullptr, "CancelRequest"},
        {10, &SFDNSRES::GetHostByNameRequestWithOptions, "GetHostByNameRequestWithOptions"},
        {11, nullptr, "GetHostByAddrRequestWithOptions"},
        {12, &SFDNSRES::GetAddrInfoRequestWithOptions, "GetAddrInfoRequestWithOptions"},
        {13, nullptr, "GetNameInfoRequestWithOptions"},
        {14, &SFDNSRES::ResolverSetOptionRequest, "ResolverSetOptionRequest"},
        {15, nullptr, "ResolverGetOptionRequest"},
        {16, &SFDNSRES::ReportTelemetry, "ReportTelemetry"},                         // 23.0.0+
        {100, &SFDNSRES::QueryAddrInfoRequestWithOptions, "QueryAddrInfoRequestWithOptions"}, // 23.0.0+
        {101, &SFDNSRES::QueryNameInfoRequestWithOptions, "QueryNameInfoRequestWithOptions"}, // 23.0.0+
        {102, &SFDNSRES::FetchAddrInfoRequestWithOptions, "FetchAddrInfoRequestWithOptions"}, // 23.0.0+
        {103, &SFDNSRES::FetchNameInfoRequestWithOptions, "FetchNameInfoRequestWithOptions"}, // 23.0.0+
        {104, &SFDNSRES::CancelQuery, "CancelQuery"},                                 // 23.0.0+
    };
    RegisterHandlers(functions);

    TestBootConnection();
    worker = std::thread([this] { WorkerLoop(); });
}

SFDNSRES::~SFDNSRES() {
    {
        std::scoped_lock lock{query_mutex};
        stop_worker = true;
    }
    query_cv.notify_all();
    if (worker.joinable()) {
        worker.join();
    }
}

enum class NetDbError : s32 {
    Internal = -1,
    Success = 0,
    HostNotFound = 1,
    TryAgain = 2,
    NoRecovery = 3,
    NoData = 4,
};

static NetDbError GetAddrInfoErrorToNetDbError(GetAddrInfoError result) {
    // These combinations have been verified on console (but are not
    // exhaustive).
    switch (result) {
    case GetAddrInfoError::SUCCESS:
        return NetDbError::Success;
    case GetAddrInfoError::AGAIN:
        return NetDbError::TryAgain;
    case GetAddrInfoError::NODATA:
        return NetDbError::HostNotFound;
    case GetAddrInfoError::SERVICE:
        return NetDbError::Success;
    default:
        return NetDbError::HostNotFound;
    }
}

static Errno GetAddrInfoErrorToErrno(GetAddrInfoError result) {
    // These combinations have been verified on console (but are not
    // exhaustive).
    switch (result) {
    case GetAddrInfoError::SUCCESS:
        // Note: Sometimes a successful lookup sets errno to EADDRNOTAVAIL for
        // some reason, but that doesn't seem useful to implement.
        return Errno::SUCCESS;
    case GetAddrInfoError::AGAIN:
        return Errno::SUCCESS;
    case GetAddrInfoError::NODATA:
        return Errno::SUCCESS;
    case GetAddrInfoError::SERVICE:
        return Errno::INVAL;
    default:
        return Errno::SUCCESS;
    }
}

template <typename T>
static void Append(std::vector<u8>& vec, T t) {
    const size_t offset = vec.size();
    vec.resize(offset + sizeof(T));
    std::memcpy(vec.data() + offset, &t, sizeof(T));
}

static void AppendNulTerminated(std::vector<u8>& vec, std::string_view str) {
    const size_t offset = vec.size();
    vec.resize(offset + str.size() + 1);
    std::memmove(vec.data() + offset, str.data(), str.size());
}

using LookupResult = std::variant<std::vector<Network::AddrInfo>, GetAddrInfoError>;

// The answer for a redirected name: one IPv4 entry, OpenPak's address, the guest's port.
static LookupResult RedirectAnswer(const std::string& address,
                                   const std::optional<std::string>& service) {
    u32 port = 0;
    if (service.has_value()) {
        for (const char c : *service) {
            if (c < '0' || c > '9') {
                return GetAddrInfoError::SERVICE;
            }
            port = port * 10 + static_cast<u32>(c - '0');
            if (port > 0xFFFF) {
                return GetAddrInfoError::SERVICE;
            }
        }
    }
    Network::IPv4Address ip{};
    unsigned parts[4]{};
    if (std::sscanf(address.c_str(), "%u.%u.%u.%u", &parts[0], &parts[1], &parts[2], &parts[3]) !=
        4) {
        return GetAddrInfoError::FAIL;
    }
    for (std::size_t i = 0; i < ip.size(); ++i) {
        ip[i] = static_cast<u8>(parts[i]);
    }
    Network::AddrInfo info{};
    info.family = Network::Domain::INET;
    info.socket_type = Network::Type::Unspecified;
    info.protocol = Network::Protocol::IP; // 0, as Type::Unspecified: any socket type
    info.addr = Network::SockAddrIn{.family = Network::Domain::INET, .ip = ip, .portno = static_cast<u16>(port)};
    return std::vector<Network::AddrInfo>{info};
}

// The decision for a name, without the host resolver: a redirect or a refusal is answered here,
// HostDns is left to the caller.
static std::optional<LookupResult> DecideLookup(const Dns::Decision& decision,
                                                const std::optional<std::string>& service) {
    switch (decision.verdict) {
    case Dns::Verdict::Redirect:
        LOG_INFO(Service, "sfdnsres: {} -> {} ({})", decision.host, decision.address,
                 decision.reason);
        return RedirectAnswer(decision.address, service);
    case Dns::Verdict::FailClosed:
        LOG_INFO(Service, "sfdnsres: {} fails closed ({}); the host resolver is not asked",
                 decision.host, decision.reason);
        return GetAddrInfoError::NODATA;
    case Dns::Verdict::HostDns:
    default:
        return std::nullopt;
    }
}

static LookupResult HostLookup(const std::string& host, const std::optional<std::string>& service) {
    LOG_DEBUG(Service, "sfdnsres: {} goes to the host resolver", host);
    auto res_v = Network::GetAddressInfo(host, service);
    if (auto* res = std::get_if<std::vector<Network::AddrInfo>>(&res_v)) {
        return std::move(*res);
    }
    return Translate(std::get<Network::GetAddrInfoError>(res_v));
}

// nsd first when the guest asked for it, then the decision, then the host resolver.
static LookupResult ResolveName(std::string host, const std::optional<std::string>& service,
                                bool use_nsd_resolve) {
    if (use_nsd_resolve) {
        host = Dns::NsdResolve(host);
    }
    const Dns::Decision decision = Dns::Decide(host);
    if (auto answer = DecideLookup(decision, service)) {
        return std::move(*answer);
    }
    return HostLookup(decision.host, service);
}

// We implement gethostbyname using the host's getaddrinfo rather than the
// host's gethostbyname, because it simplifies portability: e.g., getaddrinfo
// behaves the same on Unix and Windows, unlike gethostbyname where Windows
// doesn't implement h_errno.
static std::vector<u8> SerializeAddrInfoAsHostEnt(const std::vector<Network::AddrInfo>& vec,
                                                  std::string_view host) {

    std::vector<u8> data;
    // h_name: use the input hostname (append nul-terminated)
    AppendNulTerminated(data, host);
    // h_aliases: leave empty

    Append<u32_be>(data, 0); // count of h_aliases
    // (If the count were nonzero, the aliases would be appended as nul-terminated here.)
    Append<u16_be>(data, static_cast<u16>(Domain::INET)); // h_addrtype
    Append<u16_be>(data, sizeof(Network::IPv4Address));   // h_length
    // h_addr_list:
    size_t count = vec.size();
    ASSERT(count <= UINT32_MAX);
    Append<u32_be>(data, static_cast<uint32_t>(count));
    for (const Network::AddrInfo& addrinfo : vec) {
        // On the Switch, this is passed through htonl despite already being
        // big-endian, so it ends up as little-endian.
        Append<u32_le>(data, Network::IPv4AddressToInteger(addrinfo.addr.ip));

        LOG_INFO(Service, "Resolved host '{}' to IPv4 address {}", host,
                 Network::IPv4AddressToString(addrinfo.addr.ip));
    }
    return data;
}

static std::pair<u32, GetAddrInfoError> GetHostByNameRequestImpl(HLERequestContext& ctx) {
    struct InputParameters {
        u8 use_nsd_resolve;
        u32 cancel_handle;
        u64 process_id;
    };
    static_assert(sizeof(InputParameters) == 0x10);

    IPC::RequestParser rp{ctx};
    const auto parameters = rp.PopRaw<InputParameters>();

    const auto host_buffer = ctx.ReadBuffer(0);
    const std::string host = Common::StringFromBuffer(host_buffer);
    // For now, ignore options, which are in input buffer 1 for GetHostByNameRequestWithOptions.

    auto res_v = ResolveName(host, /*service*/ std::nullopt, (parameters.use_nsd_resolve & 1) != 0);
    if (auto* res = std::get_if<std::vector<Network::AddrInfo>>(&res_v)) {
        const std::vector<u8> data = SerializeAddrInfoAsHostEnt(*res, host);
        const u32 data_size = u32(data.size());
        ctx.WriteBuffer(data, 0);
        return {data_size, GetAddrInfoError::SUCCESS};
    }
    return {0, std::get<GetAddrInfoError>(res_v)};
}

void SFDNSRES::GetHostByNameRequest(HLERequestContext& ctx) {
    auto [data_size, emu_gai_err] = GetHostByNameRequestImpl(ctx);

    struct OutputParameters {
        NetDbError netdb_error;
        Errno bsd_errno;
        u32 data_size;
    };
    static_assert(sizeof(OutputParameters) == 0xc);

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.PushRaw(OutputParameters{
        .netdb_error = GetAddrInfoErrorToNetDbError(emu_gai_err),
        .bsd_errno = GetAddrInfoErrorToErrno(emu_gai_err),
        .data_size = data_size,
    });
}

void SFDNSRES::GetHostByNameRequestWithOptions(HLERequestContext& ctx) {
    auto [data_size, emu_gai_err] = GetHostByNameRequestImpl(ctx);

    struct OutputParameters {
        u32 data_size;
        NetDbError netdb_error;
        Errno bsd_errno;
    };
    static_assert(sizeof(OutputParameters) == 0xc);

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.PushRaw(OutputParameters{
        .data_size = data_size,
        .netdb_error = GetAddrInfoErrorToNetDbError(emu_gai_err),
        .bsd_errno = GetAddrInfoErrorToErrno(emu_gai_err),
    });
}

static std::vector<u8> SerializeAddrInfo(const std::vector<Network::AddrInfo>& vec,
                                         std::string_view host) {
    // Adapted from
    // https://github.com/switchbrew/libnx/blob/c5a9a909a91657a9818a3b7e18c9b91ff0cbb6e3/nx/source/runtime/resolver.c#L190
    std::vector<u8> data;

    for (const Network::AddrInfo& addrinfo : vec) {
        // serialized addrinfo:
        Append<u32_be>(data, 0xBEEFCAFE);                                        // magic
        Append<u32_be>(data, 0);                                                 // ai_flags
        Append<u32_be>(data, static_cast<u32>(Translate(addrinfo.family)));      // ai_family
        Append<u32_be>(data, static_cast<u32>(Translate(addrinfo.socket_type))); // ai_socktype
        Append<u32_be>(data, static_cast<u32>(Translate(addrinfo.protocol)));    // ai_protocol
        Append<u32_be>(data, 16); // ai_addrlen
        // ^ *not* sizeof(SerializedSockAddrIn), not that it matters since they're the same size

        // ai_addr:
        Append<u16_be>(data, static_cast<u16>(Translate(addrinfo.addr.family))); // sin_family
        // On the Switch, the following fields are passed through htonl despite
        // already being big-endian, so they end up as little-endian.
        Append<u16_le>(data, addrinfo.addr.portno);                            // sin_port
        Append<u32_le>(data, Network::IPv4AddressToInteger(addrinfo.addr.ip)); // sin_addr
        data.resize(data.size() + 8, 0);                                       // sin_zero

        if (addrinfo.canon_name.has_value()) {
            AppendNulTerminated(data, *addrinfo.canon_name);
        } else {
            data.push_back(0);
        }

        LOG_INFO(Service, "Resolved host '{}' to IPv4 address {}", host,
                 Network::IPv4AddressToString(addrinfo.addr.ip));
    }

    data.resize(data.size() + 4, 0); // 4-byte sentinel value

    return data;
}

static std::pair<u32, GetAddrInfoError> GetAddrInfoRequestImpl(HLERequestContext& ctx) {
    struct InputParameters {
        u8 use_nsd_resolve;
        u32 cancel_handle;
        u64 process_id;
    };
    static_assert(sizeof(InputParameters) == 0x10);

    IPC::RequestParser rp{ctx};
    const auto parameters = rp.PopRaw<InputParameters>();

    const auto host_buffer = ctx.ReadBuffer(0);
    const std::string host = Common::StringFromBuffer(host_buffer);

    std::optional<std::string> service = std::nullopt;
    if (ctx.CanReadBuffer(1)) {
        const std::span<const u8> service_buffer = ctx.ReadBuffer(1);
        service = Common::StringFromBuffer(service_buffer);
        if (service->empty()) {
            service = std::nullopt;
        }
    }

    // Serialized hints are also passed in a buffer, but are ignored for now.

    auto res_v = ResolveName(host, service, (parameters.use_nsd_resolve & 1) != 0);
    if (auto* res = std::get_if<std::vector<Network::AddrInfo>>(&res_v)) {
        const std::vector<u8> data = SerializeAddrInfo(*res, host);
        const u32 data_size = u32(data.size());
        ctx.WriteBuffer(data, 0);
        return {data_size, GetAddrInfoError::SUCCESS};
    }
    return {0, std::get<GetAddrInfoError>(res_v)};
}

void SFDNSRES::GetAddrInfoRequest(HLERequestContext& ctx) {
    auto [data_size, emu_gai_err] = GetAddrInfoRequestImpl(ctx);

    struct OutputParameters {
        Errno bsd_errno;
        GetAddrInfoError gai_error;
        u32 data_size;
    };
    static_assert(sizeof(OutputParameters) == 0xc);

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.PushRaw(OutputParameters{
        .bsd_errno = GetAddrInfoErrorToErrno(emu_gai_err),
        .gai_error = emu_gai_err,
        .data_size = data_size,
    });
}

void SFDNSRES::GetGaiStringErrorRequest(HLERequestContext& ctx) {
    struct InputParameters {
        GetAddrInfoError gai_errno;
    };
    IPC::RequestParser rp{ctx};
    auto input = rp.PopRaw<InputParameters>();

    const std::string result = Translate(input.gai_errno);
    ctx.WriteBuffer(result);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void SFDNSRES::GetAddrInfoRequestWithOptions(HLERequestContext& ctx) {
    // Additional options are ignored
    auto [data_size, emu_gai_err] = GetAddrInfoRequestImpl(ctx);

    struct OutputParameters {
        u32 data_size;
        GetAddrInfoError gai_error;
        NetDbError netdb_error;
        Errno bsd_errno;
    };
    static_assert(sizeof(OutputParameters) == 0x10);

    IPC::ResponseBuilder rb{ctx, 6};
    rb.Push(ResultSuccess);
    rb.PushRaw(OutputParameters{
        .data_size = data_size,
        .gai_error = emu_gai_err,
        .netdb_error = GetAddrInfoErrorToNetDbError(emu_gai_err),
        .bsd_errno = GetAddrInfoErrorToErrno(emu_gai_err),
    });
}

void SFDNSRES::ResolverSetOptionRequest(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 3};

    rb.Push(ResultSuccess);
    rb.Push<s32>(0); // bsd errno
}

// ---------------------------------------------------------------------------------------------
// The asynchronous resolver of firmware 23.0.0+. SDK 23.3 clients (account, friends, bcat, ns,
// npns, eupld; their libcurl resolves through it) submit with 100 and name an eventfd of their own
// bsd session, select() on that eventfd, and only then fetch with 102 or give up with 104.
// Layouts: the Ryujinx Harbor (friends 23.0.1 client marshallers) and Atmosphere's dns_mitm
// (f5cd91260) agree, and the console capture (dnsmitm/capture, 475 requests from account, bcat
// and 0x25) shows 100's raw input as {fd 0, options version 13, option count 2, pad, pid 0} with
// two 0x14-byte option entries. The answer is decided at once for redirected and refused names;
// a host lookup runs on the resolver worker, never on the IPC thread. Behaviour follows the
// Ryujinx Harbor (MIT-licensed Ryujinx base).

void SFDNSRES::SetEventFdSignal(EventFdSignal signal) {
    std::scoped_lock lock{EventFdMutex()};
    EventFdStorage() = std::move(signal);
}

void SFDNSRES::SignalEventFd(u64 process_id, s32 fd) {
    EventFdSignal signal;
    {
        std::scoped_lock lock{EventFdMutex()};
        signal = EventFdStorage();
    }
    if (!signal || !signal(process_id, fd)) {
        LOG_WARNING(Service, "sfdnsres: eventfd {} of process {} could not be signalled; the "
                             "caller will time out",
                    fd, process_id);
    }
}

static u64 CallerProcessId(HLERequestContext& ctx) {
    auto* const process = ctx.GetThread().GetOwnerProcess();
    return process != nullptr ? process->GetProcessId() : ctx.GetPID();
}

u32 SFDNSRES::NewQueryHandle() {
    // Under query_mutex.
    do {
        ++next_query;
    } while (next_query == 0 || queries.contains(next_query));
    return next_query;
}

void SFDNSRES::WorkerLoop() {
    for (;;) {
        PendingLookup job;
        {
            std::unique_lock lock{query_mutex};
            query_cv.wait(lock, [this] { return stop_worker || !pending.empty(); });
            if (stop_worker) {
                return;
            }
            job = std::move(pending.front());
            pending.pop_front();
        }

        LookupResult result = HostLookup(job.host, job.service);

        bool signal = false;
        {
            std::scoped_lock lock{query_mutex};
            const auto it = queries.find(job.handle);
            // Cancelled meanwhile: nobody waits for it.
            if (it != queries.end()) {
                it->second.result = std::move(result);
                it->second.ready = true;
                signal = true;
            }
        }
        if (signal) {
            SignalEventFd(job.process_id, job.event_fd);
        }
    }
}

void SFDNSRES::ReportTelemetry(HLERequestContext& ctx) {
    // Three resolver failure counters, sent only when one is non-zero. Nothing to report to.
    LOG_DEBUG(Service, "called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void SFDNSRES::QueryAddrInfoRequestWithOptions(HLERequestContext& ctx) {
    struct InputParameters {
        s32 event_fd;
        u32 options_version;
        u32 option_count;
        INSERT_PADDING_WORDS(1);
        u64 process_id;
    };
    static_assert(sizeof(InputParameters) == 0x18);

    IPC::RequestParser rp{ctx};
    const auto parameters = rp.PopRaw<InputParameters>();

    const std::string host = Common::StringFromBuffer(ctx.ReadBuffer(0));
    std::optional<std::string> service;
    if (ctx.CanReadBuffer(1) && ctx.GetReadBufferSize(1) != 0) {
        service = Common::StringFromBuffer(ctx.ReadBuffer(1));
        if (service->empty()) {
            service = std::nullopt;
        }
    }
    // The hints (buffer 2) and the options (buffer 3; keys 0x7531 and 0x7533 seen) are not
    // applied, as for 6 and 12.

    const u64 process_id = CallerProcessId(ctx);
    const Dns::Decision decision = Dns::Decide(host);
    std::optional<LookupResult> answer = DecideLookup(decision, service);

    u32 handle;
    {
        std::scoped_lock lock{query_mutex};
        handle = NewQueryHandle();
        Query& query = queries[handle];
        query.host = decision.host;
        if (answer.has_value()) {
            query.result = std::move(*answer);
            query.ready = true;
        } else {
            pending.push_back(PendingLookup{
                .handle = handle,
                .host = decision.host,
                .service = service,
                .process_id = process_id,
                .event_fd = parameters.event_fd,
            });
        }
    }

    if (answer.has_value()) {
        SignalEventFd(process_id, parameters.event_fd);
    } else {
        query_cv.notify_one();
    }

    struct OutputParameters {
        u32 query_handle;
        s32 ret;
        Errno bsd_errno;
    };
    static_assert(sizeof(OutputParameters) == 0xc);

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.PushRaw(OutputParameters{
        .query_handle = handle,
        .ret = 0,
        .bsd_errno = Errno::SUCCESS,
    });
}

void SFDNSRES::FetchAddrInfoRequestWithOptions(HLERequestContext& ctx) {
    struct InputParameters {
        u32 query_handle;
        u32 options_version;
        u64 process_id;
    };
    static_assert(sizeof(InputParameters) == 0x10);

    IPC::RequestParser rp{ctx};
    const auto parameters = rp.PopRaw<InputParameters>();

    struct OutputParameters {
        u32 data_size;
        GetAddrInfoError gai_error;
        Errno bsd_errno;
    };
    static_assert(sizeof(OutputParameters) == 0xc);

    std::optional<Query> query;
    {
        std::scoped_lock lock{query_mutex};
        const auto it = queries.find(parameters.query_handle);
        if (it != queries.end() && it->second.ready) {
            query = std::move(it->second);
            queries.erase(it);
        } else if (it != queries.end()) {
            query = Query{};
        }
    }

    OutputParameters out{.data_size = 0, .gai_error = GetAddrInfoError::FAIL,
                         .bsd_errno = Errno::SUCCESS};
    if (!query.has_value()) {
        LOG_WARNING(Service, "unknown contract: sfdnsres 102 for unknown query {}; answered "
                             "EAI_FAIL",
                    parameters.query_handle);
    } else if (!query->ready) {
        // The caller fetched before its eventfd fired.
        LOG_WARNING(Service, "unknown contract: sfdnsres 102 before query {} completed; answered "
                             "EAI_AGAIN",
                    parameters.query_handle);
        out.gai_error = GetAddrInfoError::AGAIN;
    } else if (auto* res = std::get_if<std::vector<Network::AddrInfo>>(&query->result)) {
        const std::vector<u8> data = SerializeAddrInfo(*res, query->host);
        out.data_size = static_cast<u32>(ctx.WriteBuffer(data, 0));
        out.gai_error = GetAddrInfoError::SUCCESS;
    } else {
        out.gai_error = std::get<GetAddrInfoError>(query->result);
        out.bsd_errno = GetAddrInfoErrorToErrno(out.gai_error);
    }

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.PushRaw(out);
}

void SFDNSRES::CancelQuery(HLERequestContext& ctx) {
    struct InputParameters {
        u32 query_handle;
        INSERT_PADDING_WORDS(1);
        u64 process_id;
    };
    static_assert(sizeof(InputParameters) == 0x10);

    IPC::RequestParser rp{ctx};
    const auto parameters = rp.PopRaw<InputParameters>();
    {
        std::scoped_lock lock{query_mutex};
        queries.erase(parameters.query_handle);
    }

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

// The name-info pair (getnameinfo, address to name). No client in the 23.0.1 boot uses it, and a
// reverse lookup of an arbitrary address would go to the host resolver, so the submit completes
// at once and the fetch answers EAI_NONAME. The raw layout is assumed to match 100's (eventfd
// first) from the buffer table (in 0x18, out 0x0c; fetch in 0x10, out 0x08).
void SFDNSRES::QueryNameInfoRequestWithOptions(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 event_fd = rp.Pop<s32>();

    LOG_WARNING(Service, "unknown contract: sfdnsres 101 QueryNameInfoRequestWithOptions, "
                         "answered EAI_NONAME at fetch");

    u32 handle;
    {
        std::scoped_lock lock{query_mutex};
        handle = NewQueryHandle();
        Query& query = queries[handle];
        query.result = GetAddrInfoError::NONAME;
        query.ready = true;
        query.name_info = true;
    }
    SignalEventFd(CallerProcessId(ctx), event_fd);

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.Push<u32>(handle);
    rb.Push<s32>(0);
    rb.Push<u32>(0);
}

void SFDNSRES::FetchNameInfoRequestWithOptions(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const u32 handle = rp.Pop<u32>();

    LOG_WARNING(Service, "unknown contract: sfdnsres 103 FetchNameInfoRequestWithOptions, "
                         "answered EAI_NONAME");
    {
        std::scoped_lock lock{query_mutex};
        queries.erase(handle);
    }

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.PushEnum(GetAddrInfoError::NONAME);
    rb.Push<u32>(0);
}


DNS_PRIV::DNS_PRIV(Core::System& system_)
    : ServiceFramework{system_, "dns:priv"} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, nullptr, "Cmd0"},
        {1, nullptr, "Cmd1"},
        {2, nullptr, "Cmd2"},
    };
    // clang-format on
    RegisterHandlers(functions);
}

DNS_PRIV::~DNS_PRIV() = default;


} // namespace Service::Sockets

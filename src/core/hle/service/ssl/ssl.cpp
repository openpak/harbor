// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstring>

#include "common/string_util.h"

#include "core/core.h"
#include "core/hle/result.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/service.h"
#include "core/hle/service/sm/sm.h"
#include "core/hle/service/sockets/bsd.h"
#include "core/hle/service/ssl/cert_store.h"
#include "core/hle/service/ssl/ssl.h"
#include "core/hle/service/ssl/ssl_backend.h"
#include "core/internal_network/network.h"
#include "core/internal_network/sockets.h"

namespace Service::SSL {

// This is nn::ssl::sf::CertificateFormat
enum class CertificateFormat : u32 {
    Pem = 1,
    Der = 2,
};

// This is nn::ssl::sf::ContextOption
enum class ContextOption : u32 {
    None = 0,
    CrlImportDateCheckEnable = 1,
};

// This is nn::ssl::Connection::IoMode
enum class IoMode : u32 {
    Blocking = 1,
    NonBlocking = 2,
};

// This is nn::ssl::sf::OptionType
enum class OptionType : u32 {
    DoNotCloseSocket = 0,
    GetServerCertChain = 1,
    SkipDefaultVerify = 2,
    EnableAlpn = 3,
};

// This is nn::ssl::sf::RenegotiationMode
enum RenegotiationMode : u32 {
    None = 0, ///< None
    Secure = 1, ///< Secure
};

// This is nn::ssl::sf::SslVersion
struct SslVersion {
    union {
        u32 raw{};

        BitField<0, 1, u32> tls_auto;
        BitField<3, 1, u32> tls_v10;
        BitField<4, 1, u32> tls_v11;
        BitField<5, 1, u32> tls_v12;
        BitField<6, 1, u32> tls_v13;
        BitField<24, 7, u32> api_version;
    };
};

struct SslContextSharedData {
    u32 connection_count = 0;
};

class ISslConnection final : public ServiceFramework<ISslConnection> {
public:
    explicit ISslConnection(Core::System& system_in, SslVersion ssl_version_in,
                            std::shared_ptr<SslContextSharedData>& shared_data_in,
                            std::unique_ptr<SSLConnectionBackend>&& backend_in)
        : ServiceFramework{system_in, "ISslConnection"}, ssl_version{ssl_version_in},
          shared_data{shared_data_in}, backend{std::move(backend_in)} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, D<&ISslConnection::SetSocketDescriptor>, "SetSocketDescriptor"},
            {1, D<&ISslConnection::SetHostName>, "SetHostName"},
            {2, D<&ISslConnection::SetVerifyOption>, "SetVerifyOption"},
            {3, D<&ISslConnection::SetIoMode>, "SetIoMode"},
            {4, D<&ISslConnection::GetSocketDescriptor>, "GetSocketDescriptor"},
            {5, D<&ISslConnection::GetHostName>, "GetHostName"},
            {6, D<&ISslConnection::GetVerifyOption>, "GetVerifyOption"},
            {7, D<&ISslConnection::GetIoMode>, "GetIoMode"},
            {8, D<&ISslConnection::DoHandshake>, "DoHandshake"},
            {9, &ISslConnection::DoHandshakeGetServerCert, "DoHandshakeGetServerCert"},
            {10, D<&ISslConnection::Read>, "Read"},
            {11, D<&ISslConnection::Write>, "Write"},
            {12, D<&ISslConnection::Pending>, "Pending"},
            {13, D<&ISslConnection::Peek>, "Peek"},
            {14, D<&ISslConnection::Poll>, "Poll"},
            {15, D<&ISslConnection::GetVerifyCertError>, "GetVerifyCertError"},
            {16, D<&ISslConnection::GetNeededServerCertBufferSize>, "GetNeededServerCertBufferSize"},
            {17, D<&ISslConnection::SetSessionCacheMode>, "SetSessionCacheMode"},
            {18, D<&ISslConnection::GetSessionCacheMode>, "GetSessionCacheMode"},
            {19, D<&ISslConnection::FlushSessionCache>, "FlushSessionCache"},
            {20, D<&ISslConnection::SetRenegotiationMode>, "SetRenegotiationMode"},
            {21, D<&ISslConnection::GetRenegotiationMode>, "GetRenegotiationMode"},
            {22, D<&ISslConnection::SetOption>, "SetOption"},
            {23, D<&ISslConnection::GetOption>, "GetOption"},
            {24, D<&ISslConnection::GetVerifyCertErrors>, "GetVerifyCertErrors"},
            {25, D<&ISslConnection::GetCipherInfo>, "GetCipherInfo"},
            {26, D<&ISslConnection::SetNextAlpnProto>, "SetNextAlpnProto"},
            {27, D<&ISslConnection::GetNextAlpnProto>, "GetNextAlpnProto"},
            {28, D<&ISslConnection::SetDtlsSocketDescriptor>, "SetDtlsSocketDescriptor"},
            {29, D<&ISslConnection::GetDtlsHandshakeTimeout>, "GetDtlsHandshakeTimeout"},
            {30, D<&ISslConnection::SetPrivateOption>, "SetPrivateOption"},
            {31, D<&ISslConnection::SetSrtpCiphers>, "SetSrtpCiphers"},
            {32, D<&ISslConnection::GetSrtpCipher>, "GetSrtpCipher"},
            {33, D<&ISslConnection::ExportKeyingMaterial>, "ExportKeyingMaterial"},
            {34, D<&ISslConnection::SetIoTimeout>, "SetIoTimeout"},
            {35, D<&ISslConnection::GetIoTimeout>, "GetIoTimeout"},
        };
        // clang-format on

        RegisterHandlers(functions);

        backend->SetVerifyOption(verify_option);

        shared_data->connection_count++;
    }

    ~ISslConnection() {
        shared_data->connection_count--;
        // The duplicate goes with the connection, unless the guest set DoNotCloseSocket: it keeps
        // the descriptor and may dial again on it, and closing the duplicate would close the
        // socket both share.
        if (fd_to_close.has_value() && !do_not_close_socket) {
            auto bsd = system.ServiceManager().GetService<Service::Sockets::BSD_USA>("bsd:u");
            if (bsd) {
                auto err = bsd->CloseImpl(*fd_to_close);
                if (err != Service::Sockets::Errno::SUCCESS) {
                    LOG_ERROR(Service_SSL, "Failed to close duplicated socket: {}", err);
                }
            }
        }
    }

private:
    Result DoHandshakeImpl() {
        ASSERT_OR_EXECUTE(!did_handshake && socket, { return ResultNoSocket; });
        if (!alpn_offered && !next_alpn_proto.empty()) {
            backend->SetAlpnProtos(next_alpn_proto);
            alpn_offered = true;
        }
        Result res = backend->DoHandshake();
        did_handshake = res.IsSuccess();
        // What the server chose is kept apart from what was offered: overwriting the offered list
        // with "h2" leaves a later handshake offering nothing, which a gRPC server refuses.
        if (did_handshake) {
            negotiated_alpn_proto = backend->GetNegotiatedAlpnProto();
        }
        return res;
    }

    std::vector<u8> SerializeServerCerts(const std::vector<std::vector<u8>>& certs) {
        struct Header {
            u64 magic;
            u32 count;
            u32 pad;
        };
        struct EntryHeader {
            u32 size;
            u32 offset;
        };
        if (!get_server_cert_chain) {
            // Just return the first one, unencoded.
            ASSERT_OR_EXECUTE_MSG(!certs.empty(), { return {}; }, "Should be at least one server cert");
            return certs[0];
        }
        std::vector<u8> ret;
        Header header{0x4E4D684374726543, u32(certs.size()), 0};
        ret.insert(ret.end(), reinterpret_cast<u8*>(&header), reinterpret_cast<u8*>(&header + 1));
        size_t data_offset = sizeof(Header) + certs.size() * sizeof(EntryHeader);
        for (auto& cert : certs) {
            EntryHeader entry_header{u32(cert.size()), u32(data_offset)};
            data_offset += cert.size();
            ret.insert(ret.end(), reinterpret_cast<u8*>(&entry_header), reinterpret_cast<u8*>(&entry_header + 1));
        }
        for (auto& cert : certs) {
            ret.insert(ret.end(), cert.begin(), cert.end());
        }
        return ret;
    }

    Result SetSocketDescriptor(s32 in_fd, Out<s32> out_fd) {
        LOG_DEBUG(Service_SSL, "called, fd={}", in_fd);
        ASSERT(!did_handshake);
        auto bsd = system.ServiceManager().GetService<Service::Sockets::BSD_USA>("bsd:u");
        ASSERT_OR_EXECUTE(bsd, { return ResultInternalError; });

        // The connection works on its own duplicate. The guest gets its descriptor back, or -1
        // when it set DoNotCloseSocket (switchbrew, libnx, the Ryujinx Harbor).
        auto const res_v = bsd->DuplicateSocketImpl(in_fd);
        if (auto *res = std::get_if<s32>(&res_v)) {
            const s32 dup_fd = *res;
            *out_fd = do_not_close_socket ? -1 : in_fd;
            fd_to_close = dup_fd;
            auto const sock = bsd->GetSocket(dup_fd);
            if (!sock.has_value()) {
                LOG_ERROR(Service_SSL, "invalid socket fd {} after duplication", dup_fd);
                return ResultInvalidSocket;
            }
            // Shared with the backend: SetIoMode and the handshake still need it here.
            socket = std::move(*sock);
            backend->SetSocket(socket);
            return ResultSuccess;
        }
        LOG_ERROR(Service_SSL, "Failed to duplicate socket with fd {}", in_fd);
        return ResultInvalidSocket;
    }

    Result SetHostName(InBuffer<BufferAttr_HipcMapAlias> buf) {
        auto const hostname = Common::StringFromBuffer(buf);
        LOG_DEBUG(Service_SSL, "called. hostname={}", hostname);
        ASSERT(!did_handshake);
        return backend->SetHostName(hostname.c_str());
    }

    // Kept so GetVerifyOption reads it back. The host verifies every connection regardless,
    // ceiling families included: a guest that asks for no verification is not given it.
    Result SetVerifyOption(u32 option) {
        LOG_DEBUG(Service_SSL, "called. option={}", option);
        ASSERT(!did_handshake);
        verify_option = option;
        backend->SetVerifyOption(option);
        R_SUCCEED();
    }

    Result GetVerifyOption(Out<u32> out_option) {
        *out_option = verify_option;
        R_SUCCEED();
    }

    Result SetIoMode(u32 input_mode) {
        auto mode = IoMode(input_mode);
        ASSERT(mode == IoMode::Blocking || mode == IoMode::NonBlocking);
        R_UNLESS(socket, ResultNoSocket);
        io_mode = mode;
        const bool non_block = mode == IoMode::NonBlocking;
        const Network::Errno error = socket->SetNonBlock(non_block);
        if (error != Network::Errno::SUCCESS) {
            LOG_ERROR(Service_SSL, "Failed to set native socket non-block flag to {}", non_block);
        }
        R_SUCCEED();
    }

    // The guest's descriptor for the connection's socket: the duplicate, never the host's.
    Result GetSocketDescriptor(Out<s32> out_fd) {
        *out_fd = fd_to_close.value_or(-1);
        R_SUCCEED();
    }

    Result GetHostName(OutBuffer<BufferAttr_HipcMapAlias> data, Out<u32> out_size) {
        LOG_WARNING(Service_SSL, "(STUBBED)");
        ASSERT(!did_handshake);
        return backend->GetHostName(std::span<u8>{data.begin(), data.end()}, out_size);
    }

    Result GetIoMode(Out<u32> out_mode) {
        *out_mode = u32(io_mode);
        R_SUCCEED();
    }

    Result DoHandshake() {
        return DoHandshakeImpl();
    }

    void DoHandshakeGetServerCert(HLERequestContext& ctx) {
        struct OutputParameters {
            u32 certs_size;
            u32 certs_count;
        };
        static_assert(sizeof(OutputParameters) == 0x8);

        Result res = DoHandshakeImpl();
        OutputParameters out{};
        if (res == ResultSuccess) {
            std::vector<std::vector<u8>> certs;
            res = backend->GetServerCerts(&certs);
            if (res == ResultSuccess) {
                const std::vector<u8> certs_buf = SerializeServerCerts(certs);
                if (ctx.CanWriteBuffer()) {
                    const size_t buffer_size = ctx.GetWriteBufferSize();
                    if (certs_buf.size() <= buffer_size) {
                        ctx.WriteBuffer(certs_buf);
                    } else {
                        LOG_WARNING(Service_SSL, "Certificate buffer too small: {} bytes needed, {} bytes available",
                                    certs_buf.size(), buffer_size);
                        ctx.WriteBuffer(std::span<const u8>(certs_buf.data(), buffer_size));
                    }
                } else {
                    LOG_DEBUG(Service_SSL, "No output buffer provided for certificates ({} bytes)", certs_buf.size());
                }

                out.certs_count = static_cast<u32>(certs.size());
                out.certs_size = static_cast<u32>(certs_buf.size());
            }
        }
        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(res);
        rb.PushRaw(out);
    }

    Result Read(OutBuffer<BufferAttr_HipcMapAlias> data, Out<u32> out_size) {
        R_UNLESS(did_handshake, ResultInternalError);
        size_t tmp{};
        auto const res = backend->Read(&tmp, data);
        *out_size = u32(tmp);
        return res;
    }

    Result Write(InBuffer<BufferAttr_HipcMapAlias> data, Out<u32> out_size) {
        R_UNLESS(did_handshake, ResultInternalError);
        size_t tmp{};
        auto const res = backend->Write(&tmp, data);
        *out_size = u32(tmp);
        return res;
    }

    Result Pending(Out<s32> out_pending_size) {
        *out_pending_size = s32(backend->Pending());
        R_SUCCEED();
    }

    // Moving Out 2's websocket peeks every 5 ms as its readiness check: no log per call.
    Result Peek(OutBuffer<BufferAttr_HipcMapAlias> data, Out<u32> out_size) {
        R_UNLESS(did_handshake, ResultInternalError);
        size_t tmp{};
        auto const res = backend->Peek(&tmp, data);
        *out_size = u32(tmp);
        return res;
    }

    Result Poll(u32 in_pollevent, u32 timer, Out<u32> out_pollevent) {
        LOG_WARNING(Service_SSL, "(STUBBED) called, no events reported");
        *out_pollevent = 0;
        R_SUCCEED();
    }

    Result GetVerifyCertError() {
        LOG_WARNING(Service_SSL, "(STUBBED)");
        R_SUCCEED();
    }

    Result GetNeededServerCertBufferSize(Out<u32> out_needed_buffer_size) {
        LOG_WARNING(Service_SSL, "(STUBBED)");
        R_SUCCEED();
    }

    Result SetSessionCacheMode(u32 mode) {
        LOG_WARNING(Service_SSL, "(STUBBED) called. value={}", mode);
        R_UNLESS(!did_handshake, ResultInternalError);
        R_SUCCEED();
    }

    Result GetSessionCacheMode(Out<u32> mode) {
        LOG_WARNING(Service_SSL, "(STUBBED)");
        R_UNLESS(!did_handshake, ResultInternalError);
        R_SUCCEED();
    }

    Result FlushSessionCache() {
        LOG_WARNING(Service_SSL, "(STUBBED)");
        R_UNLESS(!did_handshake, ResultInternalError);
        R_SUCCEED();
    }

    Result SetRenegotiationMode(RenegotiationMode mode) {
        LOG_WARNING(Service_SSL, "(STUBBED)");
        backend->SetRenegotiationMode(u32(mode));
        R_SUCCEED();
    }

    Result GetRenegotiationMode(Out<RenegotiationMode> mode) {
        LOG_WARNING(Service_SSL, "(STUBBED)");
        u32 tmp{};
        auto const res = backend->GetRenegotiationMode(&tmp);
        *mode = RenegotiationMode(tmp);
        return res;
    }

    // nn::ssl: the flag comes first, then the option (libnx, the Ryujinx Harbor).
    Result SetOption(bool value, OptionType option) {
        switch (option) {
        case OptionType::DoNotCloseSocket:
            do_not_close_socket = bool(value);
            break;
        case OptionType::GetServerCertChain:
            get_server_cert_chain = bool(value);
            break;
        case OptionType::SkipDefaultVerify:
            skip_default_verify = bool(value);
            break;
        case OptionType::EnableAlpn:
            enable_alpn = bool(value);
            break;
        default:
            LOG_WARNING(Service_SSL, "Unknown option={}, value={}", u32(option), value);
        }
        R_SUCCEED();
    }

    Result GetOption(OptionType option, Out<u8> value) {
        switch (option) {
        case OptionType::DoNotCloseSocket:
            *value = u8(do_not_close_socket);
            break;
        case OptionType::GetServerCertChain:
            *value = u8(get_server_cert_chain);
            break;
        case OptionType::SkipDefaultVerify:
            *value = u8(skip_default_verify);
            break;
        case OptionType::EnableAlpn:
            *value = u8(enable_alpn);
            break;
        default:
            LOG_WARNING(Service_SSL, "Unknown option={}", u32(option));
            *value = 0;
            break;
        }
        LOG_DEBUG(Service_SSL, "GetOption called, option={}, ret value={}", u32(option), *value);
        R_SUCCEED();
    }

    // The length-prefixed list the guest offers, applied at the handshake.
    Result SetNextAlpnProto(InBuffer<BufferAttr_HipcMapAlias> data) {
        next_alpn_proto.assign(data.begin(), data.end());
        alpn_offered = false;
        LOG_DEBUG(Service_SSL, "SetNextAlpnProto called, size={}", next_alpn_proto.size());
        R_SUCCEED();
    }

    // The negotiated protocol once there is one; before the handshake, what was offered.
    Result GetNextAlpnProto(OutBuffer<BufferAttr_HipcMapAlias> data, Out<u32> to_write) {
        const auto& source = negotiated_alpn_proto.empty() ? next_alpn_proto : negotiated_alpn_proto;
        *to_write = u32((std::min)(source.size(), data.size()));
        std::memcpy(data.data(), source.data(), *to_write);
        LOG_DEBUG(Service_SSL, "GetNextAlpnProto called, size={}", *to_write);
        R_SUCCEED();
    }

    // A failed verification ends the handshake, so a connection the guest can still ask about has
    // no certificate errors.
    Result GetVerifyCertErrors(OutBuffer<BufferAttr_HipcMapAlias> out_errors, Out<u32> out_count,
                               Out<u32> out_total) {
        LOG_WARNING(Service_SSL, "unknown contract: ssl ISslConnection 24 GetVerifyCertErrors, "
                                 "answered no errors");
        *out_count = 0;
        *out_total = 0;
        R_SUCCEED();
    }

    // nn::ssl::sf::CipherInfo: the cipher name, then the protocol version, both NUL-terminated.
    Result GetCipherInfo(u32 version, OutBuffer<BufferAttr_HipcMapAlias> out_info) {
        struct CipherInfo {
            std::array<char, 0x40> cipher_name;
            std::array<char, 0x8> protocol_version;
        };
        static_assert(sizeof(CipherInfo) == 0x48);
        R_UNLESS(did_handshake, ResultInternalError);
        std::string name;
        std::string protocol;
        R_TRY(backend->GetCipherInfo(&name, &protocol));
        CipherInfo info{};
        std::memcpy(info.cipher_name.data(), name.data(),
                    (std::min)(name.size(), info.cipher_name.size() - 1));
        std::memcpy(info.protocol_version.data(), protocol.data(),
                    (std::min)(protocol.size(), info.protocol_version.size() - 1));
        std::memcpy(out_info.data(), &info, (std::min)(out_info.size(), sizeof(info)));
        R_SUCCEED();
    }

    // DTLS, SRTP, private options and keying material: no title is known to use them. Answered
    // as the old fork's stubs (from Citron) did; each is an unknown contract.
    Result SetDtlsSocketDescriptor(s32 fd, Out<s32> out_fd) {
        LOG_WARNING(Service_SSL, "unknown contract: ssl ISslConnection 28 SetDtlsSocketDescriptor");
        *out_fd = -1;
        R_SUCCEED();
    }

    Result GetDtlsHandshakeTimeout(Out<u64> out_timeout_ns) {
        LOG_WARNING(Service_SSL, "unknown contract: ssl ISslConnection 29 GetDtlsHandshakeTimeout");
        *out_timeout_ns = 10'000'000'000ULL;
        R_SUCCEED();
    }

    Result SetPrivateOption(u32 option, u32 value) {
        LOG_WARNING(Service_SSL,
                    "unknown contract: ssl ISslConnection 30 SetPrivateOption option={} value={}",
                    option, value);
        R_SUCCEED();
    }

    Result SetSrtpCiphers(InBuffer<BufferAttr_HipcMapAlias> ciphers) {
        LOG_WARNING(Service_SSL, "unknown contract: ssl ISslConnection 31 SetSrtpCiphers");
        R_SUCCEED();
    }

    Result GetSrtpCipher(Out<u16> out_cipher) {
        LOG_WARNING(Service_SSL, "unknown contract: ssl ISslConnection 32 GetSrtpCipher");
        *out_cipher = 0;
        R_SUCCEED();
    }

    Result ExportKeyingMaterial(OutBuffer<BufferAttr_HipcMapAlias> out_material) {
        LOG_WARNING(Service_SSL, "unknown contract: ssl ISslConnection 33 ExportKeyingMaterial");
        std::memset(out_material.data(), 0, out_material.size());
        R_SUCCEED();
    }

    Result SetIoTimeout(u32 timeout) {
        LOG_WARNING(Service_SSL, "unknown contract: ssl ISslConnection 34 SetIoTimeout {}", timeout);
        io_timeout = timeout;
        R_SUCCEED();
    }

    Result GetIoTimeout(Out<u32> out_timeout) {
        LOG_WARNING(Service_SSL, "unknown contract: ssl ISslConnection 35 GetIoTimeout");
        *out_timeout = io_timeout;
        R_SUCCEED();
    }

    SslVersion ssl_version;
    std::shared_ptr<SslContextSharedData> shared_data;
    std::unique_ptr<SSLConnectionBackend> backend;
    std::optional<int> fd_to_close;
    std::shared_ptr<Network::SocketBase> socket;
    std::vector<u8> next_alpn_proto;       ///< The length-prefixed list the guest offers.
    std::vector<u8> negotiated_alpn_proto; ///< What the server picked, read back by the guest.
    // nn::ssl's default is PeerCa | HostName; the host verifies regardless.
    u32 verify_option = 3;
    u32 io_timeout = 0;
    IoMode io_mode = IoMode::Blocking;
    bool alpn_offered = false;

    bool do_not_close_socket = false;
    bool get_server_cert_chain = false;
    bool skip_default_verify = false;
    bool enable_alpn = false;
    bool did_handshake = false;
};

class ISslContext final : public ServiceFramework<ISslContext> {
public:
    explicit ISslContext(Core::System& system_, SslVersion version)
        : ServiceFramework{system_, "ISslContext"}, ssl_version{version},
          shared_data{std::make_shared<SslContextSharedData>()} {
        static const FunctionInfo functions[] = {
            {0, &ISslContext::SetOption, "SetOption"},
            {1, &ISslContext::GetOption, "GetOption"},
            {2, &ISslContext::CreateConnection, "CreateConnection"},
            {3, &ISslContext::GetConnectionCount, "GetConnectionCount"},
            {4, &ISslContext::ImportServerPki, "ImportServerPki"},
            {5, &ISslContext::ImportClientPki, "ImportClientPki"},
            {6, D<&ISslContext::RemoveServerPki>, "RemoveServerPki"},
            {7, D<&ISslContext::RemoveClientPki>, "RemoveClientPki"},
            {8, D<&ISslContext::RegisterInternalPki>, "RegisterInternalPki"},
            {9, D<&ISslContext::AddPolicyOid>, "AddPolicyOid"},
            {10, D<&ISslContext::ImportCrl>, "ImportCrl"},
            {11, D<&ISslContext::RemoveCrl>, "RemoveCrl"},
            {12, D<&ISslContext::ImportClientCertKeyPki>, "ImportClientCertKeyPki"},
            {13, D<&ISslContext::GeneratePrivateKeyAndCert>, "GeneratePrivateKeyAndCert"},
            // ISslContextForSystem only; the same object serves both ports.
            {14, &ISslContext::CreateConnectionEx, "CreateConnectionEx"},
        };
        RegisterHandlers(functions);
    }

private:
    SslVersion ssl_version;
    std::shared_ptr<SslContextSharedData> shared_data;

    void SetOption(HLERequestContext& ctx) {
        struct Parameters {
            ContextOption option;
            s32 value;
        };
        static_assert(sizeof(Parameters) == 0x8, "Parameters is an invalid size");

        IPC::RequestParser rp{ctx};
        const auto parameters = rp.PopRaw<Parameters>();

        LOG_WARNING(Service_SSL, "(STUBBED) called. option={}, value={}", parameters.option,
                    parameters.value);

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);

    }

    void GetOption(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto parameters = rp.PopRaw<OptionType>();

        LOG_WARNING(Service_SSL, "(STUBBED) called. option={}", parameters);

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void CreateConnection(HLERequestContext& ctx) {
        LOG_WARNING(Service_SSL, "called");

        std::unique_ptr<SSLConnectionBackend> backend;
        const Result res = CreateSSLConnectionBackend(&backend);

        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(res);
        if (res == ResultSuccess) {
            rb.PushIpcInterface<ISslConnection>(ctx, system, ssl_version, shared_data, std::move(backend));
        }
    }

    void GetConnectionCount(HLERequestContext& ctx) {
        LOG_DEBUG(Service_SSL, "connection_count={}", shared_data->connection_count);

        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(shared_data->connection_count);
    }

    void ImportServerPki(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto certificate_format = rp.PopEnum<CertificateFormat>();
        [[maybe_unused]] const auto pkcs_12_certificates = ctx.ReadBuffer(0);

        constexpr u64 server_id = 0;

        LOG_WARNING(Service_SSL, "(STUBBED) called, certificate_format={}", certificate_format);

        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push(server_id);
    }

    void ImportClientPki(HLERequestContext& ctx) {
        [[maybe_unused]] const auto pkcs_12_certificate = ctx.ReadBuffer(0);
        [[maybe_unused]] const auto ascii_password = [&ctx] {
            if (ctx.CanReadBuffer(1)) {
                return ctx.ReadBuffer(1);
            }

            return std::span<const u8>{};
        }();

        constexpr u64 client_id = 0;

        LOG_WARNING(Service_SSL, "(STUBBED) called");

        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push(client_id);
    }

    Result RegisterInternalPki() {
        LOG_WARNING(Service_SSL, "(STUBBED) called");
        R_SUCCEED();
    }

    // The PKI a guest imports is accepted and not used: the host verifies against its own roots
    // and the pinned OpenPak CA, never a list the guest supplies.
    Result RemoveServerPki(u64 server_id) {
        LOG_WARNING(Service_SSL, "(STUBBED) called");
        R_SUCCEED();
    }

    Result RemoveClientPki(u64 client_id) {
        LOG_WARNING(Service_SSL, "(STUBBED) called");
        R_SUCCEED();
    }

    Result AddPolicyOid(InBuffer<BufferAttr_HipcMapAlias> oid) {
        LOG_WARNING(Service_SSL, "(STUBBED) called");
        R_SUCCEED();
    }

    Result ImportCrl(InBuffer<BufferAttr_HipcMapAlias> crl, Out<u64> out_crl_id) {
        LOG_WARNING(Service_SSL, "(STUBBED) called");
        *out_crl_id = 0;
        R_SUCCEED();
    }

    Result RemoveCrl(u64 crl_id) {
        LOG_WARNING(Service_SSL, "(STUBBED) called");
        R_SUCCEED();
    }

    Result ImportClientCertKeyPki(Out<u64> out_client_id) {
        LOG_WARNING(Service_SSL, "unknown contract: ssl ISslContext 12 ImportClientCertKeyPki");
        *out_client_id = 0;
        R_SUCCEED();
    }

    Result GeneratePrivateKeyAndCert(Out<u64> out_client_id) {
        LOG_WARNING(Service_SSL, "unknown contract: ssl ISslContext 13 GeneratePrivateKeyAndCert");
        *out_client_id = 0;
        R_SUCCEED();
    }

    void CreateConnectionEx(HLERequestContext& ctx) {
        LOG_WARNING(Service_SSL,
                    "unknown contract: ssl ISslContextForSystem 14 CreateConnectionEx, answered "
                    "as CreateConnection");
        CreateConnection(ctx);
    }
};

// nn::ssl::sf::ISslService::CreateContext and ssl:s CreateContextForSystem take the same input.
struct CreateContextParameters {
    SslVersion ssl_version;
    INSERT_PADDING_BYTES(0x4);
    u64 pid_placeholder;
};
static_assert(sizeof(CreateContextParameters) == 0x10, "Parameters is an invalid size");

static void PushNewContext(Core::System& system, HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto parameters = rp.PopRaw<CreateContextParameters>();

    LOG_DEBUG(Service_SSL, "called, api_version={}", parameters.ssl_version.api_version.Value());

    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<ISslContext>(ctx, system, parameters.ssl_version);
}

class ISslService final : public ServiceFramework<ISslService> {
public:
    explicit ISslService(Core::System& system_)
        : ServiceFramework{system_, "ssl"}, cert_store{system} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &ISslService::CreateContext, "CreateContext"},
            {1, D<&ISslService::GetContextCount>, "GetContextCount"},
            {2, D<&ISslService::GetCertificates>, "GetCertificates"},
            {3, D<&ISslService::GetCertificateBufSize>, "GetCertificateBufSize"},
            {4, nullptr, "DebugIoctl"},
            {5, &ISslService::SetInterfaceVersion, "SetInterfaceVersion"},
            {6, D<&ISslService::FlushSessionCache>, "FlushSessionCache"},
            {7, D<&ISslService::SetDebugOption>, "SetDebugOption"},
            {8, D<&ISslService::GetDebugOption>, "GetDebugOption"},
            {9, D<&ISslService::ClearTls12FallbackFlag>, "ClearTls12FallbackFlag"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }

private:
    void CreateContext(HLERequestContext& ctx) {
        PushNewContext(system, ctx);
    }

    Result GetContextCount(Out<u32> out_count) {
        LOG_WARNING(Service_SSL, "(STUBBED) called");
        *out_count = 0;
        R_SUCCEED();
    }

    Result FlushSessionCache(u32 option, InBuffer<BufferAttr_HipcMapAlias> host_name,
                             Out<u32> out_flushed) {
        LOG_WARNING(Service_SSL, "(STUBBED) called");
        *out_flushed = 0;
        R_SUCCEED();
    }

    Result SetDebugOption(u32 option, InBuffer<BufferAttr_HipcMapAlias> value) {
        LOG_WARNING(Service_SSL, "(STUBBED) called, option={}", option);
        R_SUCCEED();
    }

    Result GetDebugOption(u32 option, OutBuffer<BufferAttr_HipcMapAlias> out_value) {
        LOG_WARNING(Service_SSL, "(STUBBED) called, option={}", option);
        std::memset(out_value.data(), 0, out_value.size());
        R_SUCCEED();
    }

    Result ClearTls12FallbackFlag() {
        LOG_WARNING(Service_SSL, "(STUBBED) called");
        R_SUCCEED();
    }

    void SetInterfaceVersion(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        u32 ssl_version = rp.Pop<u32>();

        LOG_DEBUG(Service_SSL, "called, ssl_version={}", ssl_version);

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    Result GetCertificateBufSize(
        Out<u32> out_size, InArray<CaCertificateId, BufferAttr_HipcMapAlias> certificate_ids) {
        LOG_INFO(Service_SSL, "called");
        u32 num_entries;
        R_RETURN(cert_store.GetCertificateBufSize(out_size, &num_entries, certificate_ids));
    }

    Result GetCertificates(Out<u32> out_num_entries, OutBuffer<BufferAttr_HipcMapAlias> out_buffer,
                           InArray<CaCertificateId, BufferAttr_HipcMapAlias> certificate_ids) {
        LOG_INFO(Service_SSL, "called");
        R_RETURN(cert_store.GetCertificates(out_num_entries, out_buffer, certificate_ids));
    }

private:
    CertStore cert_store;
};

// ssl:s, the system port: the same contexts and certificate store as ssl.
class ISslServiceForSystem final : public ServiceFramework<ISslServiceForSystem> {
public:
    explicit ISslServiceForSystem(Core::System& system_)
        : ServiceFramework{system_, "ssl:s"}, cert_store{system} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &ISslServiceForSystem::CreateContext, "CreateContext"},
            {1, D<&ISslServiceForSystem::GetContextCount>, "GetContextCount"},
            {2, D<&ISslServiceForSystem::GetCertificates>, "GetCertificates"},
            {3, D<&ISslServiceForSystem::GetCertificateBufSize>, "GetCertificateBufSize"},
            {4, D<&ISslServiceForSystem::DebugIoctl>, "DebugIoctl"},
            {5, D<&ISslServiceForSystem::SetInterfaceVersion>, "SetInterfaceVersion"},
            {6, D<&ISslServiceForSystem::FlushSessionCache>, "FlushSessionCache"},
            {7, D<&ISslServiceForSystem::SetDebugOption>, "SetDebugOption"},
            {8, D<&ISslServiceForSystem::GetDebugOption>, "GetDebugOption"},
            {9, D<&ISslServiceForSystem::ClearTls12FallbackFlag>, "ClearTls12FallbackFlag"},
            {100, &ISslServiceForSystem::CreateContextForSystem, "CreateContextForSystem"},
            {101, D<&ISslServiceForSystem::SetThreadCoreMask>, "SetThreadCoreMask"},
            {102, D<&ISslServiceForSystem::GetThreadCoreMask>, "GetThreadCoreMask"},
            {103, D<&ISslServiceForSystem::VerifySignature>, "VerifySignature"}
        };
        // clang-format on

        RegisterHandlers(functions);
    }

private:
    // A context opened through ssl:s is a real one: answering success with no object left the
    // caller holding nothing.
    void CreateContext(HLERequestContext& ctx) {
        PushNewContext(system, ctx);
    }

    void CreateContextForSystem(HLERequestContext& ctx) {
        PushNewContext(system, ctx);
    }

    Result GetContextCount(Out<u32> out_count) {
        LOG_WARNING(Service_SSL, "(STUBBED) called");
        *out_count = 0;
        R_SUCCEED();
    }

    Result GetCertificates(Out<u32> out_num_entries, OutBuffer<BufferAttr_HipcMapAlias> out_buffer,
                           InArray<CaCertificateId, BufferAttr_HipcMapAlias> certificate_ids) {
        R_RETURN(cert_store.GetCertificates(out_num_entries, out_buffer, certificate_ids));
    }

    Result GetCertificateBufSize(
        Out<u32> out_size, InArray<CaCertificateId, BufferAttr_HipcMapAlias> certificate_ids) {
        u32 num_entries;
        R_RETURN(cert_store.GetCertificateBufSize(out_size, &num_entries, certificate_ids));
    }

    Result DebugIoctl() {
        LOG_WARNING(Service_SSL, "(STUBBED) called");
        R_SUCCEED();
    }

    Result SetInterfaceVersion(u32 version) {
        LOG_DEBUG(Service_SSL, "called, version={}", version);
        R_SUCCEED();
    }

    Result FlushSessionCache(u32 option, InBuffer<BufferAttr_HipcMapAlias> host_name,
                             Out<u32> out_flushed) {
        LOG_WARNING(Service_SSL, "(STUBBED) called");
        *out_flushed = 0;
        R_SUCCEED();
    }

    Result SetDebugOption(u32 option, InBuffer<BufferAttr_HipcMapAlias> value) {
        LOG_WARNING(Service_SSL, "(STUBBED) called, option={}", option);
        R_SUCCEED();
    }

    Result GetDebugOption(u32 option, OutBuffer<BufferAttr_HipcMapAlias> out_value) {
        LOG_WARNING(Service_SSL, "(STUBBED) called, option={}", option);
        std::memset(out_value.data(), 0, out_value.size());
        R_SUCCEED();
    }

    Result ClearTls12FallbackFlag() {
        LOG_WARNING(Service_SSL, "(STUBBED) called");
        R_SUCCEED();
    }

    Result SetThreadCoreMask(u64 mask) {
        LOG_DEBUG(Service_SSL, "(STUBBED) called, mask={:#x}", mask);
        thread_core_mask = mask;
        R_SUCCEED();
    }

    Result GetThreadCoreMask(Out<u64> out_mask) {
        *out_mask = thread_core_mask;
        R_SUCCEED();
    }

    Result VerifySignature() {
        LOG_WARNING(Service_SSL, "unknown contract: ssl:s 103 VerifySignature, answered success");
        R_SUCCEED();
    }

    CertStore cert_store;
    u64 thread_core_mask = 0;
};

void LoopProcess(Core::System& system) {
    auto server_manager = std::make_unique<ServerManager>(system);

    server_manager->RegisterNamedService("ssl", std::make_shared<ISslService>(system));
    server_manager->RegisterNamedService("ssl:s", std::make_shared<ISslServiceForSystem>(system));
    ServerManager::RunServer(std::move(server_manager));
}

} // namespace Service::SSL

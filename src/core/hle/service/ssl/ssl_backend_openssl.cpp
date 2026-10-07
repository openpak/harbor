// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <mutex>
#include <vector>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "common/fs/file.h"
#include "common/fs/path_util.h"
#include "common/hex_util.h"
#include "common/string_util.h"

#include "core/hle/service/ssl/ssl_backend.h"
#include "core/hle/service/ssl/ssl_trust.h"
#include "core/internal_network/network.h"
#include "core/internal_network/sockets.h"

#ifdef YUZU_BUNDLED_OPENSSL
#include <openssl/cert.h>
#endif

using namespace Common::FS;

namespace Service::SSL {

// Import OpenSSL's `SSL` type into the namespace.  This is needed because the
// namespace is also named `SSL`.
using ::SSL;

namespace {

std::once_flag one_time_init_flag;
bool one_time_init_success = false;

SSL_CTX* ssl_ctx;
#ifndef NDEBUG
// TLS secrets of guest connections: debug builds only, and only when SSLKEYLOGFILE is set.
IOFile key_log_file;
#endif
BIO_METHOD* bio_meth;

// The pinned OpenPak CA last added to the context's store, so an unchanged file is not re-added.
std::mutex pinned_ca_mutex;
std::string pinned_ca_pem;

Result CheckOpenSSLErrors();
void OneTimeInit();
void OneTimeInitLogFile();
bool OneTimeInitBIO();
void TrustPinnedCa();

#ifdef YUZU_BUNDLED_OPENSSL
// This is ported from httplib
struct scope_exit {
  explicit scope_exit(std::function<void(void)> &&f)
      : exit_function(std::move(f)), execute_on_destruction{true} {}

  scope_exit(scope_exit &&rhs) noexcept
      : exit_function(std::move(rhs.exit_function)),
        execute_on_destruction{rhs.execute_on_destruction} {
    rhs.release();
  }

  ~scope_exit() {
    if (execute_on_destruction) { this->exit_function(); }
  }

  void release() { this->execute_on_destruction = false; }

private:
  scope_exit(const scope_exit &) = delete;
  void operator=(const scope_exit &) = delete;
  scope_exit &operator=(scope_exit &&) = delete;

  std::function<void(void)> exit_function;
  bool execute_on_destruction;
};

inline X509_STORE *CreateCaCertStore(const char *ca_cert,
                                                    std::size_t size) {
    auto mem = BIO_new_mem_buf(ca_cert, static_cast<int>(size));
    auto se = scope_exit([&] { BIO_free_all(mem); });
    if (!mem) { return nullptr; }

    auto inf = PEM_X509_INFO_read_bio(mem, nullptr, nullptr, nullptr);
    if (!inf) { return nullptr; }

    auto cts = X509_STORE_new();
    if (cts) {
        for (auto i = 0; i < static_cast<int>(sk_X509_INFO_num(inf)); i++) {
            auto itmp = sk_X509_INFO_value(inf, i);
            if (!itmp) { continue; }

            if (itmp->x509) { X509_STORE_add_cert(cts, itmp->x509); }
            if (itmp->crl) { X509_STORE_add_crl(cts, itmp->crl); }
        }
    }

    sk_X509_INFO_pop_free(inf, X509_INFO_free);
    return cts;
}

inline void SetCaCertStore(SSL_CTX *ctx, X509_STORE *ca_cert_store) {
    if (ca_cert_store) {
        if (ctx) {
            if (SSL_CTX_get_cert_store(ctx) != ca_cert_store) {
                // Free memory allocated for old cert and use new store `ca_cert_store`
                SSL_CTX_set_cert_store(ctx, ca_cert_store);
            }
        } else {
            X509_STORE_free(ca_cert_store);
        }
    }
}

inline void LoadCaCertStore(SSL_CTX* ctx, const char* ca_cert, std::size_t size)
{
    SetCaCertStore(ctx, CreateCaCertStore(ca_cert, size));
}
#endif

} // namespace

class SSLConnectionBackendOpenSSL final : public SSLConnectionBackend {
public:
    Result Init() {
        std::call_once(one_time_init_flag, OneTimeInit);

        if (!one_time_init_success) {
            LOG_ERROR(Service_SSL,
                      "Can't create SSL connection because OpenSSL one-time initialization failed");
            return ResultInternalError;
        }

        TrustPinnedCa();

        ssl = SSL_new(ssl_ctx);
        if (!ssl) {
            LOG_ERROR(Service_SSL, "SSL_new failed");
            return CheckOpenSSLErrors();
        }

        SSL_set_connect_state(ssl);
        // Always verified host-side, whatever the guest's verify option (ssl_trust.h).
        SSL_set_verify(ssl, SSL_VERIFY_PEER, nullptr);

        bio = BIO_new(bio_meth);
        if (!bio) {
            LOG_ERROR(Service_SSL, "BIO_new failed");
            return CheckOpenSSLErrors();
        }

        BIO_set_data(bio, this);
        BIO_set_init(bio, 1);
        SSL_set_bio(ssl, bio, bio);

        return ResultSuccess;
    }

    void SetSocket(std::shared_ptr<Network::SocketBase> socket_in) override {
        socket = std::move(socket_in);
    }

    // The name and the verify option arrive in either order, so the name is only remembered here
    // and applied when the handshake starts.
    Result SetHostName(const char* hostname) override {
        host_name = hostname;
        R_SUCCEED();
    }

    Result GetHostName(std::span<u8> data, u32* out_size) override {
        auto const peer_name = SSL_get0_peername(ssl);
        if (peer_name == nullptr) {
            LOG_ERROR(Service_SSL, "SSL_get0_peername()");
            return CheckOpenSSLErrors();
        }
        auto const s = std::string{peer_name};
        *out_size = u32(s.size());
        std::memcpy(data.data(), s.data(), (std::min)(s.size(), data.size()));
        R_SUCCEED();
    }

    void SetVerifyOption(u32 option) override {
        // Recorded for the log only. A guest that asks for no verification still gets it: a
        // redirected Nintendo name must prove it is OpenPak, and nothing else is excused either
        // (the Ryujinx Harbor's model).
        LOG_DEBUG(Service_SSL, "guest verify option {} (the host verifies regardless)", option);
    }

    // The name the certificate must carry: the one the guest set, or the peer's address when it
    // set none. A literal address is matched against the IP SAN and never sent as SNI.
    Result ApplyVerification() {
        verify_name = host_name;
        if (verify_name.empty() && socket) {
            const auto [peer, err] = socket->GetPeerName();
            if (err == Network::Errno::SUCCESS) {
                verify_name = Network::IPv4AddressToString(peer.ip);
            }
        }
        if (!Trust::SetExpectedPeer(SSL_get0_param(ssl), verify_name)) {
            LOG_ERROR(Service_SSL, "No name to verify the server against; refusing the handshake");
            return ResultConnectionReset;
        }
        if (!host_name.empty() && !Trust::IsIpLiteral(host_name) &&
            !SSL_set_tlsext_host_name(ssl, host_name.c_str())) {
            LOG_ERROR(Service_SSL, "SSL_set_tlsext_host_name({}) failed", host_name);
            return CheckOpenSSLErrors();
        }
        R_SUCCEED();
    }

    // Whether the verified chain's root is the pinned OpenPak CA (for the log only).
    bool ChainEndsAtPinnedCa() const {
        STACK_OF(X509)* const chain = SSL_get0_verified_chain(ssl);
        const int count = chain != nullptr ? sk_X509_num(chain) : 0;
        if (count == 0) {
            return false;
        }
        unsigned char* buffer = nullptr;
        const int length = i2d_X509(sk_X509_value(chain, count - 1), &buffer);
        std::vector<unsigned char> pinned;
        {
            std::scoped_lock lock{pinned_ca_mutex};
            pinned = Trust::PemCaToDer(pinned_ca_pem);
        }
        const bool same = length > 0 && buffer != nullptr &&
                          pinned.size() == static_cast<std::size_t>(length) &&
                          std::equal(pinned.begin(), pinned.end(), buffer);
        OPENSSL_free(buffer);
        return same;
    }

    Result DoHandshake() override {
        // Once: a non-blocking handshake is called again until it completes.
        if (!verification_applied) {
            R_TRY(ApplyVerification());
            verification_applied = true;
        }

        const int ret = SSL_do_handshake(ssl);

        if (ret == 1) {
            LOG_DEBUG(Service_SSL, "Verified {}: the chain ends at {}", verify_name,
                      ChainEndsAtPinnedCa() ? "the pinned OpenPak CA" : "a host root");
        }
        if (ret <= 0) {
            const long verify_result = SSL_get_verify_result(ssl);
            if (verify_result != X509_V_OK) {
                LOG_WARNING(Service_SSL, "Refused the certificate of {}: {}", verify_name,
                            X509_verify_cert_error_string(verify_result));
                ERR_clear_error();
                return ResultConnectionReset;
            }

            const int ssl_err = SSL_get_error(ssl, ret);
            if (ssl_err == SSL_ERROR_ZERO_RETURN ||
                (ssl_err == SSL_ERROR_SYSCALL && got_read_eof)) {
                LOG_ERROR(Service_SSL, "SSL handshake failed because server hung up");
                return ResultInternalError;
            }
        }
        return HandleReturn("SSL_do_handshake", 0, ret);
    }

    Result Read(size_t* out_size, std::span<u8> data) override {
        const int ret = SSL_read_ex(ssl, data.data(), data.size(), out_size);
        return HandleReturn("SSL_read_ex", out_size, ret);
    }

    // Reads without consuming: Moving Out 2's websocket uses Peek as its readiness check.
    Result Peek(size_t* out_size, std::span<u8> data) override {
        *out_size = 0;
        const int ret = SSL_peek_ex(ssl, data.data(), data.size(), out_size);
        return HandleReturn("SSL_peek_ex", out_size, ret);
    }

    Result Write(size_t* out_size, std::span<const u8> data) override {
        const int ret = SSL_write_ex(ssl, data.data(), data.size(), out_size);
        return HandleReturn("SSL_write_ex", out_size, ret);
    }

    Result HandleReturn(const char* what, size_t* actual, int ret) {
        const int ssl_err = SSL_get_error(ssl, ret);
        CheckOpenSSLErrors();
        switch (ssl_err) {
        case SSL_ERROR_NONE:
            return ResultSuccess;
        case SSL_ERROR_ZERO_RETURN:
            LOG_DEBUG(Service_SSL, "{} => SSL_ERROR_ZERO_RETURN", what);
            // DoHandshake special-cases this, but for Read and Write:
            *actual = 0;
            return ResultSuccess;
        case SSL_ERROR_WANT_READ:
            LOG_DEBUG(Service_SSL, "{} => SSL_ERROR_WANT_READ", what);
            return ResultWouldBlock;
        case SSL_ERROR_WANT_WRITE:
            LOG_DEBUG(Service_SSL, "{} => SSL_ERROR_WANT_WRITE", what);
            return ResultWouldBlock;
        default:
            if (ssl_err == SSL_ERROR_SYSCALL && got_read_eof) {
                LOG_DEBUG(Service_SSL, "{} => SSL_ERROR_SYSCALL because server hung up", what);
                *actual = 0;
                return ResultSuccess;
            }
            LOG_ERROR(Service_SSL, "{} => other SSL_get_error return value {}", what, ssl_err);
            return ResultInternalError;
        }
    }

    Result GetServerCerts(std::vector<std::vector<u8>>* out_certs) override {
        STACK_OF(X509)* chain = SSL_get_peer_cert_chain(ssl);
        if (!chain) {
            LOG_ERROR(Service_SSL, "SSL_get_peer_cert_chain returned nullptr");
            return ResultInternalError;
        }
        int count = sk_X509_num(chain);
        ASSERT(count >= 0);
        for (int i = 0; i < count; i++) {
            X509* x509 = sk_X509_value(chain, i);
            ASSERT_OR_EXECUTE(x509 != nullptr, { continue; });
            unsigned char* buf = nullptr;
            int len = i2d_X509(x509, &buf);
            ASSERT_OR_EXECUTE(len >= 0 && buf, { continue; });
            out_certs->emplace_back(buf, buf + len);
            OPENSSL_free(buf);
        }
        R_SUCCEED();
    }

    int Pending() override {
        return SSL_pending(ssl);
    }

    // This connection's own options: the context is shared by every guest connection.
    Result SetRenegotiationMode(u32 mode) override {
        if (mode == 0) {
            SSL_clear_options(ssl, SSL_OP_ALLOW_CLIENT_RENEGOTIATION);
            SSL_set_options(ssl, SSL_OP_NO_RENEGOTIATION);
        } else {
            SSL_clear_options(ssl, SSL_OP_NO_RENEGOTIATION);
            SSL_set_options(ssl, SSL_OP_ALLOW_CLIENT_RENEGOTIATION);
        }
        R_SUCCEED();
    }

    Result GetRenegotiationMode(u32* mode) override {
        *mode = SSL_get_secure_renegotiation_support(ssl) ? 1 : 0;
        R_SUCCEED();
    }

    // The guest's list goes to OpenSSL as it arrived: SetNextAlpnProto carries the
    // length-prefixed wire form SSL_set_alpn_protos wants.
    void SetAlpnProtos(std::span<const u8> protos) override {
        if (protos.empty()) {
            return;
        }
        if (SSL_set_alpn_protos(ssl, protos.data(), static_cast<unsigned>(protos.size())) != 0) {
            LOG_ERROR(Service_SSL, "SSL_set_alpn_protos failed");
        }
    }

    std::vector<u8> GetNegotiatedAlpnProto() override {
        const unsigned char* selected = nullptr;
        unsigned int length = 0;
        SSL_get0_alpn_selected(ssl, &selected, &length);
        if (selected == nullptr || length == 0) {
            return {};
        }
        return {selected, selected + length};
    }

    Result GetCipherInfo(std::string* cipher_name, std::string* protocol_version) override {
        const SSL_CIPHER* const cipher = SSL_get_current_cipher(ssl);
        if (cipher == nullptr) {
            return ResultInternalError;
        }
        *cipher_name = SSL_CIPHER_get_name(cipher);
        *protocol_version = SSL_get_version(ssl);
        R_SUCCEED();
    }

    ~SSLConnectionBackendOpenSSL() {
        // this is null-tolerant:
        SSL_free(ssl);
    }

#ifndef NDEBUG
    static void KeyLogCallback(const SSL* ssl, const char* line) {
        std::string str(line);
        str.push_back('\n');
        // Do this in a single WriteString for atomicity if multiple instances
        // are running on different threads (though that can't currently
        // happen). The secret itself never goes to the log.
        if (key_log_file.WriteString(str) != str.size() || !key_log_file.Flush()) {
            LOG_CRITICAL(Service_SSL, "Failed to write to SSLKEYLOGFILE");
        }
    }
#endif

    static int WriteCallback(BIO* bio, const char* buf, size_t len, size_t* actual_p) {
        auto self = static_cast<SSLConnectionBackendOpenSSL*>(BIO_get_data(bio));
        ASSERT_OR_EXECUTE_MSG(
            self->socket, { return 0; }, "OpenSSL asked to send but we have no socket");
        BIO_clear_retry_flags(bio);
        auto [actual, err] = self->socket->Send({reinterpret_cast<const u8*>(buf), len}, 0);
        switch (err) {
        case Network::Errno::SUCCESS:
            *actual_p = actual;
            return 1;
        case Network::Errno::AGAIN:
            BIO_set_flags(bio, BIO_FLAGS_WRITE | BIO_FLAGS_SHOULD_RETRY);
            return 0;
        default:
            LOG_ERROR(Service_SSL, "Socket send returned Network::Errno {}", err);
            return -1;
        }
    }

    static int ReadCallback(BIO* bio, char* buf, size_t len, size_t* actual_p) {
        auto self = static_cast<SSLConnectionBackendOpenSSL*>(BIO_get_data(bio));
        ASSERT_OR_EXECUTE_MSG(
            self->socket, { return 0; }, "OpenSSL asked to recv but we have no socket");
        BIO_clear_retry_flags(bio);
        auto [actual, err] = self->socket->Recv(0, {reinterpret_cast<u8*>(buf), len});
        switch (err) {
        case Network::Errno::SUCCESS:
            *actual_p = actual;
            if (actual == 0) {
                self->got_read_eof = true;
            }
            return actual ? 1 : 0;
        case Network::Errno::AGAIN:
            BIO_set_flags(bio, BIO_FLAGS_READ | BIO_FLAGS_SHOULD_RETRY);
            return 0;
        default:
            LOG_ERROR(Service_SSL, "Socket recv returned Network::Errno {}", err);
            return -1;
        }
    }

    static long CtrlCallback(BIO* bio, int cmd, long l_arg, void* p_arg) {
        switch (cmd) {
        case BIO_CTRL_FLUSH:
            // Nothing to flush.
            return 1;
        case BIO_CTRL_PUSH:
        case BIO_CTRL_POP:
#ifdef BIO_CTRL_GET_KTLS_SEND
        case BIO_CTRL_GET_KTLS_SEND:
        case BIO_CTRL_GET_KTLS_RECV:
#endif
            // We don't support these operations, but don't bother logging them
            // as they're nothing unusual.
            return 0;
        default:
            LOG_DEBUG(Service_SSL, "OpenSSL BIO got ctrl({}, {}, {})", cmd, l_arg, p_arg);
            return 0;
        }
    }

    SSL* ssl = nullptr;
    BIO* bio = nullptr;
    bool got_read_eof = false;
    bool verification_applied = false;
    std::string host_name;
    std::string verify_name;

    std::shared_ptr<Network::SocketBase> socket;
};

Result CreateSSLConnectionBackend(std::unique_ptr<SSLConnectionBackend>* out_backend) {
    auto conn = std::make_unique<SSLConnectionBackendOpenSSL>();

    R_TRY(conn->Init());

    *out_backend = std::move(conn);
    return ResultSuccess;
}

namespace {

Result CheckOpenSSLErrors() {
    unsigned long rc;
    const char* file;
    int line;
    const char* func;
    const char* data;
    int flags;
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    while ((rc = ERR_get_error_all(&file, &line, &func, &data, &flags)))
#else
    // Can't get function names from OpenSSL on this version, so use mine:
    func = __func__;
    while ((rc = ERR_get_error_line_data(&file, &line, &data, &flags)))
#endif
    {
        std::string msg;
        msg.resize(1024, '\0');
        ERR_error_string_n(rc, msg.data(), msg.size());
        msg.resize(strlen(msg.data()), '\0');
        if (flags & ERR_TXT_STRING) {
            msg.append(" | ");
            msg.append(data);
        }
        Common::Log::FmtLogMessage(Common::Log::Class::Service_SSL, Common::Log::Level::Error,
                                   file, line, func, "OpenSSL: {}",
                                   msg);
    }
    return ResultInternalError;
}

void OneTimeInit() {
    ssl_ctx = SSL_CTX_new(TLS_client_method());
    if (!ssl_ctx) {
        LOG_ERROR(Service_SSL, "SSL_CTX_new failed");
        CheckOpenSSLErrors();
        return;
    }

    SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_PEER, nullptr);

    if (!SSL_CTX_set_default_verify_paths(ssl_ctx)) {
        LOG_ERROR(Service_SSL, "SSL_CTX_set_default_verify_paths failed");
        CheckOpenSSLErrors();
        return;
    }

    // Once, before any connection: reloading it per connection replaced the store, and with it
    // the OpenPak CA, under every connection already set up.
#ifdef YUZU_BUNDLED_OPENSSL
    LoadCaCertStore(ssl_ctx, kCert, sizeof(kCert));
#endif

    OneTimeInitLogFile();

    if (!OneTimeInitBIO()) {
        return;
    }

    one_time_init_success = true;
}

void OneTimeInitLogFile() {
#ifndef NDEBUG
    const char* logfile = getenv("SSLKEYLOGFILE");
    if (logfile) {
        key_log_file.Open(logfile, FileAccessMode::Append, FileType::TextFile,
                          FileShareFlag::ShareWriteOnly);
        if (key_log_file.IsOpen()) {
            SSL_CTX_set_keylog_callback(ssl_ctx, &SSLConnectionBackendOpenSSL::KeyLogCallback);
        } else {
            LOG_CRITICAL(Service_SSL,
                         "SSLKEYLOGFILE was set but file could not be opened; not logging keys!");
        }
    }
#endif
}

std::filesystem::path PinnedCaFile() {
    return Trust::PinnedCaPath(Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "openpak" /
                               "ca.pem");
}

// OpenPak answers to Nintendo's own names, which no public CA can issue for, so its pinned CA
// joins the roots guest connections are checked against. Nothing else is loosened: a chain that
// reaches neither it nor a host root is refused. Behaviour follows the Ryujinx Harbor (MIT-licensed
// Ryujinx base).
void TrustPinnedCa() {
    std::string pem = Trust::ReadFile(PinnedCaFile());
    if (pem.empty()) {
        return;
    }
    std::scoped_lock lock{pinned_ca_mutex};
    if (pem == pinned_ca_pem) {
        return;
    }
    const int added = Trust::AddPemCaCertificates(SSL_CTX_get_cert_store(ssl_ctx), pem);
    if (added == 0) {
        LOG_WARNING(Service_SSL, "openpak/ca.pem holds no CA certificate; it is not trusted");
        return;
    }
    pinned_ca_pem = std::move(pem);
    LOG_INFO(Service_SSL, "Guest TLS trusts the host roots and the pinned OpenPak CA");
}

bool OneTimeInitBIO() {
    bio_meth =
        BIO_meth_new(BIO_get_new_index() | BIO_TYPE_SOURCE_SINK, "SSLConnectionBackendOpenSSL");
    if (!bio_meth ||
        !BIO_meth_set_write_ex(bio_meth, &SSLConnectionBackendOpenSSL::WriteCallback) ||
        !BIO_meth_set_read_ex(bio_meth, &SSLConnectionBackendOpenSSL::ReadCallback) ||
        !BIO_meth_set_ctrl(bio_meth, &SSLConnectionBackendOpenSSL::CtrlCallback)) {
        LOG_ERROR(Service_SSL, "Failed to create BIO_METHOD");
        return false;
    }
    return true;
}

} // namespace

std::vector<u8> LoadPinnedCaDer() {
    return Trust::PemCaToDer(Trust::ReadFile(PinnedCaFile()));
}

} // namespace Service::SSL

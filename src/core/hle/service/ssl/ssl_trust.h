// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// How a guest TLS connection decides whom it trusts. The model is the Ryujinx Harbor's: the host
// verifies every connection itself, whatever verify option the guest set and whatever CAs it
// imported. A chain is accepted when it reaches a root the host trusts, or the pinned OpenPak CA
// for a name OpenPak answers (one the resolver redirects, or the console server itself); the
// peer's certificate must carry the name the guest dialled (its IP SAN when the guest named an
// address), and a mismatch is never excused: behind a redirect, the name is the only thing that
// says who answered.
//
// Header-only and free of Eden types, so ssl_trust_selfcheck.cpp can build it on its own.

#pragma once

#include <cstdio>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "core/hle/service/sockets/sfdnsres_redirect.h"

namespace Service::SSL::Trust {

// Where the pinned OpenPak CA lives (openpak/ca.pem under the user directory). The host boot sets
// it once it has fetched and checked the CA against the profile's pin; an empty path means no
// OpenPak CA is trusted. Read on every new connection, so a CA written after boot is picked up.
inline std::mutex& HookMutex() {
    static std::mutex mutex;
    return mutex;
}

inline std::function<std::filesystem::path()>& PinnedCaPathHook() {
    static std::function<std::filesystem::path()> hook;
    return hook;
}

inline void SetPinnedCaPath(std::function<std::filesystem::path()> hook) {
    std::scoped_lock lock{HookMutex()};
    PinnedCaPathHook() = std::move(hook);
}

inline std::filesystem::path PinnedCaPath(const std::filesystem::path& fallback) {
    std::scoped_lock lock{HookMutex()};
    return PinnedCaPathHook() ? PinnedCaPathHook()() : fallback;
}

// True for an IPv4 or IPv6 literal. Such a name is checked against the certificate's IP SAN and is
// never sent as SNI, which carries host names only (RFC 6066).
inline bool IsIpLiteral(const std::string& name) {
    ASN1_OCTET_STRING* const ip = a2i_IPADDRESS(name.c_str());
    const bool is_ip = ip != nullptr;
    ASN1_OCTET_STRING_free(ip);
    return is_ip;
}

// The name the peer's certificate must carry. Fails (and the handshake with it) for an empty name:
// there is nothing to match, and matching nothing is not verification.
inline bool SetExpectedPeer(X509_VERIFY_PARAM* param, const std::string& name) {
    if (param == nullptr || name.empty()) {
        return false;
    }
    if (IsIpLiteral(name)) {
        return X509_VERIFY_PARAM_set1_host(param, nullptr, 0) == 1 &&
               X509_VERIFY_PARAM_set1_ip_asc(param, name.c_str()) == 1;
    }
    X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    return X509_VERIFY_PARAM_set1_host(param, name.c_str(), name.size()) == 1;
}

// Adds every CA certificate in `pem` to `store` and returns how many it added. A certificate that
// is not a CA (basic constraints) is refused, as the Ryujinx Harbor refuses it: a leaf in ca.pem
// would otherwise vouch for itself.
inline int AddPemCaCertificates(X509_STORE* store, std::string_view pem) {
    if (store == nullptr || pem.empty()) {
        return 0;
    }
    BIO* const bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (bio == nullptr) {
        return 0;
    }
    int added = 0;
    while (X509* const cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr)) {
        if (X509_check_ca(cert) >= 1 && X509_STORE_add_cert(store, cert) == 1) {
            ++added;
        }
        X509_free(cert);
    }
    BIO_free(bio);
    return added;
}

// The first CA certificate in `pem` as DER, or nothing.
inline std::vector<unsigned char> PemCaToDer(std::string_view pem) {
    std::vector<unsigned char> der;
    if (pem.empty()) {
        return der;
    }
    BIO* const bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (bio == nullptr) {
        return der;
    }
    while (X509* const cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr)) {
        const bool is_ca = X509_check_ca(cert) >= 1;
        unsigned char* buffer = nullptr;
        const int length = is_ca ? i2d_X509(cert, &buffer) : -1;
        if (length > 0 && buffer != nullptr) {
            der.assign(buffer, buffer + length);
        }
        OPENSSL_free(buffer);
        X509_free(cert);
        if (!der.empty()) {
            break;
        }
    }
    BIO_free(bio);
    return der;
}

// Reads a whole file, or nothing.
inline std::string ReadFile(const std::filesystem::path& path) {
    std::string data;
    if (path.empty()) {
        return data;
    }
    std::FILE* const file = std::fopen(path.string().c_str(), "rb");
    if (file == nullptr) {
        return data;
    }
    char chunk[4096];
    std::size_t read = 0;
    while ((read = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
        data.append(chunk, read);
    }
    std::fclose(file);
    return data;
}

// The decision itself, for one chain (leaf first) against one store: what the handshake's
// verification computes, exposed for the self-check. X509_V_OK or the OpenSSL reason.
inline int VerifyChain(X509_STORE* store, X509* leaf, STACK_OF(X509)* untrusted,
                       const std::string& name) {
    X509_STORE_CTX* const ctx = X509_STORE_CTX_new();
    if (ctx == nullptr || X509_STORE_CTX_init(ctx, store, leaf, untrusted) != 1) {
        X509_STORE_CTX_free(ctx);
        return X509_V_ERR_UNSPECIFIED;
    }
    X509_VERIFY_PARAM* const param = X509_STORE_CTX_get0_param(ctx);
    X509_VERIFY_PARAM_set_purpose(param, X509_PURPOSE_SSL_SERVER);
    if (!SetExpectedPeer(param, name)) {
        X509_STORE_CTX_free(ctx);
        return X509_V_ERR_HOSTNAME_MISMATCH;
    }
    const int ok = X509_verify_cert(ctx);
    const int error = ok == 1 ? X509_V_OK : X509_STORE_CTX_get_error(ctx);
    X509_STORE_CTX_free(ctx);
    return error == X509_V_OK && ok != 1 ? X509_V_ERR_UNSPECIFIED : error;
}

// Whether the pinned OpenPak CA may vouch for `name` (the name the guest dialled). Only for a name
// the resolver redirects to OpenPak, from the same answer it gives the guest, and for the console
// server's own address or name. Everything else is checked against the host roots alone: the
// OpenPak CA signs for any name it is asked for, and must not speak for one the guest reached
// through host DNS. Answered from memory on every connection, so a profile refresh applies at once.
inline bool PinnedCaTrustedFor(std::string_view name, const Sockets::Dns::Hooks& hooks,
                               std::string_view console_server) {
    namespace Dns = Sockets::Dns;
    const std::string host = Dns::Normalise(name);
    if (host.empty() || !hooks.enabled || !hooks.enabled()) {
        return false;
    }
    if (!console_server.empty() && host == Dns::Normalise(console_server)) {
        return true;
    }
    return Dns::Decide(host, hooks).verdict == Dns::Verdict::Redirect;
}

// The store a connection verifies against: `pinned_store` (the host roots plus the pinned CA) for
// a name the pinned CA may vouch for, `host_store` for every other.
inline X509_STORE* StoreFor(std::string_view name, X509_STORE* host_store,
                            X509_STORE* pinned_store, const Sockets::Dns::Hooks& hooks,
                            std::string_view console_server) {
    return pinned_store != nullptr && PinnedCaTrustedFor(name, hooks, console_server)
               ? pinned_store
               : host_store;
}

// Whether a certificate in `chain` was issued by a CA certificate in `pem` (for the log line that
// names a refused OpenPak chain).
inline bool ChainIssuedBy(STACK_OF(X509)* chain, std::string_view pem) {
    if (chain == nullptr || pem.empty()) {
        return false;
    }
    BIO* const bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (bio == nullptr) {
        return false;
    }
    bool issued = false;
    while (X509* const ca = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr)) {
        for (int i = 0; !issued && i < sk_X509_num(chain); ++i) {
            issued = X509_check_ca(ca) >= 1 &&
                     X509_check_issued(ca, sk_X509_value(chain, i)) == X509_V_OK;
        }
        X509_free(ca);
        if (issued) {
            break;
        }
    }
    BIO_free(bio);
    return issued;
}

} // namespace Service::SSL::Trust

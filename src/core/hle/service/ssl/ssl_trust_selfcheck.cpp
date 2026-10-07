// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Self-check for the guest TLS trust decisions (ssl_trust.h). Eden's unit tests are off in the
// Harbor build, so this is a standalone program:
//
//   g++ -std=c++20 -I <src> ssl_trust_selfcheck.cpp -lssl -lcrypto -o ssl_trust_selfcheck
//   ./ssl_trust_selfcheck
//
// It mints a throwaway OpenPak-style CA, a leaf it signs and a leaf from an unrelated CA, then
// checks what a handshake would decide. Prints "ssl trust self-check: OK" and exits 0, or names
// the failed check and exits 1.

#include <cstdio>
#include <cstdlib>
#include <string>

#include <openssl/evp.h>

#include "core/hle/service/ssl/ssl_trust.h"

using namespace Service::SSL;

namespace {

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAILED line %d: %s\n", __LINE__, #cond);                         \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)

EVP_PKEY* NewKey() {
    EVP_PKEY* const key = EVP_EC_gen("P-256");
    CHECK(key != nullptr);
    return key;
}

void AddExtension(X509* cert, X509* issuer, int nid, const char* value) {
    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, issuer, cert, nullptr, nullptr, 0);
    X509_EXTENSION* const ext = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value);
    CHECK(ext != nullptr);
    CHECK(X509_add_ext(cert, ext, -1) == 1);
    X509_EXTENSION_free(ext);
}

// A certificate for `subject`, signed by `issuer_key` (itself when issuer is null).
X509* MakeCert(const char* subject, EVP_PKEY* key, X509* issuer, EVP_PKEY* issuer_key, bool ca,
               const char* san) {
    X509* const cert = X509_new();
    CHECK(cert != nullptr);
    X509_set_version(cert, 2);
    static long serial = 1;
    ASN1_INTEGER_set(X509_get_serialNumber(cert), serial++);
    X509_gmtime_adj(X509_getm_notBefore(cert), -3600);
    X509_gmtime_adj(X509_getm_notAfter(cert), 3600);
    X509_NAME* const name = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>(subject), -1, -1, 0);
    X509_set_issuer_name(cert, issuer ? X509_get_subject_name(issuer) : name);
    X509_set_pubkey(cert, key);
    AddExtension(cert, issuer ? issuer : cert, NID_basic_constraints,
                 ca ? "critical,CA:TRUE" : "critical,CA:FALSE");
    if (ca) {
        AddExtension(cert, issuer ? issuer : cert, NID_key_usage, "critical,keyCertSign,cRLSign");
    } else {
        AddExtension(cert, issuer, NID_ext_key_usage, "serverAuth");
    }
    if (san != nullptr) {
        AddExtension(cert, issuer ? issuer : cert, NID_subject_alt_name, san);
    }
    CHECK(X509_sign(cert, issuer_key, EVP_sha256()) > 0);
    return cert;
}

std::string ToPem(X509* cert) {
    BIO* const bio = BIO_new(BIO_s_mem());
    CHECK(PEM_write_bio_X509(bio, cert) == 1);
    char* data = nullptr;
    const long size = BIO_get_mem_data(bio, &data);
    std::string pem(data, static_cast<std::size_t>(size));
    BIO_free(bio);
    return pem;
}

} // namespace

int main() {
    EVP_PKEY* const ca_key = NewKey();
    X509* const ca = MakeCert("OpenPak Test CA", ca_key, nullptr, ca_key, true, nullptr);

    EVP_PKEY* const leaf_key = NewKey();
    X509* const leaf = MakeCert("accounts.nintendo.com", leaf_key, ca, ca_key, false,
                                "DNS:accounts.nintendo.com,DNS:*.baas.nintendo.com,"
                                "IP:145.241.199.19");

    EVP_PKEY* const rogue_key = NewKey();
    X509* const rogue_ca = MakeCert("Somebody Else", rogue_key, nullptr, rogue_key, true, nullptr);
    X509* const rogue_leaf = MakeCert("accounts.nintendo.com", leaf_key, rogue_ca, rogue_key, false,
                                      "DNS:accounts.nintendo.com");

    // The pinned CA file: a leaf in it is refused, the CA is added.
    X509_STORE* const store = X509_STORE_new();
    CHECK(Trust::AddPemCaCertificates(store, ToPem(leaf)) == 0);
    CHECK(Trust::AddPemCaCertificates(store, ToPem(leaf) + ToPem(ca)) == 1);
    CHECK(Trust::AddPemCaCertificates(store, "") == 0);
    CHECK(Trust::PemCaToDer(ToPem(leaf)).empty());
    CHECK(!Trust::PemCaToDer(ToPem(leaf) + ToPem(ca)).empty());

    // A good OpenPak chain, by name, by wildcard and by IP SAN.
    CHECK(Trust::VerifyChain(store, leaf, nullptr, "accounts.nintendo.com") == X509_V_OK);
    CHECK(Trust::VerifyChain(store, leaf, nullptr, "e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com") ==
          X509_V_OK);
    CHECK(Trust::VerifyChain(store, leaf, nullptr, "145.241.199.19") == X509_V_OK);

    // A wrong name is never excused, nor a wrong address, nor no name at all.
    CHECK(Trust::VerifyChain(store, leaf, nullptr, "api.accounts.nintendo.com") ==
          X509_V_ERR_HOSTNAME_MISMATCH);
    CHECK(Trust::VerifyChain(store, leaf, nullptr, "evilnintendo.com") ==
          X509_V_ERR_HOSTNAME_MISMATCH);
    CHECK(Trust::VerifyChain(store, leaf, nullptr, "a.b.baas.nintendo.com") ==
          X509_V_ERR_HOSTNAME_MISMATCH);
    CHECK(Trust::VerifyChain(store, leaf, nullptr, "145.241.199.20") == X509_V_ERR_IP_ADDRESS_MISMATCH);
    CHECK(Trust::VerifyChain(store, leaf, nullptr, "") != X509_V_OK);

    // An untrusted chain is refused whatever name it carries.
    const int rogue = Trust::VerifyChain(store, rogue_leaf, nullptr, "accounts.nintendo.com");
    CHECK(rogue == X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY ||
          rogue == X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT);
    // The OpenPak leaf is refused by a store without the pinned CA.
    X509_STORE* const empty_store = X509_STORE_new();
    CHECK(Trust::VerifyChain(empty_store, leaf, nullptr, "accounts.nintendo.com") != X509_V_OK);

    CHECK(Trust::IsIpLiteral("145.241.199.19"));
    CHECK(Trust::IsIpLiteral("::1"));
    CHECK(!Trust::IsIpLiteral("accounts.nintendo.com"));

    X509_STORE_free(empty_store);
    X509_STORE_free(store);
    X509_free(rogue_leaf);
    X509_free(rogue_ca);
    X509_free(leaf);
    X509_free(ca);
    EVP_PKEY_free(rogue_key);
    EVP_PKEY_free(leaf_key);
    EVP_PKEY_free(ca_key);

    std::puts("ssl trust self-check: OK");
    return 0;
}

// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Self-check of Harbor's calibration binary: two new installs get different calibration, a binary
// read back is the same one, the CAL0 header and block checks are valid (and catch a flipped
// byte), the identity fields are well formed, every private key opens with its device key and
// matches its certificate, and another device key opens none of them; an enrolment with a
// throwaway device CA made here signs the proofs and takes the signed certificates. No emulator,
// no files.
// Build and run from the source root:
//
//   c++ -std=c++20 -Isrc src/core/harbor_calibration/check/calibration_check.cpp
//       src/core/harbor_calibration/cal0.cpp -lcrypto -o calibration_check
//   ./calibration_check
//
// Prints "calibration_check: all checks passed" and exits 0, or names the failed check and exits 1.
// It prints no serial, address, id or key.

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/x509.h>

#include "core/harbor_calibration/cal0.h"

namespace cal0 = Harbor::Cal0;

namespace {

int failures = 0;

void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "  ok  " : "  FAIL", what);
    if (!ok) {
        ++failures;
    }
}

bool SerialWellFormed(const std::string& serial) {
    if (serial.size() != 14 || serial.compare(0, 3, cal0::SerialPrefix) != 0) {
        return false;
    }
    u32 odd = 0;
    u32 even = 0;
    for (std::size_t i = 0; i < 10; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(serial[3 + i]))) {
            return false;
        }
        (i % 2 == 0 ? odd : even) += u32(serial[3 + i] - '0');
    }
    u32 check = (even * 3 + odd) % 10;
    check = check == 0 ? 0 : 10 - check;
    return serial[13] == char('0' + check);
}

bool LocallyAdministered(const std::array<u8, 6>& mac) {
    return (mac[0] & 0x03) == 0x02;
}

/// The RSA key's modulus is the one in the certificate block.
bool RsaMatches(std::span<const u8> bin, const cal0::Block& cert, const cal0::Block& key,
                const cal0::DeviceKey& device_key) {
    const auto der = cal0::RsaPrivateKeyDer(bin, key, device_key);
    if (!der) {
        return false;
    }
    const u8* in = der->data();
    EVP_PKEY* pkey = d2i_AutoPrivateKey(nullptr, &in, static_cast<long>(der->size()));
    BIGNUM* n = nullptr;
    std::array<u8, 0x100> modulus{};
    bool ok = pkey != nullptr && EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_N, &n) == 1 &&
              BN_bn2binpad(n, modulus.data(), 0x100) == 0x100 &&
              std::memcmp(modulus.data(), bin.data() + cert.offset + 0x108, 0x100) == 0;
    EVP_PKEY_CTX* ctx = pkey ? EVP_PKEY_CTX_new(pkey, nullptr) : nullptr;
    ok = ok && ctx != nullptr && EVP_PKEY_pairwise_check(ctx) == 1;
    EVP_PKEY_CTX_free(ctx);
    BN_free(n);
    EVP_PKEY_free(pkey);
    return ok;
}

/// The ECC scalar times the curve's generator is the point in the device certificate.
bool EccMatches(std::span<const u8> bin, const cal0::DeviceKey& device_key) {
    const auto scalar = cal0::EccDevicePrivateKey(bin, device_key);
    if (!scalar) {
        return false;
    }
    std::array<u8, 61> point{0x04};
    std::memcpy(point.data() + 1, bin.data() + cal0::EccB233DeviceCertificate.offset + 0x108, 60);
    BIGNUM* priv = BN_bin2bn(scalar->data(), static_cast<int>(scalar->size()), nullptr);
    OSSL_PARAM_BLD* build = OSSL_PARAM_BLD_new();
    OSSL_PARAM_BLD_push_utf8_string(build, OSSL_PKEY_PARAM_GROUP_NAME, "sect233r1", 0);
    OSSL_PARAM_BLD_push_BN(build, OSSL_PKEY_PARAM_PRIV_KEY, priv);
    OSSL_PARAM_BLD_push_octet_string(build, OSSL_PKEY_PARAM_PUB_KEY, point.data(), point.size());
    OSSL_PARAM* params = OSSL_PARAM_BLD_to_param(build);
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
    EVP_PKEY* pkey = nullptr;
    bool ok = params != nullptr && ctx != nullptr && EVP_PKEY_fromdata_init(ctx) == 1 &&
              EVP_PKEY_fromdata(ctx, &pkey, EVP_PKEY_KEYPAIR, params) == 1;
    EVP_PKEY_CTX* check = ok ? EVP_PKEY_CTX_new(pkey, nullptr) : nullptr;
    ok = ok && check != nullptr && EVP_PKEY_pairwise_check(check) == 1;
    EVP_PKEY_CTX_free(check);
    EVP_PKEY_free(pkey);
    EVP_PKEY_CTX_free(ctx);
    OSSL_PARAM_free(params);
    OSSL_PARAM_BLD_free(build);
    BN_free(priv);
    return ok;
}

/// The X.509 certificate parses and carries the public half of the wrapped TLS key.
bool SslMatches(std::span<const u8> bin, const cal0::DeviceKey& device_key) {
    const auto cert_der = cal0::SslCertificateDer(bin);
    const auto key_der = cal0::RsaPrivateKeyDer(bin, cal0::ExtendedSslKey, device_key);
    if (cert_der.empty() || !key_der) {
        return false;
    }
    const u8* in = cert_der.data();
    X509* cert = d2i_X509(nullptr, &in, static_cast<long>(cert_der.size()));
    const u8* key_in = key_der->data();
    EVP_PKEY* key = d2i_AutoPrivateKey(nullptr, &key_in, static_cast<long>(key_der->size()));
    const bool ok = cert != nullptr && key != nullptr && X509_check_private_key(cert, key) == 1 &&
                    X509_verify(cert, key) == 1;
    EVP_PKEY_free(key);
    X509_free(cert);
    return ok;
}

// ---- A throwaway device CA, as the server's enrolment signs ----

/// A Nintendo-format certificate signed the way the server signs it: ECDSA-SHA256 on B233 over the
/// certificate from its issuer on, r and s (30 bytes each) at 0x4.
std::vector<u8> CaSign(EVP_PKEY* ca, std::vector<u8> cert) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    std::vector<u8> der(128);
    std::size_t size = der.size();
    const bool ok = EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, ca) == 1 &&
                    EVP_DigestSign(ctx, der.data(), &size, cert.data() + 0x80,
                                   cert.size() - 0x80) == 1;
    EVP_MD_CTX_free(ctx);
    if (!ok) {
        return {};
    }
    const u8* in = der.data();
    ECDSA_SIG* sig = d2i_ECDSA_SIG(nullptr, &in, static_cast<long>(size));
    const bool placed = sig != nullptr &&
                        BN_bn2binpad(ECDSA_SIG_get0_r(sig), cert.data() + 4, 30) == 30 &&
                        BN_bn2binpad(ECDSA_SIG_get0_s(sig), cert.data() + 34, 30) == 30;
    ECDSA_SIG_free(sig);
    return placed ? cert : std::vector<u8>{};
}

bool CaVerifies(EVP_PKEY* ca, const std::vector<u8>& cert) {
    ECDSA_SIG* sig = ECDSA_SIG_new();
    ECDSA_SIG_set0(sig, BN_bin2bn(cert.data() + 4, 30, nullptr), BN_bin2bn(cert.data() + 34, 30, nullptr));
    u8* der = nullptr;
    const int size = i2d_ECDSA_SIG(sig, &der);
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    const bool ok = size > 0 && EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, ca) == 1 &&
                    EVP_DigestVerify(ctx, der, static_cast<std::size_t>(size), cert.data() + 0x80,
                                     cert.size() - 0x80) == 1;
    EVP_MD_CTX_free(ctx);
    OPENSSL_free(der);
    ECDSA_SIG_free(sig);
    return ok;
}

/// The TLS client certificate the CA issues for `public_key`.
std::vector<u8> CaIssue(EVP_PKEY* ca_key, EVP_PKEY* public_key) {
    X509* cert = X509_new();
    X509_NAME* subject = X509_NAME_new();
    X509_NAME* issuer = X509_NAME_new();
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 42);
    X509_set_version(cert, 2);
    X509_gmtime_adj(X509_getm_notBefore(cert), -3600);
    X509_gmtime_adj(X509_getm_notAfter(cert), 86400);
    X509_set_pubkey(cert, public_key);
    X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>("NX7E00000000000000-0"), -1, -1, 0);
    X509_NAME_add_entry_by_txt(issuer, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>("Test Harbor Device CA"), -1, -1, 0);
    X509_set_subject_name(cert, subject);
    X509_set_issuer_name(cert, issuer);
    std::vector<u8> out;
    if (X509_sign(cert, ca_key, EVP_sha256()) != 0) {
        const int size = i2d_X509(cert, nullptr);
        out.resize(static_cast<std::size_t>(size));
        u8* p = out.data();
        i2d_X509(cert, &p);
    }
    X509_NAME_free(issuer);
    X509_NAME_free(subject);
    X509_free(cert);
    return out;
}

EVP_PKEY* PublicOf(std::span<const u8> cert_der) {
    const u8* in = cert_der.data();
    X509* cert = d2i_X509(nullptr, &in, static_cast<long>(cert_der.size()));
    EVP_PKEY* key = cert ? X509_get_pubkey(cert) : nullptr;
    X509_free(cert);
    return key;
}

/// An RSA proof verifies against the modulus in a Nintendo-format certificate.
bool RsaProofVerifies(std::span<const u8> bin, const cal0::Block& cert,
                      const std::vector<u8>& message, const std::vector<u8>& proof) {
    BIGNUM* n = BN_bin2bn(bin.data() + cert.offset + 0x108, 0x100, nullptr);
    BIGNUM* e = BN_bin2bn(bin.data() + cert.offset + 0x208, 4, nullptr);
    OSSL_PARAM_BLD* build = OSSL_PARAM_BLD_new();
    OSSL_PARAM_BLD_push_BN(build, OSSL_PKEY_PARAM_RSA_N, n);
    OSSL_PARAM_BLD_push_BN(build, OSSL_PKEY_PARAM_RSA_E, e);
    OSSL_PARAM* params = OSSL_PARAM_BLD_to_param(build);
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr);
    EVP_PKEY* key = nullptr;
    EVP_PKEY_fromdata_init(ctx);
    EVP_PKEY_fromdata(ctx, &key, EVP_PKEY_PUBLIC_KEY, params);
    EVP_MD_CTX* md = EVP_MD_CTX_new();
    const bool ok = key != nullptr &&
                    EVP_DigestVerifyInit(md, nullptr, EVP_sha256(), nullptr, key) == 1 &&
                    EVP_DigestVerify(md, proof.data(), proof.size(), message.data(),
                                     message.size()) == 1;
    EVP_MD_CTX_free(md);
    EVP_PKEY_free(key);
    EVP_PKEY_CTX_free(ctx);
    OSSL_PARAM_free(params);
    OSSL_PARAM_BLD_free(build);
    BN_free(e);
    BN_free(n);
    return ok;
}

bool EccProofVerifies(std::span<const u8> bin, const std::vector<u8>& message,
                      const std::vector<u8>& proof) {
    std::array<u8, 61> point{0x04};
    std::memcpy(point.data() + 1, bin.data() + cal0::EccB233DeviceCertificate.offset + 0x108, 60);
    OSSL_PARAM_BLD* build = OSSL_PARAM_BLD_new();
    OSSL_PARAM_BLD_push_utf8_string(build, OSSL_PKEY_PARAM_GROUP_NAME, "sect233r1", 0);
    OSSL_PARAM_BLD_push_octet_string(build, OSSL_PKEY_PARAM_PUB_KEY, point.data(), point.size());
    OSSL_PARAM* params = OSSL_PARAM_BLD_to_param(build);
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
    EVP_PKEY* key = nullptr;
    EVP_PKEY_fromdata_init(ctx);
    EVP_PKEY_fromdata(ctx, &key, EVP_PKEY_PUBLIC_KEY, params);
    EVP_MD_CTX* md = EVP_MD_CTX_new();
    const bool ok = key != nullptr &&
                    EVP_DigestVerifyInit(md, nullptr, EVP_sha256(), nullptr, key) == 1 &&
                    EVP_DigestVerify(md, proof.data(), proof.size(), message.data(),
                                     message.size()) == 1;
    EVP_MD_CTX_free(md);
    EVP_PKEY_free(key);
    EVP_PKEY_CTX_free(ctx);
    OSSL_PARAM_free(params);
    OSSL_PARAM_BLD_free(build);
    return ok;
}

void EnrolmentChecks(const std::vector<u8>& a, const cal0::DeviceKey& key_a,
                     const cal0::DeviceKey& key_b) {
    Check(!cal0::IsEnrolled(a), "a new binary is not enrolled");

    const std::vector<u8> message{'n', 'o', 'n', 'c', 'e', 1, 2, 3};
    const auto proofs = cal0::SignProofs(a, key_a, message);
    Check(proofs && EccProofVerifies(a, message, proofs->ecc) &&
              RsaProofVerifies(a, cal0::Rsa2048ETicketCertificate, message, proofs->eticket) &&
              RsaProofVerifies(a, cal0::Rsa2048DeviceCertificate, message, proofs->device),
          "proofs: the ECC, eTicket and device keys sign the challenge");
    {
        EVP_PKEY* tls = PublicOf(cal0::SslCertificateDer(a));
        EVP_MD_CTX* md = EVP_MD_CTX_new();
        Check(proofs && tls != nullptr &&
                  EVP_DigestVerifyInit(md, nullptr, EVP_sha256(), nullptr, tls) == 1 &&
                  EVP_DigestVerify(md, proofs->tls.data(), proofs->tls.size(), message.data(),
                                   message.size()) == 1,
              "proofs: the TLS key signs the challenge");
        EVP_MD_CTX_free(md);
        EVP_PKEY_free(tls);
    }
    Check(!cal0::SignProofs(a, key_b, message), "proofs: another device key signs nothing");

    EVP_PKEY* ca_ecc = EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "sect233r1");
    EVP_PKEY* ca_tls = EVP_RSA_gen(2048);
    EVP_PKEY* own_tls = PublicOf(cal0::SslCertificateDer(a));
    cal0::SignedCertificates signed_certs{
        .ecc = CaSign(ca_ecc, cal0::Certificate(a, cal0::EccB233DeviceCertificate)),
        .eticket = CaSign(ca_ecc, cal0::Certificate(a, cal0::Rsa2048ETicketCertificate)),
        .device = CaSign(ca_ecc, cal0::Certificate(a, cal0::Rsa2048DeviceCertificate)),
        .tls = CaIssue(ca_tls, own_tls),
    };

    // Refused: a body that is not this console's, a TLS certificate for another key.
    std::vector<u8> untouched = a;
    auto other_body = signed_certs;
    other_body.eticket[0x150] ^= 1;
    Check(!cal0::ApplySignedCertificates(untouched, other_body) && untouched == a,
          "enrolment: a certificate with another body is refused, the binary unchanged");
    auto other_key = signed_certs;
    EVP_PKEY* stranger = EVP_RSA_gen(2048);
    other_key.tls = CaIssue(ca_tls, stranger);
    Check(!cal0::ApplySignedCertificates(untouched, other_key) && untouched == a,
          "enrolment: a TLS certificate for another key is refused");
    auto unsigned_ecc = signed_certs;
    unsigned_ecc.ecc = cal0::Certificate(a, cal0::EccB233DeviceCertificate);
    Check(!cal0::ApplySignedCertificates(untouched, unsigned_ecc) && untouched == a,
          "enrolment: an unsigned certificate is refused");

    std::vector<u8> enrolled = a;
    Check(cal0::ApplySignedCertificates(enrolled, signed_certs) && cal0::Verify(enrolled) &&
              cal0::IsEnrolled(enrolled),
          "enrolment: signed certificates taken, every check passes, enrolled");
    Check(CaVerifies(ca_ecc, cal0::Certificate(enrolled, cal0::EccB233DeviceCertificate)) &&
              CaVerifies(ca_ecc, cal0::Certificate(enrolled, cal0::Rsa2048ETicketCertificate)) &&
              CaVerifies(ca_ecc, cal0::Certificate(enrolled, cal0::Rsa2048DeviceCertificate)),
          "enrolment: the three Nintendo-format certificates carry the CA's signature");
    Check(cal0::SslCertificateDer(enrolled) == signed_certs.tls && EccMatches(enrolled, key_a) &&
              cal0::DeviceId(enrolled) == cal0::DeviceId(a) &&
              cal0::Serial(enrolled) == cal0::Serial(a),
          "enrolment: same identity and keys, the issued TLS certificate in place");
    {
        const auto cert_der = cal0::SslCertificateDer(enrolled);
        const auto key_der = cal0::RsaPrivateKeyDer(enrolled, cal0::ExtendedSslKey, key_a);
        const u8* in = cert_der.data();
        X509* cert = d2i_X509(nullptr, &in, static_cast<long>(cert_der.size()));
        const u8* key_in = key_der ? key_der->data() : nullptr;
        EVP_PKEY* key = key_der ? d2i_AutoPrivateKey(nullptr, &key_in, static_cast<long>(key_der->size())) : nullptr;
        Check(cert != nullptr && key != nullptr && X509_check_private_key(cert, key) == 1 &&
                  X509_verify(cert, ca_tls) == 1,
              "enrolment: the TLS key opens and matches the CA-issued certificate");
        EVP_PKEY_free(key);
        X509_free(cert);
    }
    EVP_PKEY_free(stranger);
    EVP_PKEY_free(own_tls);
    EVP_PKEY_free(ca_tls);
    EVP_PKEY_free(ca_ecc);
}

} // namespace

int main() {
    const cal0::DeviceKey key_a = cal0::NewDeviceKey();
    const cal0::DeviceKey key_b = cal0::NewDeviceKey();
    const auto a = cal0::Generate(key_a);
    const auto b = cal0::Generate(key_b);

    Check(a.size() == cal0::BinarySize && b.size() == cal0::BinarySize, "two binaries made");
    std::string why;
    Check(cal0::Verify(a, &why), "first binary: header CRC, body hash, every block check");
    Check(cal0::Verify(b), "second binary: same checks");

    Check(key_a != key_b, "two installs: different device keys");
    Check(cal0::Serial(a) != cal0::Serial(b), "two installs: different serial numbers");
    Check(cal0::DeviceId(a) != cal0::DeviceId(b), "two installs: different device ids");
    Check(cal0::WlanMac(a) != cal0::WlanMac(b), "two installs: different Wi-Fi MACs");
    Check(cal0::BluetoothAddress(a) != cal0::BluetoothAddress(b),
          "two installs: different Bluetooth addresses");
    Check(cal0::SslCertificateDer(a) != cal0::SslCertificateDer(b),
          "two installs: different TLS certificates");
    Check(std::memcmp(a.data() + cal0::EccB233DeviceCertificate.offset,
                      b.data() + cal0::EccB233DeviceCertificate.offset,
                      cal0::EccB233CertificateSize) != 0,
          "two installs: different device certificates");

    // A binary read back from disk is these bytes; reading must give the same identity.
    const std::vector<u8> reread(a.begin(), a.end());
    Check(cal0::Verify(reread) && cal0::Serial(reread) == cal0::Serial(a) &&
              cal0::DeviceId(reread) == cal0::DeviceId(a) &&
              cal0::EccDevicePrivateKey(reread, key_a) == cal0::EccDevicePrivateKey(a, key_a),
          "reload: same serial, device id and keys");

    Check(SerialWellFormed(cal0::Serial(a)) && SerialWellFormed(cal0::Serial(b)),
          "serial: HBW + ten digits + check digit (never a console's X prefix)");
    Check((cal0::DeviceId(a) >> 56) == (cal0::DeviceIdPrefix >> 56) &&
              (cal0::DeviceId(b) >> 56) == (cal0::DeviceIdPrefix >> 56),
          "device id: Harbor's top byte (a console's fuse id has top byte 0)");
    Check(LocallyAdministered(cal0::WlanMac(a)) && LocallyAdministered(cal0::BluetoothAddress(a)),
          "addresses: locally administered unicast");

    Check(EccMatches(a, key_a), "ECC B233 device key opens and matches its certificate");
    Check(RsaMatches(a, cal0::Rsa2048ETicketCertificate, cal0::ExtendedRsa2048ETicketKey, key_a),
          "eTicket RSA-2048 key opens and matches its certificate");
    Check(RsaMatches(a, cal0::Rsa2048DeviceCertificate, cal0::ExtendedRsa2048DeviceKey, key_a),
          "RSA-2048 device key opens and matches its certificate");
    Check(SslMatches(a, key_a), "TLS key opens and matches its X.509 certificate");
    Check(!cal0::EccDevicePrivateKey(a, key_b) &&
              !cal0::RsaPrivateKeyDer(a, cal0::ExtendedRsa2048ETicketKey, key_b) &&
              !cal0::RsaPrivateKeyDer(a, cal0::ExtendedSslKey, key_b),
          "another install's device key opens none of them");
    Check(!cal0::IsSigned(a, cal0::EccB233DeviceCertificate) &&
              !cal0::IsSigned(a, cal0::Rsa2048ETicketCertificate),
          "certificates unsigned until OpenPak's device CA signs them");

    Check(cal0::DeriveKey(key_a, "fs-save-transfer") == cal0::DeriveKey(key_a, "fs-save-transfer") &&
              cal0::DeriveKey(key_a, "fs-save-transfer") != cal0::DeriveKey(key_b, "fs-save-transfer") &&
              cal0::DeriveKey(key_a, "fs-save-transfer") != cal0::DeriveKey(key_a, "other"),
          "derived keys: stable per install and label, different across installs and labels");

    EnrolmentChecks(a, key_a, key_b);

    // New key pairs for a NAND whose device key is gone: identity kept, old key no longer opens.
    std::vector<u8> moved = a;
    const cal0::DeviceKey key_c = cal0::NewDeviceKey();
    Check(cal0::RegenerateKeys(moved, key_c) && cal0::Verify(moved) &&
              cal0::Serial(moved) == cal0::Serial(a) && cal0::DeviceId(moved) == cal0::DeviceId(a) &&
              cal0::WlanMac(moved) == cal0::WlanMac(a) && EccMatches(moved, key_c) &&
              !cal0::EccDevicePrivateKey(moved, key_a),
          "new key pairs: same serial, id and MAC; only the new device key opens them");

    // A console backup's sealed device key: the right passphrase on the right console opens it.
    const auto sealed = cal0::SealDeviceKey(key_a, cal0::DeviceId(a), "correct horse battery");
    Check(!sealed.empty() &&
              cal0::OpenDeviceKey(sealed, cal0::DeviceId(a), "correct horse battery") == key_a,
          "sealed device key: opens with its passphrase on its console");
    Check(!cal0::OpenDeviceKey(sealed, cal0::DeviceId(a), "wrong horse battery") &&
              !cal0::OpenDeviceKey(sealed, cal0::DeviceId(b), "correct horse battery"),
          "sealed device key: a wrong passphrase or another console opens nothing");
    auto tampered = sealed;
    tampered[40] ^= 0x01;
    Check(!cal0::OpenDeviceKey(tampered, cal0::DeviceId(a), "correct horse battery"),
          "sealed device key: a flipped byte opens nothing");

    // Every check notices a flipped byte.
    bool all_caught = true;
    for (const cal0::Block& block : cal0::AllBlocks()) {
        std::vector<u8> broken = a;
        broken[block.offset] ^= 0x01;
        // Fix the body hash so only the block's own check can fail.
        cal0::Seal(broken);
        all_caught = all_caught && !cal0::Verify(broken);
    }
    Check(all_caught, "a flipped byte in any block fails that block's check");
    std::vector<u8> broken_header = a;
    broken_header[0x0C] ^= 0x01;
    Check(!cal0::Verify(broken_header), "a flipped header byte fails the header CRC");
    std::vector<u8> broken_body = a;
    broken_body[0x7000] ^= 0x01;
    Check(!cal0::Verify(broken_body), "a flipped body byte outside every block fails the body hash");

    if (failures != 0) {
        std::printf("calibration_check: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("calibration_check: all checks passed\n");
    return 0;
}

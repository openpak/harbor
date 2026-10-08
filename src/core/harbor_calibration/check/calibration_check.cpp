// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Self-check of Harbor's calibration binary: two new installs get different calibration, a binary
// read back is the same one, the CAL0 header and block checks are valid (and catch a flipped
// byte), the identity fields are well formed, every private key opens with its device key and
// matches its certificate, and another device key opens none of them. No emulator, no files.
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

    // New key pairs for a NAND whose device key is gone: identity kept, old key no longer opens.
    std::vector<u8> moved = a;
    const cal0::DeviceKey key_c = cal0::NewDeviceKey();
    Check(cal0::RegenerateKeys(moved, key_c) && cal0::Verify(moved) &&
              cal0::Serial(moved) == cal0::Serial(a) && cal0::DeviceId(moved) == cal0::DeviceId(a) &&
              cal0::WlanMac(moved) == cal0::WlanMac(a) && EccMatches(moved, key_c) &&
              !cal0::EccDevicePrivateKey(moved, key_a),
          "new key pairs: same serial, id and MAC; only the new device key opens them");

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

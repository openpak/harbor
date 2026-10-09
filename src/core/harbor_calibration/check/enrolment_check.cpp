// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// The enrolment against a real nx-baas, without the emulator: this tool plays a Harbor console's
// calibration (cal0.cpp and enrolment_wire.cpp, the code Harbor runs) and enrolment_check.sh drives
// it against a local nx-baas with throwaway CA keys. Build from the source root:
//
//   c++ -std=c++20 -Isrc -I<nlohmann_json>/single_include
//       src/core/harbor_calibration/check/enrolment_check.cpp
//       src/core/harbor_calibration/cal0.cpp src/core/harbor_calibration/enrolment_wire.cpp
//       -lcrypto -o enrolment_check
//
//   enrolment_check new <dir>                  a new console: cal0.bin, its device key (test files
//                                              only), and its TLS client pair tls.pem / tls.key
//   enrolment_check rekey <dir> <copy>         the same id and serial with new keys (a copied NAND
//                                              that made its own)
//   enrolment_check request <dir> <challenge>  the enrolment request for a challenge answer (file)
//   enrolment_check apply <dir> <reply>        take the signed certificates (file); tls.pem becomes
//                                              the CA-issued certificate
//
// Prints no id, serial or key.

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include <openssl/pem.h>
#include <openssl/x509.h>

#include "core/harbor_calibration/cal0.h"
#include "core/harbor_calibration/enrolment_wire.h"

namespace fs = std::filesystem;
namespace cal0 = Harbor::Cal0;

namespace {

std::vector<u8> Read(const fs::path& path) {
    std::ifstream file{path, std::ios::binary};
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void Write(const fs::path& path, std::span<const u8> data) {
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

cal0::DeviceKey Key(const fs::path& dir) {
    const auto bytes = Read(dir / "device-key.bin");
    cal0::DeviceKey key{};
    std::copy_n(bytes.begin(), std::min(bytes.size(), key.size()), key.begin());
    return key;
}

/// The console's TLS client pair, as Harbor writes it for a server.
bool WriteTlsPair(const fs::path& dir, const std::vector<u8>& bin, const cal0::DeviceKey& key) {
    const auto cert_der = cal0::SslCertificateDer(bin);
    const auto key_der = cal0::RsaPrivateKeyDer(bin, cal0::ExtendedSslKey, key);
    if (!key_der) {
        return false;
    }
    const u8* cert_in = cert_der.data();
    const u8* key_in = key_der->data();
    X509* cert = d2i_X509(nullptr, &cert_in, static_cast<long>(cert_der.size()));
    EVP_PKEY* pkey = d2i_AutoPrivateKey(nullptr, &key_in, static_cast<long>(key_der->size()));
    FILE* cert_file = std::fopen((dir / "tls.pem").string().c_str(), "wb");
    FILE* key_file = std::fopen((dir / "tls.key").string().c_str(), "wb");
    const bool ok = cert != nullptr && pkey != nullptr && cert_file != nullptr &&
                    key_file != nullptr && PEM_write_X509(cert_file, cert) != 0 &&
                    PEM_write_PrivateKey(key_file, pkey, nullptr, nullptr, 0, nullptr, nullptr) != 0;
    if (cert_file != nullptr) {
        std::fclose(cert_file);
    }
    if (key_file != nullptr) {
        std::fclose(key_file);
    }
    X509_free(cert);
    EVP_PKEY_free(pkey);
    return ok;
}

int Fail(const char* why) {
    std::cerr << "enrolment_check: " << why << "\n";
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        return Fail("usage: new <dir> | rekey <dir> <copy> | request <dir> <challenge> | "
                    "apply <dir> <reply>");
    }
    const std::string verb = argv[1];
    const fs::path dir = argv[2];

    if (verb == "new") {
        fs::create_directories(dir);
        const auto key = cal0::NewDeviceKey();
        const auto bin = cal0::Generate(key);
        if (bin.empty()) {
            return Fail("no calibration");
        }
        Write(dir / "cal0.bin", bin);
        Write(dir / "device-key.bin", key);
        return WriteTlsPair(dir, bin, key) ? 0 : Fail("no TLS pair");
    }
    if (verb == "rekey" && argc == 4) {
        const fs::path copy = argv[3];
        fs::create_directories(copy);
        auto bin = Read(dir / "cal0.bin");
        const auto key = cal0::NewDeviceKey();
        if (!cal0::RegenerateKeys(bin, key)) {
            return Fail("no new keys");
        }
        Write(copy / "cal0.bin", bin);
        Write(copy / "device-key.bin", key);
        return WriteTlsPair(copy, bin, key) ? 0 : Fail("no TLS pair");
    }

    if (argc != 4) {
        return Fail("missing file");
    }
    const auto bin = Read(dir / "cal0.bin");
    const auto key = Key(dir);
    const auto body = Read(argv[3]);
    const std::string text(body.begin(), body.end());
    if (!cal0::Verify(bin)) {
        return Fail("calibration fails its checks");
    }

    if (verb == "request") {
        const auto challenge = Harbor::Enrolment::ParseChallenge(text);
        if (!challenge) {
            return Fail("no challenge");
        }
        const auto proofs = cal0::SignProofs(
            bin, key, Harbor::Enrolment::ProofMessage(challenge->raw, cal0::DeviceId(bin)));
        if (!proofs) {
            return Fail("the keys did not sign");
        }
        std::cout << Harbor::Enrolment::RequestBody(bin, *proofs, *challenge);
        return 0;
    }
    if (verb == "apply") {
        const auto certificates = Harbor::Enrolment::ParseSigned(text);
        if (!certificates) {
            return Fail("no certificates in the answer");
        }
        auto enrolled = bin;
        if (!cal0::ApplySignedCertificates(enrolled, *certificates) || !cal0::Verify(enrolled) ||
            !cal0::IsEnrolled(enrolled)) {
            return Fail("the certificates were not taken");
        }
        Write(dir / "cal0.bin", enrolled);
        if (!WriteTlsPair(dir, enrolled, key)) {
            return Fail("the TLS key does not open");
        }
        std::cout << "enrolled\n";
        return 0;
    }
    return Fail("unknown command");
}

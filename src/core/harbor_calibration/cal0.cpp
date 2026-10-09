// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/param_build.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/x509.h>

#include "core/harbor_calibration/cal0.h"

namespace Harbor::Cal0 {

namespace {

constexpr u32 HeaderVersion = 7;
constexpr u32 BodySize = BinarySize - HeaderSize;

// Nintendo certificate fields: signature type, key type.
constexpr u32 SignatureEcdsaSha256 = 0x00010005;
constexpr u32 KeyTypeRsa2048 = 1;
constexpr u32 KeyTypeEccB233 = 2;
constexpr std::size_t EccScalarSize = 30;
constexpr std::size_t EccPointSize = 0x3C;

// Offsets inside a certificate signed with ECDSA (both Harbor's certificate kinds are).
constexpr std::size_t CertSignature = 0x004;
constexpr std::size_t CertIssuer = 0x080;
constexpr std::size_t CertKeyType = 0x0C0;
constexpr std::size_t CertName = 0x0C4;
constexpr std::size_t CertPublicKey = 0x108;

// A wrapped key: a 12-byte nonce in a 16-byte field, the ciphertext, the 16-byte GCM tag.
constexpr std::size_t WrapIv = 0x10;
constexpr std::size_t WrapTag = 0x10;

constexpr u16 Crc16Table[] = {
    0x0000, 0xCC01, 0xD801, 0x1400, 0xF001, 0x3C00, 0x2800, 0xE401,
    0xA001, 0x6C00, 0x7800, 0xB401, 0x5000, 0x9C01, 0x8801, 0x4400,
};

constexpr Block Blocks[] = {
    ConfigurationId1,
    WlanCountryCodes,
    WlanMacAddress,
    BdAddress,
    AccelerometerOffset,
    AccelerometerScale,
    GyroscopeOffset,
    GyroscopeScale,
    SerialNumber,
    EccB233DeviceCertificate,
    SslKey,
    SslCertificateSize,
    SslCertificate,
    RandomNumber,
    GameCardCertificate,
    Rsa2048ETicketCertificate,
    BatteryLot,
    SpeakerCalibrationValue,
    RegionCode,
    ProductModel,
    ColorVariation,
    ExtendedEccB233DeviceKey,
    ExtendedRsa2048ETicketKey,
    ExtendedSslKey,
    LcdVendorId,
    ExtendedRsa2048DeviceKey,
    Rsa2048DeviceCertificate,
    UsbTypeCPowerSourceCircuit,
    ConsoleSixAxisSensorModuleType,
    ConsoleSixAxisSensorHorizontalOffset,
    BatteryVersion,
};

struct PkeyFree {
    void operator()(EVP_PKEY* key) const {
        EVP_PKEY_free(key);
    }
};
using Pkey = std::unique_ptr<EVP_PKEY, PkeyFree>;

struct BnFree {
    void operator()(BIGNUM* bn) const {
        BN_clear_free(bn);
    }
};
using Bn = std::unique_ptr<BIGNUM, BnFree>;

void PutBe32(u8* at, u32 value) {
    at[0] = u8(value >> 24);
    at[1] = u8(value >> 16);
    at[2] = u8(value >> 8);
    at[3] = u8(value);
}

void PutLe16(u8* at, u16 value) {
    at[0] = u8(value);
    at[1] = u8(value >> 8);
}

u8* At(std::vector<u8>& cal0, const Block& block) {
    return cal0.data() + block.offset;
}

std::size_t DataSize(const Block& block) {
    return block.size - (block.sha256 ? SHA256_DIGEST_LENGTH : 2);
}

void Sha256(const u8* data, std::size_t size, u8* out) {
    SHA256(data, size, out);
}

std::size_t HashedSize(std::span<const u8> cal0, const Block& block) {
    if (block.offset != SslCertificate.offset) {
        return DataSize(block);
    }
    u64 size = 0;
    std::memcpy(&size, cal0.data() + SslCertificateSize.offset, sizeof(size));
    return std::min<u64>(size, DataSize(block));
}

void Close(std::vector<u8>& cal0, const Block& block) {
    u8* data = At(cal0, block);
    const std::size_t size = DataSize(block);
    if (block.sha256) {
        Sha256(data, HashedSize(cal0, block), data + size);
    } else {
        PutLe16(data + size, Crc16(data, size));
    }
}

bool Random(u8* out, std::size_t size) {
    return RAND_bytes(out, static_cast<int>(size)) == 1;
}

/// Ten random digits and the check digit consoles use (weights 1 and 3, as on a 3DS or Switch).
bool Digits(char* out) {
    std::array<u8, 10> digits{};
    for (auto& digit : digits) {
        u8 byte = 0;
        do {
            if (!Random(&byte, 1)) {
                return false;
            }
        } while (byte >= 250);
        digit = u8(byte % 10);
    }
    u32 odd = 0;
    u32 even = 0;
    for (std::size_t i = 0; i < digits.size(); i += 2) {
        odd += digits[i];
        even += digits[i + 1];
    }
    u32 check = (even * 3 + odd) % 10;
    if (check != 0) {
        check = 10 - check;
    }
    for (std::size_t i = 0; i < digits.size(); ++i) {
        out[i] = char('0' + digits[i]);
    }
    out[digits.size()] = char('0' + check);
    return true;
}

std::string CertificateName(u64 device_id) {
    char name[32];
    std::snprintf(name, sizeof(name), "NX%016llX-0", static_cast<unsigned long long>(device_id));
    return name;
}

/// The unsigned body of a Nintendo-format certificate signed with ECDSA: issuer, key type, name.
/// The public key goes in after.
void CertificateHeader(u8* cert, u32 key_type, u64 device_id) {
    PutBe32(cert, SignatureEcdsaSha256);
    std::strncpy(reinterpret_cast<char*>(cert + CertIssuer), DeviceCaIssuer, 0x40);
    PutBe32(cert + CertKeyType, key_type);
    const std::string name = CertificateName(device_id);
    std::strncpy(reinterpret_cast<char*>(cert + CertName), name.c_str(), 0x40);
}

bool Wrap(u8* out, const u8* plain, std::size_t size, const DeviceKey& device_key, u32 aad) {
    const DeviceKey key = DeriveKey(device_key, "harbor-cal0-key-wrap");
    std::memset(out, 0, WrapIv);
    if (!Random(out, 12)) {
        return false;
    }
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    int length = 0;
    bool ok = ctx != nullptr &&
              EVP_EncryptInit_ex(ctx, EVP_aes_128_gcm(), nullptr, key.data(), out) == 1 &&
              EVP_EncryptUpdate(ctx, nullptr, &length, reinterpret_cast<const u8*>(&aad),
                                sizeof(aad)) == 1 &&
              EVP_EncryptUpdate(ctx, out + WrapIv, &length, plain, static_cast<int>(size)) == 1 &&
              EVP_EncryptFinal_ex(ctx, out + WrapIv + length, &length) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, WrapTag, out + WrapIv + size) == 1;
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

std::optional<std::vector<u8>> Unwrap(std::span<const u8> in, std::size_t size,
                                      const DeviceKey& device_key, u32 aad) {
    const DeviceKey key = DeriveKey(device_key, "harbor-cal0-key-wrap");
    std::vector<u8> plain(size);
    std::array<u8, WrapTag> tag{};
    std::memcpy(tag.data(), in.data() + WrapIv + size, WrapTag);
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    int length = 0;
    bool ok = ctx != nullptr &&
              EVP_DecryptInit_ex(ctx, EVP_aes_128_gcm(), nullptr, key.data(), in.data()) == 1 &&
              EVP_DecryptUpdate(ctx, nullptr, &length, reinterpret_cast<const u8*>(&aad),
                                sizeof(aad)) == 1 &&
              EVP_DecryptUpdate(ctx, plain.data(), &length, in.data() + WrapIv,
                                static_cast<int>(size)) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, WrapTag, tag.data()) == 1 &&
              EVP_DecryptFinal_ex(ctx, plain.data() + length, &length) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) {
        OPENSSL_cleanse(plain.data(), plain.size());
        return std::nullopt;
    }
    return plain;
}

bool BnToBe(const BIGNUM* bn, u8* out, std::size_t size) {
    return BN_bn2binpad(bn, out, static_cast<int>(size)) == static_cast<int>(size);
}

Bn GetBn(EVP_PKEY* key, const char* name) {
    BIGNUM* bn = nullptr;
    EVP_PKEY_get_bn_param(key, name, &bn);
    return Bn{bn};
}

// ECC B233 (sect233r1) device key: a 30-byte scalar, wrapped, and its point in the certificate.
bool MakeEccDeviceKey(std::vector<u8>& cal0, const DeviceKey& device_key, u64 device_id) {
#ifdef OPENSSL_NO_EC2M
    return false;
#else
    Pkey key{EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "sect233r1")};
    if (!key) {
        return false;
    }
    std::array<u8, 1 + 2 * EccScalarSize> point{};
    std::size_t point_size = 0;
    const Bn scalar = GetBn(key.get(), OSSL_PKEY_PARAM_PRIV_KEY);
    std::array<u8, 0x20> plain{};
    const bool ok =
        scalar && BnToBe(scalar.get(), plain.data(), EccScalarSize) &&
        EVP_PKEY_get_octet_string_param(key.get(), OSSL_PKEY_PARAM_PUB_KEY, point.data(),
                                        point.size(), &point_size) == 1 &&
        point_size == point.size() && point[0] == 0x04 &&
        Wrap(At(cal0, ExtendedEccB233DeviceKey), plain.data(), plain.size(), device_key,
             static_cast<u32>(ExtendedEccB233DeviceKey.offset));
    OPENSSL_cleanse(plain.data(), plain.size());
    if (!ok) {
        return false;
    }
    u8* cert = At(cal0, EccB233DeviceCertificate);
    std::memset(cert, 0, EccB233CertificateSize);
    CertificateHeader(cert, KeyTypeEccB233, device_id);
    std::memcpy(cert + CertPublicKey, point.data() + 1, EccPointSize);
    return true;
#endif
}

// RSA-2048 key: p, q and e wrapped (the rest follows from them); the modulus goes where the caller
// says (a certificate, or nowhere for the TLS key, whose X.509 certificate carries it).
Pkey MakeRsaKey(std::vector<u8>& cal0, const Block& key_block, const DeviceKey& device_key) {
    Pkey key{EVP_RSA_gen(2048)};
    if (!key) {
        return nullptr;
    }
    const Bn p = GetBn(key.get(), OSSL_PKEY_PARAM_RSA_FACTOR1);
    const Bn q = GetBn(key.get(), OSSL_PKEY_PARAM_RSA_FACTOR2);
    const Bn e = GetBn(key.get(), OSSL_PKEY_PARAM_RSA_E);
    std::array<u8, 0x110> plain{};
    const bool ok = p && q && e && BnToBe(p.get(), plain.data(), 0x80) &&
                    BnToBe(q.get(), plain.data() + 0x80, 0x80) &&
                    BnToBe(e.get(), plain.data() + 0x100, 4) &&
                    Wrap(At(cal0, key_block), plain.data(), plain.size(), device_key,
                         static_cast<u32>(key_block.offset));
    OPENSSL_cleanse(plain.data(), plain.size());
    return ok ? std::move(key) : nullptr;
}

bool MakeRsaCertificate(std::vector<u8>& cal0, const Block& cert_block, const Block& key_block,
                        const DeviceKey& device_key, u64 device_id) {
    const Pkey key = MakeRsaKey(cal0, key_block, device_key);
    if (!key) {
        return false;
    }
    u8* cert = At(cal0, cert_block);
    std::memset(cert, 0, Rsa2048CertificateSize);
    CertificateHeader(cert, KeyTypeRsa2048, device_id);
    const Bn n = GetBn(key.get(), OSSL_PKEY_PARAM_RSA_N);
    const Bn e = GetBn(key.get(), OSSL_PKEY_PARAM_RSA_E);
    return n && e && BnToBe(n.get(), cert + CertPublicKey, 0x100) &&
           BnToBe(e.get(), cert + CertPublicKey + 0x100, 4);
}

// The TLS client key and a self-signed X.509 certificate for it (DER, in the SslCertificate block).
bool MakeSslCertificate(std::vector<u8>& cal0, const DeviceKey& device_key) {
    const Pkey key = MakeRsaKey(cal0, ExtendedSslKey, device_key);
    if (!key) {
        return false;
    }
    X509* cert = X509_new();
    X509_NAME* name = X509_NAME_new();
    std::array<u8, 16> serial{};
    Random(serial.data(), serial.size());
    serial[0] &= 0x7F;
    BIGNUM* serial_bn = BN_bin2bn(serial.data(), static_cast<int>(serial.size()), nullptr);
    bool ok = cert != nullptr && name != nullptr && serial_bn != nullptr &&
              BN_to_ASN1_INTEGER(serial_bn, X509_get_serialNumber(cert)) != nullptr &&
              X509_set_version(cert, 2) == 1 &&
              X509_gmtime_adj(X509_getm_notBefore(cert), -86400) != nullptr &&
              X509_gmtime_adj(X509_getm_notAfter(cert), 25L * 365 * 86400) != nullptr &&
              X509_set_pubkey(cert, key.get()) == 1 &&
              X509_NAME_add_entry_by_txt(
                  name, "CN", MBSTRING_ASC,
                  reinterpret_cast<const unsigned char*>("OpenPak Harbor device"), -1, -1, 0) == 1 &&
              X509_set_subject_name(cert, name) == 1 && X509_set_issuer_name(cert, name) == 1 &&
              X509_sign(cert, key.get(), EVP_sha256()) != 0;
    if (ok) {
        const int size = i2d_X509(cert, nullptr);
        ok = size > 0 && static_cast<std::size_t>(size) <= SslCertificateMax;
        if (ok) {
            u8* out = At(cal0, SslCertificate);
            std::memset(out, 0, SslCertificateMax);
            i2d_X509(cert, &out);
            const u64 size64 = static_cast<u64>(size);
            std::memcpy(At(cal0, SslCertificateSize), &size64, sizeof(size64));
        }
    }
    BN_free(serial_bn);
    X509_NAME_free(name);
    X509_free(cert);
    return ok;
}

bool MakeKeys(std::vector<u8>& cal0, const DeviceKey& device_key, u64 device_id) {
    return MakeEccDeviceKey(cal0, device_key, device_id) &&
           MakeRsaCertificate(cal0, Rsa2048ETicketCertificate, ExtendedRsa2048ETicketKey,
                              device_key, device_id) &&
           MakeRsaCertificate(cal0, Rsa2048DeviceCertificate, ExtendedRsa2048DeviceKey, device_key,
                              device_id) &&
           MakeSslCertificate(cal0, device_key);
}

void CloseAll(std::vector<u8>& cal0) {
    for (const Block& block : Blocks) {
        Close(cal0, block);
    }
    Seal(cal0);
}

} // namespace

std::span<const Block> AllBlocks() {
    return Blocks;
}

u16 Crc16(const u8* data, std::size_t size) {
    u16 crc = 0x55AA;
    for (std::size_t i = 0; i < size; ++i) {
        u16 tmp = Crc16Table[crc & 0xF];
        crc = u16(((crc >> 4) & 0x0FFF) ^ tmp ^ Crc16Table[data[i] & 0xF]);
        tmp = Crc16Table[crc & 0xF];
        crc = u16(((crc >> 4) & 0x0FFF) ^ tmp ^ Crc16Table[(data[i] >> 4) & 0xF]);
    }
    return crc;
}

std::span<const u8> Data(std::span<const u8> cal0, const Block& block) {
    return cal0.subspan(block.offset, DataSize(block));
}

void Seal(std::vector<u8>& cal0) {
    PutLe16(cal0.data() + 0x1E, Crc16(cal0.data(), 0x1E));
    Sha256(cal0.data() + HeaderSize, BodySize, cal0.data() + 0x20);
    // The CRC covers bytes 0..0x1E only, so the hash after it is not inside it.
}

bool Verify(std::span<const u8> cal0, std::string* why) {
    const auto fail = [why](std::string reason) {
        if (why != nullptr) {
            *why = std::move(reason);
        }
        return false;
    };
    if (cal0.size() != BinarySize) {
        return fail("size");
    }
    u32 magic = 0;
    u32 body_size = 0;
    std::memcpy(&magic, cal0.data(), 4);
    std::memcpy(&body_size, cal0.data() + 8, 4);
    if (magic != Magic) {
        return fail("magic");
    }
    if (Crc16(cal0.data(), 0x1E) != u16(cal0[0x1E] | (cal0[0x1F] << 8))) {
        return fail("header crc");
    }
    if (body_size > BodySize) {
        return fail("body size");
    }
    u8 hash[SHA256_DIGEST_LENGTH];
    Sha256(cal0.data() + HeaderSize, body_size, hash);
    if (std::memcmp(hash, cal0.data() + 0x20, sizeof(hash)) != 0) {
        return fail("body hash");
    }
    for (const Block& block : Blocks) {
        const u8* data = cal0.data() + block.offset;
        const std::size_t size = DataSize(block);
        bool ok;
        if (block.sha256) {
            Sha256(data, HashedSize(cal0, block), hash);
            ok = std::memcmp(hash, data + size, sizeof(hash)) == 0;
        } else {
            ok = Crc16(data, size) == u16(data[size] | (data[size + 1] << 8));
        }
        if (!ok) {
            char at[32];
            std::snprintf(at, sizeof(at), "block 0x%04zx", block.offset);
            return fail(at);
        }
    }
    return true;
}

DeviceKey NewDeviceKey() {
    DeviceKey key{};
    Random(key.data(), key.size());
    return key;
}

DeviceKey DeriveKey(const DeviceKey& device_key, std::string_view label) {
    u8 mac[EVP_MAX_MD_SIZE];
    unsigned int size = 0;
    HMAC(EVP_sha256(), device_key.data(), static_cast<int>(device_key.size()),
         reinterpret_cast<const u8*>(label.data()), label.size(), mac, &size);
    DeviceKey out{};
    std::memcpy(out.data(), mac, out.size());
    OPENSSL_cleanse(mac, sizeof(mac));
    return out;
}

namespace {

// The sealed device key: "HBDK", version, scrypt log2(N), r, p, a zero byte, 16 bytes of salt,
// 12 bytes of nonce, the 16-byte key encrypted, the 16-byte GCM tag. The header and the device id
// are the associated data.
constexpr u32 SealMagic = 0x4B444248; // "HBDK"
constexpr u8 SealVersion = 1;
constexpr u8 ScryptLogN = 15;
constexpr u8 ScryptR = 8;
constexpr u8 ScryptP = 1;
constexpr std::size_t SealHeader = 4 + 4 + 16 + 12;
constexpr std::size_t SealSize = SealHeader + 16 + 16;

bool PassphraseKey(std::string_view passphrase, const u8* salt, u8 log_n, u8 r, u8 p,
                   std::array<u8, 32>& out) {
    return EVP_PBE_scrypt(passphrase.data(), passphrase.size(), salt, 16, u64{1} << log_n, r, p,
                          u64{256} << 20, out.data(), out.size()) == 1;
}

std::array<u8, SealHeader + 8> SealAad(const u8* header, u64 device_id) {
    std::array<u8, SealHeader + 8> aad{};
    std::memcpy(aad.data(), header, SealHeader);
    std::memcpy(aad.data() + SealHeader, &device_id, 8);
    return aad;
}

} // namespace

std::vector<u8> SealDeviceKey(const DeviceKey& device_key, u64 device_id,
                              std::string_view passphrase) {
    std::vector<u8> out(SealSize, 0);
    std::memcpy(out.data(), &SealMagic, 4);
    out[4] = SealVersion;
    out[5] = ScryptLogN;
    out[6] = ScryptR;
    out[7] = ScryptP;
    std::array<u8, 32> key{};
    if (!Random(out.data() + 8, 16 + 12) ||
        !PassphraseKey(passphrase, out.data() + 8, ScryptLogN, ScryptR, ScryptP, key)) {
        return {};
    }
    const auto aad = SealAad(out.data(), device_id);
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    int length = 0;
    const bool ok =
        ctx != nullptr &&
        EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, key.data(), out.data() + 24) == 1 &&
        EVP_EncryptUpdate(ctx, nullptr, &length, aad.data(), static_cast<int>(aad.size())) == 1 &&
        EVP_EncryptUpdate(ctx, out.data() + SealHeader, &length, device_key.data(),
                          static_cast<int>(device_key.size())) == 1 &&
        EVP_EncryptFinal_ex(ctx, out.data() + SealHeader + length, &length) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, out.data() + SealHeader + 16) == 1;
    EVP_CIPHER_CTX_free(ctx);
    OPENSSL_cleanse(key.data(), key.size());
    return ok ? out : std::vector<u8>{};
}

std::optional<DeviceKey> OpenDeviceKey(std::span<const u8> sealed, u64 device_id,
                                       std::string_view passphrase) {
    u32 magic = 0;
    if (sealed.size() != SealSize) {
        return std::nullopt;
    }
    std::memcpy(&magic, sealed.data(), 4);
    if (magic != SealMagic || sealed[4] != SealVersion || sealed[5] < 10 || sealed[5] > 20) {
        return std::nullopt;
    }
    std::array<u8, 32> key{};
    if (!PassphraseKey(passphrase, sealed.data() + 8, sealed[5], sealed[6], sealed[7], key)) {
        return std::nullopt;
    }
    const auto aad = SealAad(sealed.data(), device_id);
    std::array<u8, 16> tag{};
    std::memcpy(tag.data(), sealed.data() + SealHeader + 16, 16);
    DeviceKey device_key{};
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    int length = 0;
    const bool ok =
        ctx != nullptr &&
        EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, key.data(), sealed.data() + 24) == 1 &&
        EVP_DecryptUpdate(ctx, nullptr, &length, aad.data(), static_cast<int>(aad.size())) == 1 &&
        EVP_DecryptUpdate(ctx, device_key.data(), &length, sealed.data() + SealHeader, 16) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, tag.data()) == 1 &&
        EVP_DecryptFinal_ex(ctx, device_key.data() + length, &length) == 1;
    EVP_CIPHER_CTX_free(ctx);
    OPENSSL_cleanse(key.data(), key.size());
    if (!ok) {
        OPENSSL_cleanse(device_key.data(), device_key.size());
        return std::nullopt;
    }
    return device_key;
}

std::vector<u8> Generate(const DeviceKey& device_key) {
    std::vector<u8> cal0(BinarySize, 0);
    u8* header = cal0.data();
    std::memcpy(header, &Magic, 4);
    std::memcpy(header + 4, &HeaderVersion, 4);
    std::memcpy(header + 8, &BodySize, 4);
    PutLe16(header + 0xC, 1); // model: the original Switch, as set:sys GetProductModel answers

    constexpr char configuration_id[] = "MP_00_01_00_00";
    std::memcpy(At(cal0, ConfigurationId1), configuration_id, sizeof(configuration_id) - 1);

    // One country code; Harbor's wlan is the host's network, so nothing reads the list's meaning.
    u8* codes = At(cal0, WlanCountryCodes);
    codes[0] = 1;
    std::memcpy(codes + 8, "R1", 2);

    // Locally administered unicast addresses: never a manufacturer's.
    for (const Block& address : {WlanMacAddress, BdAddress}) {
        u8* mac = At(cal0, address);
        do {
            if (!Random(mac, 6)) {
                return {};
            }
            mac[0] = u8((mac[0] & 0xFC) | 0x02);
        } while (std::all_of(mac + 1, mac + 6, [](u8 b) { return b == 0; }));
    }

    // Six-axis sensor: no offset, unit scale (the host feeds the motion, already calibrated).
    for (const Block& scale : {AccelerometerScale, GyroscopeScale}) {
        for (int axis = 0; axis < 3; ++axis) {
            PutLe16(At(cal0, scale) + axis * 2, 0x4000);
        }
    }

    char* serial = reinterpret_cast<char*>(At(cal0, SerialNumber));
    std::memcpy(serial, SerialPrefix, 3);
    if (!Digits(serial + 3)) {
        return {};
    }

    char* lot = reinterpret_cast<char*>(At(cal0, BatteryLot));
    std::memcpy(lot, "BHACHZZAD", 9);
    if (!Digits(lot + 10)) {
        return {};
    }
    lot[9] = char('A' + (lot[10] - '0'));

    if (!Random(At(cal0, RandomNumber), DataSize(RandomNumber))) {
        return {};
    }

    PutLe16(At(cal0, RegionCode), 1);
    PutLe16(At(cal0, ProductModel), 1);

    u64 device_id = 0;
    if (!Random(reinterpret_cast<u8*>(&device_id), sizeof(device_id))) {
        return {};
    }
    device_id = DeviceIdPrefix | (device_id & ((1ULL << 56) - 1));

    if (!MakeKeys(cal0, device_key, device_id)) {
        OPENSSL_cleanse(cal0.data(), cal0.size());
        return {};
    }
    CloseAll(cal0);
    return cal0;
}

bool RegenerateKeys(std::vector<u8>& cal0, const DeviceKey& device_key) {
    if (!MakeKeys(cal0, device_key, DeviceId(cal0))) {
        return false;
    }
    CloseAll(cal0);
    return true;
}

std::string Serial(std::span<const u8> cal0) {
    const auto data = Data(cal0, SerialNumber);
    const auto end = std::find(data.begin(), data.begin() + 0x18, u8{0});
    return std::string(data.begin(), end);
}

u64 DeviceId(std::span<const u8> cal0) {
    const char* name = reinterpret_cast<const char*>(cal0.data() + EccB233DeviceCertificate.offset +
                                                     CertName);
    unsigned long long id = 0;
    if (std::sscanf(name, "NX%16llx-0", &id) != 1) {
        return 0;
    }
    return id;
}

std::array<u8, 6> WlanMac(std::span<const u8> cal0) {
    std::array<u8, 6> mac{};
    std::memcpy(mac.data(), cal0.data() + WlanMacAddress.offset, mac.size());
    return mac;
}

std::array<u8, 6> BluetoothAddress(std::span<const u8> cal0) {
    std::array<u8, 6> address{};
    std::memcpy(address.data(), cal0.data() + BdAddress.offset, address.size());
    return address;
}

std::vector<u8> SslCertificateDer(std::span<const u8> cal0) {
    const auto data = Data(cal0, SslCertificate);
    const std::size_t size = HashedSize(cal0, SslCertificate);
    return std::vector<u8>(data.begin(), data.begin() + size);
}

std::optional<std::vector<u8>> EccDevicePrivateKey(std::span<const u8> cal0, const DeviceKey& key) {
    auto plain = Unwrap(Data(cal0, ExtendedEccB233DeviceKey), 0x20, key,
                        static_cast<u32>(ExtendedEccB233DeviceKey.offset));
    if (plain) {
        plain->resize(EccScalarSize);
    }
    return plain;
}

std::optional<std::vector<u8>> RsaPrivateKeyDer(std::span<const u8> cal0, const Block& key_block,
                                                const DeviceKey& key) {
    auto plain = Unwrap(Data(cal0, key_block), 0x110, key, static_cast<u32>(key_block.offset));
    if (!plain) {
        return std::nullopt;
    }
    BN_CTX* ctx = BN_CTX_new();
    Bn p{BN_bin2bn(plain->data(), 0x80, nullptr)};
    Bn q{BN_bin2bn(plain->data() + 0x80, 0x80, nullptr)};
    Bn e{BN_bin2bn(plain->data() + 0x100, 4, nullptr)};
    OPENSSL_cleanse(plain->data(), plain->size());
    Bn n{BN_new()}, d{BN_new()}, dp{BN_new()}, dq{BN_new()}, qi{BN_new()};
    Bn p1{BN_new()}, q1{BN_new()}, phi{BN_new()};
    bool ok = ctx && p && q && e && n && d && dp && dq && qi && p1 && q1 && phi &&
              BN_mul(n.get(), p.get(), q.get(), ctx) == 1 && BN_sub(p1.get(), p.get(), BN_value_one()) == 1 &&
              BN_sub(q1.get(), q.get(), BN_value_one()) == 1 &&
              BN_mul(phi.get(), p1.get(), q1.get(), ctx) == 1 &&
              BN_mod_inverse(d.get(), e.get(), phi.get(), ctx) != nullptr &&
              BN_mod(dp.get(), d.get(), p1.get(), ctx) == 1 &&
              BN_mod(dq.get(), d.get(), q1.get(), ctx) == 1 &&
              BN_mod_inverse(qi.get(), q.get(), p.get(), ctx) != nullptr;
    std::optional<std::vector<u8>> der;
    if (ok) {
        OSSL_PARAM_BLD* build = OSSL_PARAM_BLD_new();
        OSSL_PARAM* params = nullptr;
        EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr);
        EVP_PKEY* pkey = nullptr;
        if (build != nullptr && pctx != nullptr &&
            OSSL_PARAM_BLD_push_BN(build, OSSL_PKEY_PARAM_RSA_N, n.get()) == 1 &&
            OSSL_PARAM_BLD_push_BN(build, OSSL_PKEY_PARAM_RSA_E, e.get()) == 1 &&
            OSSL_PARAM_BLD_push_BN(build, OSSL_PKEY_PARAM_RSA_D, d.get()) == 1 &&
            OSSL_PARAM_BLD_push_BN(build, OSSL_PKEY_PARAM_RSA_FACTOR1, p.get()) == 1 &&
            OSSL_PARAM_BLD_push_BN(build, OSSL_PKEY_PARAM_RSA_FACTOR2, q.get()) == 1 &&
            OSSL_PARAM_BLD_push_BN(build, OSSL_PKEY_PARAM_RSA_EXPONENT1, dp.get()) == 1 &&
            OSSL_PARAM_BLD_push_BN(build, OSSL_PKEY_PARAM_RSA_EXPONENT2, dq.get()) == 1 &&
            OSSL_PARAM_BLD_push_BN(build, OSSL_PKEY_PARAM_RSA_COEFFICIENT1, qi.get()) == 1 &&
            (params = OSSL_PARAM_BLD_to_param(build)) != nullptr &&
            EVP_PKEY_fromdata_init(pctx) == 1 &&
            EVP_PKEY_fromdata(pctx, &pkey, EVP_PKEY_KEYPAIR, params) == 1) {
            const int size = i2d_PrivateKey(pkey, nullptr);
            if (size > 0) {
                der.emplace(static_cast<std::size_t>(size));
                u8* out = der->data();
                i2d_PrivateKey(pkey, &out);
            }
        }
        EVP_PKEY_free(pkey);
        EVP_PKEY_CTX_free(pctx);
        OSSL_PARAM_free(params);
        OSSL_PARAM_BLD_free(build);
    }
    BN_CTX_free(ctx);
    return der;
}

bool IsSigned(std::span<const u8> cal0, const Block& certificate_block) {
    const u8* signature = cal0.data() + certificate_block.offset + CertSignature;
    return std::any_of(signature, signature + EccPointSize, [](u8 b) { return b != 0; });
}

std::vector<u8> Certificate(std::span<const u8> cal0, const Block& certificate_block) {
    const std::size_t size = certificate_block.offset == EccB233DeviceCertificate.offset
                                 ? EccB233CertificateSize
                                 : Rsa2048CertificateSize;
    const u8* at = cal0.data() + certificate_block.offset;
    return std::vector<u8>(at, at + size);
}

namespace {

struct X509Free {
    void operator()(X509* cert) const {
        X509_free(cert);
    }
};
using X509Ptr = std::unique_ptr<X509, X509Free>;

X509Ptr ParseX509(std::span<const u8> der) {
    const u8* in = der.data();
    X509Ptr cert{d2i_X509(nullptr, &in, static_cast<long>(der.size()))};
    if (cert && in != der.data() + der.size()) {
        return nullptr; // trailing bytes: not one certificate
    }
    return cert;
}

/// The B233 device key as an OpenSSL key: its scalar and the point in its certificate.
Pkey EccKey(std::span<const u8> cal0, const DeviceKey& device_key) {
#ifdef OPENSSL_NO_EC2M
    return nullptr;
#else
    auto scalar = EccDevicePrivateKey(cal0, device_key);
    if (!scalar) {
        return nullptr;
    }
    std::array<u8, 1 + EccPointSize> point{0x04};
    std::memcpy(point.data() + 1, cal0.data() + EccB233DeviceCertificate.offset + CertPublicKey,
                EccPointSize);
    Bn priv{BN_bin2bn(scalar->data(), static_cast<int>(scalar->size()), nullptr)};
    OPENSSL_cleanse(scalar->data(), scalar->size());
    OSSL_PARAM_BLD* build = OSSL_PARAM_BLD_new();
    OSSL_PARAM* params = nullptr;
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
    EVP_PKEY* key = nullptr;
    const bool ok =
        priv && build != nullptr && ctx != nullptr &&
        OSSL_PARAM_BLD_push_utf8_string(build, OSSL_PKEY_PARAM_GROUP_NAME, "sect233r1", 0) == 1 &&
        OSSL_PARAM_BLD_push_BN(build, OSSL_PKEY_PARAM_PRIV_KEY, priv.get()) == 1 &&
        OSSL_PARAM_BLD_push_octet_string(build, OSSL_PKEY_PARAM_PUB_KEY, point.data(),
                                         point.size()) == 1 &&
        (params = OSSL_PARAM_BLD_to_param(build)) != nullptr && EVP_PKEY_fromdata_init(ctx) == 1 &&
        EVP_PKEY_fromdata(ctx, &key, EVP_PKEY_KEYPAIR, params) == 1;
    EVP_PKEY_CTX_free(ctx);
    OSSL_PARAM_free(params);
    OSSL_PARAM_BLD_free(build);
    if (!ok) {
        EVP_PKEY_free(key);
        return nullptr;
    }
    return Pkey{key};
#endif
}

Pkey RsaKey(std::span<const u8> cal0, const Block& key_block, const DeviceKey& device_key) {
    auto der = RsaPrivateKeyDer(cal0, key_block, device_key);
    if (!der) {
        return nullptr;
    }
    const u8* in = der->data();
    Pkey key{d2i_AutoPrivateKey(nullptr, &in, static_cast<long>(der->size()))};
    OPENSSL_cleanse(der->data(), der->size());
    return key;
}

std::vector<u8> SignSha256(EVP_PKEY* key, std::span<const u8> message) {
    if (key == nullptr) {
        return {};
    }
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    std::size_t size = 0;
    std::vector<u8> out;
    if (ctx != nullptr && EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, key) == 1 &&
        EVP_DigestSign(ctx, nullptr, &size, message.data(), message.size()) == 1) {
        out.resize(size);
        if (EVP_DigestSign(ctx, out.data(), &size, message.data(), message.size()) == 1) {
            out.resize(size);
        } else {
            out.clear();
        }
    }
    EVP_MD_CTX_free(ctx);
    return out;
}

/// A Nintendo-format certificate from the CA: this binary's own, with a signature in place.
bool SameCertificateSigned(std::span<const u8> cal0, const Block& block,
                           const std::vector<u8>& from_ca) {
    const std::vector<u8> own = Certificate(cal0, block);
    if (from_ca.size() != own.size() ||
        std::memcmp(from_ca.data(), own.data(), CertSignature) != 0 ||
        std::memcmp(from_ca.data() + CertIssuer, own.data() + CertIssuer,
                    own.size() - CertIssuer) != 0) {
        return false;
    }
    const u8* signature = from_ca.data() + CertSignature;
    return std::any_of(signature, signature + EccPointSize, [](u8 b) { return b != 0; });
}

} // namespace

bool IsEnrolled(std::span<const u8> cal0) {
    if (!IsSigned(cal0, EccB233DeviceCertificate) || !IsSigned(cal0, Rsa2048ETicketCertificate) ||
        !IsSigned(cal0, Rsa2048DeviceCertificate)) {
        return false;
    }
    const X509Ptr cert = ParseX509(SslCertificateDer(cal0));
    return cert && X509_NAME_cmp(X509_get_subject_name(cert.get()),
                                 X509_get_issuer_name(cert.get())) != 0;
}

std::optional<Proofs> SignProofs(std::span<const u8> cal0, const DeviceKey& device_key,
                                 std::span<const u8> message) {
    Proofs out;
    out.ecc = SignSha256(EccKey(cal0, device_key).get(), message);
    out.eticket =
        SignSha256(RsaKey(cal0, ExtendedRsa2048ETicketKey, device_key).get(), message);
    out.device = SignSha256(RsaKey(cal0, ExtendedRsa2048DeviceKey, device_key).get(), message);
    out.tls = SignSha256(RsaKey(cal0, ExtendedSslKey, device_key).get(), message);
    if (out.ecc.empty() || out.eticket.empty() || out.device.empty() || out.tls.empty()) {
        return std::nullopt;
    }
    return out;
}

bool ApplySignedCertificates(std::vector<u8>& cal0, const SignedCertificates& certificates) {
    if (!SameCertificateSigned(cal0, EccB233DeviceCertificate, certificates.ecc) ||
        !SameCertificateSigned(cal0, Rsa2048ETicketCertificate, certificates.eticket) ||
        !SameCertificateSigned(cal0, Rsa2048DeviceCertificate, certificates.device) ||
        certificates.tls.empty() || certificates.tls.size() > SslCertificateMax) {
        return false;
    }
    // The issued TLS certificate must carry the key this binary holds.
    const X509Ptr issued = ParseX509(certificates.tls);
    const X509Ptr own = ParseX509(SslCertificateDer(cal0));
    if (!issued || !own) {
        return false;
    }
    EVP_PKEY* issued_key = X509_get0_pubkey(issued.get());
    EVP_PKEY* own_key = X509_get0_pubkey(own.get());
    if (issued_key == nullptr || own_key == nullptr || EVP_PKEY_eq(issued_key, own_key) != 1) {
        return false;
    }

    std::memcpy(At(cal0, EccB233DeviceCertificate), certificates.ecc.data(),
                EccB233CertificateSize);
    std::memcpy(At(cal0, Rsa2048ETicketCertificate), certificates.eticket.data(),
                Rsa2048CertificateSize);
    std::memcpy(At(cal0, Rsa2048DeviceCertificate), certificates.device.data(),
                Rsa2048CertificateSize);
    u8* tls = At(cal0, SslCertificate);
    std::memset(tls, 0, SslCertificateMax);
    std::memcpy(tls, certificates.tls.data(), certificates.tls.size());
    const u64 size = certificates.tls.size();
    std::memcpy(At(cal0, SslCertificateSize), &size, sizeof(size));
    for (const Block& block : {EccB233DeviceCertificate, Rsa2048ETicketCertificate,
                               Rsa2048DeviceCertificate, SslCertificateSize, SslCertificate}) {
        Close(cal0, block);
    }
    Seal(cal0);
    return true;
}

} // namespace Harbor::Cal0

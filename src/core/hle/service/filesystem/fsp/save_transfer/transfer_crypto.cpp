// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include "core/hle/service/filesystem/fsp/save_transfer/transfer_crypto.h"
#include "common/harbor_crash.h"

#include <algorithm>
#include <cstring>
#include <memory>

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <zlib.h>

namespace Service::FileSystem::SaveTransfer {

namespace {

struct CipherCtxDeleter {
    void operator()(EVP_CIPHER_CTX* ctx) const {
        EVP_CIPHER_CTX_free(ctx);
    }
};
using CipherCtx = std::unique_ptr<EVP_CIPHER_CTX, CipherCtxDeleter>;

struct PkeyDeleter {
    void operator()(EVP_PKEY* key) const {
        EVP_PKEY_free(key);
    }
};
using Pkey = std::unique_ptr<EVP_PKEY, PkeyDeleter>;

struct PkeyCtxDeleter {
    void operator()(EVP_PKEY_CTX* ctx) const {
        EVP_PKEY_CTX_free(ctx);
    }
};
using PkeyCtx = std::unique_ptr<EVP_PKEY_CTX, PkeyCtxDeleter>;

struct MdCtxDeleter {
    void operator()(EVP_MD_CTX* ctx) const {
        EVP_MD_CTX_free(ctx);
    }
};
using MdCtx = std::unique_ptr<EVP_MD_CTX, MdCtxDeleter>;

Key128 EcbDecrypt(const Key128& key, const Key128& block) {
    HARBOR_CRUMB_ONCE("fs: first OpenSSL ECB");
    Key128 out{};
    CipherCtx ctx{EVP_CIPHER_CTX_new()};
    int len = 0;
    EVP_DecryptInit_ex(ctx.get(), EVP_aes_128_ecb(), nullptr, key.data(), nullptr);
    EVP_CIPHER_CTX_set_padding(ctx.get(), 0);
    EVP_DecryptUpdate(ctx.get(), out.data(), &len, block.data(), static_cast<int>(block.size()));
    return out;
}

bool Gcm(bool encrypt, const Key128& key, std::span<const u8> iv, std::span<u8> data,
         Key128& tag) {
    CipherCtx ctx{EVP_CIPHER_CTX_new()};
    if (!ctx) {
        return false;
    }
    int len = 0;
    const auto init = encrypt ? EVP_EncryptInit_ex : EVP_DecryptInit_ex;
    if (init(ctx.get(), EVP_aes_128_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(iv.size()),
                            nullptr) != 1 ||
        init(ctx.get(), nullptr, nullptr, key.data(), iv.data()) != 1) {
        return false;
    }
    // Large buffers go through in pieces (EVP lengths are int).
    std::size_t done = 0;
    while (done < data.size()) {
        const int piece = static_cast<int>(std::min<std::size_t>(data.size() - done, 1 << 24));
        const auto update = encrypt ? EVP_EncryptUpdate : EVP_DecryptUpdate;
        if (update(ctx.get(), data.data() + done, &len, data.data() + done, piece) != 1) {
            return false;
        }
        done += static_cast<std::size_t>(piece);
    }
    if (encrypt) {
        return EVP_EncryptFinal_ex(ctx.get(), nullptr, &len) == 1 &&
               EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, 16, tag.data()) == 1;
    }
    return EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, 16, tag.data()) == 1 &&
           EVP_DecryptFinal_ex(ctx.get(), nullptr, &len) == 1;
}

Pkey PublicKey(const Modulus& modulus) {
    HARBOR_CRUMB_ONCE("fs: first OpenSSL public key");
    BIGNUM* n = BN_bin2bn(modulus.data(), static_cast<int>(modulus.size()), nullptr);
    BIGNUM* e = BN_new();
    BN_set_word(e, 65537);
    OSSL_PARAM_BLD* builder = OSSL_PARAM_BLD_new();
    OSSL_PARAM_BLD_push_BN(builder, OSSL_PKEY_PARAM_RSA_N, n);
    OSSL_PARAM_BLD_push_BN(builder, OSSL_PKEY_PARAM_RSA_E, e);
    OSSL_PARAM* params = OSSL_PARAM_BLD_to_param(builder);
    EVP_PKEY* key = nullptr;
    PkeyCtx ctx{EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr)};
    if (ctx && EVP_PKEY_fromdata_init(ctx.get()) == 1) {
        EVP_PKEY_fromdata(ctx.get(), &key, EVP_PKEY_PUBLIC_KEY, params);
    }
    OSSL_PARAM_free(params);
    OSSL_PARAM_BLD_free(builder);
    BN_free(n);
    BN_free(e);
    return Pkey{key};
}

} // namespace

int MasterKeyIndex(int key_generation) {
    return std::max(key_generation - 1, 0);
}

Key128 DeriveKey(const Key128& master, const Key128& kek_generation_source,
                 const Key128& key_generation_source, const Key128& kek_source,
                 const Key128& source) {
    // GenerateAesKek: the static generation kek, then the access key.
    const Key128 generation_kek = EcbDecrypt(master, kek_generation_source);
    const Key128 access_key = EcbDecrypt(generation_kek, kek_source);
    // GenerateAesKey: LoadAesKey(access key, key generation source), then the source.
    const Key128 slot_key = EcbDecrypt(access_key, key_generation_source);
    return EcbDecrypt(slot_key, source);
}

bool GcmEncrypt(const Key128& key, std::span<const u8> iv, std::span<u8> data, Key128& tag) {
    return Gcm(true, key, iv, data, tag);
}

bool GcmDecrypt(const Key128& key, std::span<const u8> iv, std::span<u8> data,
                const Key128& tag) {
    Key128 expected = tag;
    return Gcm(false, key, iv, data, expected);
}

// AES-128 CMAC (RFC 4493) over single-block AES-ECB. It used to go through EVP_MAC "CMAC", which
// fails silently (a zero MAC, so every initial-data MAC mismatched as 2002-7006) when the
// build's OpenSSL has no usable CMAC provider.
Key128 Cmac(const Key128& key, std::span<const u8> data) {
    const auto encrypt = [&](const Key128& block) {
        Key128 out{};
        CipherCtx ctx{EVP_CIPHER_CTX_new()};
        int len = 0;
        if (!ctx || EVP_EncryptInit_ex(ctx.get(), EVP_aes_128_ecb(), nullptr, key.data(),
                                       nullptr) != 1) {
            return out;
        }
        EVP_CIPHER_CTX_set_padding(ctx.get(), 0);
        EVP_EncryptUpdate(ctx.get(), out.data(), &len, block.data(),
                          static_cast<int>(block.size()));
        return out;
    };
    const auto shift = [](const Key128& in) {
        Key128 out{};
        for (std::size_t i = 0; i < in.size(); ++i) {
            out[i] = static_cast<u8>(in[i] << 1 | (i + 1 < in.size() ? in[i + 1] >> 7 : 0));
        }
        if (in[0] & 0x80) {
            out[15] ^= 0x87;
        }
        return out;
    };
    const Key128 k1 = shift(encrypt(Key128{}));
    const Key128 k2 = shift(k1);
    const std::size_t blocks = data.empty() ? 1 : (data.size() + 15) / 16;
    const bool complete = !data.empty() && data.size() % 16 == 0;
    Key128 x{};
    for (std::size_t n = 0; n < blocks; ++n) {
        Key128 block{};
        const std::size_t at = n * 16;
        const std::size_t have = std::min<std::size_t>(16, data.size() - std::min(at, data.size()));
        std::copy_n(data.begin() + std::min(at, data.size()), have, block.begin());
        if (n + 1 == blocks) {
            if (!complete) {
                block[have] = 0x80;
            }
            const Key128& k = complete ? k1 : k2;
            for (std::size_t i = 0; i < 16; ++i) {
                block[i] ^= k[i];
            }
        }
        for (std::size_t i = 0; i < 16; ++i) {
            x[i] ^= block[i];
        }
        x = encrypt(x);
    }
    return x;
}

bool VerifyPss(const Modulus& modulus, std::span<const u8> signature, std::span<const u8> data) {
    if (signature.size() != modulus.size()) {
        return false;
    }
    const Pkey key = PublicKey(modulus);
    MdCtx md{EVP_MD_CTX_new()};
    EVP_PKEY_CTX* pctx = nullptr;
    if (!key || !md ||
        EVP_DigestVerifyInit(md.get(), &pctx, EVP_sha256(), nullptr, key.get()) != 1 ||
        EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) != 1 ||
        EVP_PKEY_CTX_set_rsa_mgf1_md(pctx, EVP_sha256()) != 1 ||
        EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, RSA_PSS_SALTLEN_AUTO) != 1) {
        return false;
    }
    return EVP_DigestVerify(md.get(), signature.data(), signature.size(), data.data(),
                            data.size()) == 1;
}

std::optional<std::array<u8, 0x100>> SignPss(const std::string& private_key_pem_path,
                                             std::span<const u8> data) {
    FILE* file = std::fopen(private_key_pem_path.c_str(), "rb");
    if (!file) {
        return std::nullopt;
    }
    Pkey key{PEM_read_PrivateKey(file, nullptr, nullptr, nullptr)};
    std::fclose(file);
    MdCtx md{EVP_MD_CTX_new()};
    EVP_PKEY_CTX* pctx = nullptr;
    std::array<u8, 0x100> signature{};
    std::size_t length = signature.size();
    if (!key || !md ||
        EVP_DigestSignInit(md.get(), &pctx, EVP_sha256(), nullptr, key.get()) != 1 ||
        EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) != 1 ||
        EVP_PKEY_CTX_set_rsa_mgf1_md(pctx, EVP_sha256()) != 1 ||
        EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, 32) != 1 ||
        EVP_DigestSign(md.get(), signature.data(), &length, data.data(), data.size()) != 1 ||
        length != signature.size()) {
        return std::nullopt;
    }
    return signature;
}

void RandomBytes(std::span<u8> out) {
    HARBOR_CRUMB_ONCE("fs: first OpenSSL RAND");
    RAND_bytes(out.data(), static_cast<int>(out.size()));
}

OpenedPackage OpenKeySeedPackage(std::span<const u8, 0x200> package, const Key128& body_key,
                                 const Modulus& modulus, const Key128& challenge) {
    KeySeedPackage copy;
    std::memcpy(&copy, package.data(), sizeof(copy));
    OpenedPackage result;
    if (copy.version >= 0x100) {
        result.error = PackageError::Version;
        return result;
    }
    // Signature ∥ Content, 0x180 bytes from +0x80, decrypted in place.
    std::span<u8> body{reinterpret_cast<u8*>(&copy.signature), 0x180};
    if (!GcmDecrypt(body_key, copy.iv, body, copy.mac)) {
        result.error = PackageError::Mac;
        return result;
    }
    const std::span<const u8> content{reinterpret_cast<const u8*>(&copy.content),
                                      sizeof(copy.content)};
    if (!VerifyPss(modulus, copy.signature, content)) {
        result.error = PackageError::Signature;
        return result;
    }
    if (copy.content.challenge != challenge) {
        result.error = PackageError::Challenge;
        return result;
    }
    result.content = copy.content;
    return result;
}

std::optional<KeySeedPackage> BuildKeySeedPackage(const KeySeedPackageContent& content,
                                                  u8 key_generation, const Key128& body_key,
                                                  const std::string& private_key_pem_path) {
    KeySeedPackage package;
    package.key_generation = key_generation;
    package.content = content;
    const auto signature = SignPss(
        private_key_pem_path,
        std::span<const u8>{reinterpret_cast<const u8*>(&package.content), sizeof(content)});
    if (!signature) {
        return std::nullopt;
    }
    package.signature = *signature;
    RandomBytes(package.iv);
    std::span<u8> body{reinterpret_cast<u8*>(&package.signature), 0x180};
    if (!GcmEncrypt(body_key, package.iv, body, package.mac)) {
        return std::nullopt;
    }
    return package;
}

std::vector<u8> EncryptStream(const Key128& key, const Key128& iv, u16 key_generation,
                              std::span<const u8> payload, Key128* out_tag) {
    AesGcmStreamHeader header;
    header.key_generation = key_generation;
    header.iv = iv;
    std::vector<u8> out(sizeof(header) + payload.size() + StreamTagSize);
    std::memcpy(out.data(), &header, sizeof(header));
    std::memcpy(out.data() + sizeof(header), payload.data(), payload.size());
    Key128 tag{};
    GcmEncrypt(key, iv, std::span<u8>{out.data() + sizeof(header), payload.size()}, tag);
    std::memcpy(out.data() + sizeof(header) + payload.size(), tag.data(), tag.size());
    if (out_tag) {
        *out_tag = tag;
    }
    return out;
}

std::optional<std::vector<u8>> DecryptStream(const Key128& key, std::span<const u8> stream) {
    if (stream.size() < sizeof(AesGcmStreamHeader) + StreamTagSize) {
        return std::nullopt;
    }
    AesGcmStreamHeader header;
    std::memcpy(&header, stream.data(), sizeof(header));
    if (header.magic != AesGcmStreamHeader{}.magic) {
        return std::nullopt;
    }
    const std::size_t size = stream.size() - sizeof(header) - StreamTagSize;
    std::vector<u8> payload(stream.begin() + sizeof(header), stream.begin() + sizeof(header) + size);
    Key128 tag{};
    std::memcpy(tag.data(), stream.data() + sizeof(header) + size, tag.size());
    if (!GcmDecrypt(key, header.iv, payload, tag)) {
        return std::nullopt;
    }
    return payload;
}

std::vector<u8> RawDeflate(std::span<const u8> data) {
    z_stream stream{};
    deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
    std::vector<u8> out(deflateBound(&stream, static_cast<uLong>(data.size())));
    stream.next_in = const_cast<Bytef*>(data.data());
    stream.avail_in = static_cast<uInt>(data.size());
    stream.next_out = out.data();
    stream.avail_out = static_cast<uInt>(out.size());
    deflate(&stream, Z_FINISH);
    out.resize(stream.total_out);
    deflateEnd(&stream);
    return out;
}

std::optional<std::vector<u8>> RawInflate(std::span<const u8> data, std::size_t size_limit) {
    z_stream stream{};
    if (inflateInit2(&stream, -15) != Z_OK) {
        return std::nullopt;
    }
    std::vector<u8> out;
    std::array<u8, 0x10000> buffer{};
    stream.next_in = const_cast<Bytef*>(data.data());
    stream.avail_in = static_cast<uInt>(data.size());
    int status = Z_OK;
    while (status == Z_OK) {
        stream.next_out = buffer.data();
        stream.avail_out = static_cast<uInt>(buffer.size());
        status = inflate(&stream, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END) {
            break;
        }
        out.insert(out.end(), buffer.data(), buffer.data() + (buffer.size() - stream.avail_out));
        if (out.size() > size_limit) {
            status = Z_DATA_ERROR;
        }
    }
    inflateEnd(&stream);
    if (status != Z_STREAM_END) {
        return std::nullopt;
    }
    return out;
}

} // namespace Service::FileSystem::SaveTransfer

// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// The cryptography of save-data transfer v2 (Save Data Cloud archives), as the 23.0.1 FS does it
// (research/olsc-re/fs-transfer.md §9): key derivation, the AES-GCM "AGS0" stream, the
// initial-data CMAC, and the key seed package (AES-GCM body, RSA-PSS-SHA256 signature).
//
// Pure functions over OpenSSL: no keys or FS constants live here. The caller supplies the
// master key and generation sources (prod.keys) and the FS constants found in the user's own
// firmware (save_transfer/fs_constants).

#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "common/common_types.h"

namespace Service::FileSystem::SaveTransfer {

using Key128 = std::array<u8, 0x10>;
using Modulus = std::array<u8, 0x100>;

/// The FS crypto configuration's key indices Harbor uses (GenerateAesKey 0x282e0).
enum class KeyIndex : int {
    KeySeedPackage = 2, ///< the key seed package body (attribute 0)
    Stream = 3,         ///< chunk and initial-data streams; source = the archive's key seed
    InitialDataMac = 5, ///< the initial-data CMAC
};

/// The key generation the console uses for streams and the initial-data MAC (attribute 0).
inline constexpr int StreamKeyGeneration = 5;

/// spl GenerateAesKek + GenerateAesKey with option 0, then the key for `source`:
/// AES-ECB-Dec(AES-ECB-Dec(AES-ECB-Dec(AES-ECB-Dec(master, kek_generation_source),
/// kek_source), key_generation_source), source). `master` is master_key[max(generation-1, 0)].
Key128 DeriveKey(const Key128& master, const Key128& kek_generation_source,
                 const Key128& key_generation_source, const Key128& kek_source,
                 const Key128& source);

/// The master-key index spl uses for a key generation (generation 0 and 1 both use 0).
int MasterKeyIndex(int key_generation);

/// AES-128-GCM over `data` in place. `iv` may be 12 or 16 bytes. Encrypt writes the tag;
/// decrypt checks it and answers whether it matched.
bool GcmEncrypt(const Key128& key, std::span<const u8> iv, std::span<u8> data, Key128& tag);
bool GcmDecrypt(const Key128& key, std::span<const u8> iv, std::span<u8> data, const Key128& tag);

/// AES-128-CMAC.
Key128 Cmac(const Key128& key, std::span<const u8> data);

/// RSASSA-PSS-SHA256 (MGF1-SHA256, any salt length), e = 65537, as FS verifies (0x27f00).
bool VerifyPss(const Modulus& modulus, std::span<const u8> signature, std::span<const u8> data);

/// Signs as above (salt length 32) with a PEM private key file: tests and server simulations
/// only. nullopt when the key cannot be read.
std::optional<std::array<u8, 0x100>> SignPss(const std::string& private_key_pem_path,
                                             std::span<const u8> data);

/// Cryptographically random bytes.
void RandomBytes(std::span<u8> out);

// ---- Key seed package (0x200 bytes, fs-transfer.md §9.2) -------------------------------------

struct KeySeedPackageContent {
    std::array<u8, 0x10> unknown{}; ///< not read by FS
    Key128 key_seed{};
    Key128 initial_data_mac{};
    Key128 challenge{};
    u8 is_local{};                  ///< 1 = a console's own "local" package (attribute 1 only)
    std::array<u8, 0x3f> reserved{};
};
static_assert(sizeof(KeySeedPackageContent) == 0x80);

struct KeySeedPackage {
    u32 version{};
    std::array<u8, 4> reserved4{};
    u8 key_generation{};
    std::array<u8, 7> reserved9{};
    Key128 iv{};
    Key128 mac{};
    std::array<u8, 0x50> reserved30{};
    std::array<u8, 0x100> signature{};
    KeySeedPackageContent content{};
};
static_assert(sizeof(KeySeedPackage) == 0x200);

enum class PackageError {
    None,
    Version,   ///< 2-7037
    Mac,       ///< 2-7002 (GCM tag)
    Signature, ///< 2-7003
    Challenge, ///< 2-7004
};

struct OpenedPackage {
    PackageError error{PackageError::None};
    KeySeedPackageContent content{};
};

/// SetKeySeedPackage's checks in FS order: version, GCM body (key `body_key` for the package's
/// key generation), PSS signature against `modulus`, the challenge.
OpenedPackage OpenKeySeedPackage(std::span<const u8, 0x200> package, const Key128& body_key,
                                 const Modulus& modulus, const Key128& challenge);

/// What the saves server does for generate_key_seed_package (tests): sign the content with a
/// PEM private key, encrypt signature ∥ content with `body_key` under a fresh IV.
std::optional<KeySeedPackage> BuildKeySeedPackage(const KeySeedPackageContent& content,
                                                  u8 key_generation, const Key128& body_key,
                                                  const std::string& private_key_pem_path);

// ---- AesGcmStream ("AGS0", fs-transfer.md §9.4) ----------------------------------------------

struct AesGcmStreamHeader {
    u32 magic{0x30534741}; ///< "AGS0"
    u16 reserved4{};
    u16 key_generation{};
    u64 reserved8{};
    Key128 iv{};
};
static_assert(sizeof(AesGcmStreamHeader) == 0x20);

inline constexpr std::size_t StreamTagSize = 0x10;

/// One stream: header ∥ AES-GCM(payload) ∥ tag. Encrypt answers the whole stream and the tag.
std::vector<u8> EncryptStream(const Key128& key, const Key128& iv, u16 key_generation,
                              std::span<const u8> payload, Key128* out_tag = nullptr);

/// The payload of a stream encrypted with `key`, or nullopt when the header or tag is wrong.
std::optional<std::vector<u8>> DecryptStream(const Key128& key, std::span<const u8> stream);

// ---- Raw DEFLATE (data chunks are deflated before encryption, zlib windowBits -15) --------------

std::vector<u8> RawDeflate(std::span<const u8> data);
std::optional<std::vector<u8>> RawInflate(std::span<const u8> data, std::size_t size_limit);

} // namespace Service::FileSystem::SaveTransfer

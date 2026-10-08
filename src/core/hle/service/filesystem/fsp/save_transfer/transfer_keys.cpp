// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include "core/hle/service/filesystem/fsp/save_transfer/transfer_keys.h"

#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include <openssl/evp.h>
#include <openssl/sha.h>

#include "common/logging.h"
#include "core/core.h"
#include "core/crypto/key_manager.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/kernel_executable.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/hle/service/filesystem/filesystem.h"

namespace Service::FileSystem::SaveTransfer {

namespace {

constexpr u64 BootImagePackageId = 0x0100000000000819;

using Sha256 = std::array<u8, 32>;

constexpr Sha256 FromHex(const char (&text)[65]) {
    Sha256 out{};
    const auto nibble = [](char c) -> u8 {
        return static_cast<u8>(c <= '9' ? c - '0' : c - 'a' + 10);
    };
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<u8>(nibble(text[i * 2]) << 4 | nibble(text[i * 2 + 1]));
    }
    return out;
}

// SHA-256 of the 16-byte constants in the 23.0.1 FS .rodata (fs-transfer.md §9.1, §9.8).
constexpr Sha256 KekSourceKspHash =
    FromHex("a6212a52050a18b8987c78c0c7332a7b43b654915e3bf8a3b419ea1cfd22aff8");
constexpr Sha256 KekSourceStreamHash =
    FromHex("140dc3a346eb52c63c38d164125455c7185bec417b0f744c7a270b9ef94662fc");
constexpr Sha256 KekSourceMacHash =
    FromHex("4107cfedee6245f6c674823ddfe4dee4173cf119db13201592dd1580b37ff6cb");
constexpr Sha256 KeySourceKspHash =
    FromHex("5f6b0b562aef4706019605bc4ecea43f6c57467796ee95e10b79d6387f4e2978");
constexpr Sha256 KeySourceMacHash =
    FromHex("98e701afb8d35373e7e32d957dbbd1b683f36148c2fa193062b750a40d1a95bf");

struct Package2Header {
    std::array<u8, 0x100> signature;
    Key128 header_ctr;
    std::array<Key128, 4> section_ctr;
    u32 magic;
    u32 base_offset;
    u32 reserved;
    u8 version_max;
    u8 version_min;
    u16 reserved2;
    std::array<u32, 4> section_size;
    std::array<u32, 4> section_offset;
    std::array<Sha256, 4> section_hash;
};
static_assert(sizeof(Package2Header) == 0x200);

void Ctr(const Key128& key, const Key128& iv, u8* data, std::size_t size) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    int len = 0;
    EVP_DecryptInit_ex(ctx, EVP_aes_128_ctr(), nullptr, key.data(), iv.data());
    EVP_DecryptUpdate(ctx, data, &len, data, static_cast<int>(size));
    EVP_CIPHER_CTX_free(ctx);
}

std::optional<std::vector<u8>> FsRodata(std::span<const u8> package2) {
    if (package2.size() < sizeof(Package2Header)) {
        return std::nullopt;
    }
    auto& keys = Core::Crypto::KeyManager::Instance();
    Package2Header header{};
    std::optional<Key128> key;
    for (u64 revision = 0; revision < 0x20 && !key; ++revision) {
        if (!keys.HasKey(Core::Crypto::S128KeyType::Package2, revision)) {
            continue;
        }
        const Key128 candidate = keys.GetKey(Core::Crypto::S128KeyType::Package2, revision);
        std::memcpy(&header, package2.data(), sizeof(header));
        Ctr(candidate, header.header_ctr, reinterpret_cast<u8*>(&header) + 0x100,
            sizeof(header) - 0x100);
        if (header.magic == 0x31324B50) { // "PK21"
            key = candidate;
        }
    }
    if (!key || header.section_size[0] == 0 ||
        sizeof(header) + header.section_size[0] > package2.size()) {
        return std::nullopt;
    }
    // Since 8.0.0 the INI1 travels inside the kernel section.
    std::vector<u8> kernel(package2.begin() + sizeof(header),
                           package2.begin() + sizeof(header) + header.section_size[0]);
    Ctr(*key, header.section_ctr[0], kernel.data(), kernel.size());
    for (std::size_t at = 0; at + 12 <= kernel.size(); at += 4) {
        if (std::memcmp(kernel.data() + at, "INI1", 4) != 0) {
            continue;
        }
        u32 size = 0;
        std::memcpy(&size, kernel.data() + at + 4, 4);
        if (size == 0 || at + size > kernel.size()) {
            continue;
        }
        const auto file = std::make_shared<FileSys::VectorVfsFile>(
            std::vector<u8>(kernel.begin() + at, kernel.begin() + at + size));
        const FileSys::INI ini{file};
        for (const auto& kip : ini.GetKIPs()) {
            if (kip.GetName() == "FS") {
                const auto rodata = kip.GetRODataSection();
                return std::vector<u8>(rodata.begin(), rodata.end());
            }
        }
    }
    return std::nullopt;
}

} // namespace

std::optional<FsKeySources> FindFsKeySources(std::span<const u8> package2) {
    const auto rodata = FsRodata(package2);
    if (!rodata) {
        return std::nullopt;
    }
    FsKeySources out;
    int found = 0;
    const std::pair<const Sha256*, Key128*> wanted[] = {
        {&KekSourceKspHash, &out.kek_source_ksp},     {&KekSourceStreamHash, &out.kek_source_stream},
        {&KekSourceMacHash, &out.kek_source_mac},     {&KeySourceKspHash, &out.key_source_ksp},
        {&KeySourceMacHash, &out.key_source_mac},
    };
    for (std::size_t at = 0; at + 16 <= rodata->size(); at += 16) {
        Sha256 digest{};
        SHA256(rodata->data() + at, 16, digest.data());
        for (const auto& [hash, slot] : wanted) {
            if (digest == *hash) {
                std::memcpy(slot->data(), rodata->data() + at, 16);
                ++found;
            }
        }
    }
    if (found != static_cast<int>(std::size(wanted))) {
        LOG_WARNING(Service_FS, "save transfer: {} of {} FS key sources found", found,
                    std::size(wanted));
        return std::nullopt;
    }
    return out;
}

std::optional<Key128> DeriveTransferKey(const FsKeySources& sources, KeyIndex index,
                                        int key_generation, const Key128& source) {
    auto& keys = Core::Crypto::KeyManager::Instance();
    const u64 master_index = static_cast<u64>(MasterKeyIndex(key_generation));
    using Core::Crypto::S128KeyType;
    using Core::Crypto::SourceKeyType;
    const auto kek_generation = static_cast<u64>(SourceKeyType::AESKekGeneration);
    const auto key_generation_source = static_cast<u64>(SourceKeyType::AESKeyGeneration);
    if (!keys.HasKey(S128KeyType::Master, master_index) ||
        !keys.HasKey(S128KeyType::Source, kek_generation) ||
        !keys.HasKey(S128KeyType::Source, key_generation_source)) {
        return std::nullopt;
    }
    const Key128 master = keys.GetKey(S128KeyType::Master, master_index);
    const Key128 kek_seed = keys.GetKey(S128KeyType::Source, kek_generation);
    const Key128 key_seed = keys.GetKey(S128KeyType::Source, key_generation_source);
    switch (index) {
    case KeyIndex::KeySeedPackage:
        return DeriveKey(master, kek_seed, key_seed, sources.kek_source_ksp,
                         sources.key_source_ksp);
    case KeyIndex::Stream:
        return DeriveKey(master, kek_seed, key_seed, sources.kek_source_stream, source);
    case KeyIndex::InitialDataMac:
        return DeriveKey(master, kek_seed, key_seed, sources.kek_source_mac,
                         sources.key_source_mac);
    }
    return std::nullopt;
}

std::optional<FsKeySources> InstalledFsKeySources(Core::System& system) {
    static std::mutex mutex;
    static std::optional<FsKeySources> cached;
    std::lock_guard lock{mutex};
    if (cached) {
        return cached;
    }
    const auto* contents = system.GetFileSystemController().GetSystemNANDContents();
    const auto nca =
        contents ? contents->GetEntry(BootImagePackageId, FileSys::ContentRecordType::Data)
                 : nullptr;
    const auto romfs = nca ? FileSys::ExtractRomFS(nca->GetRomFS()) : nullptr;
    const auto nx = romfs ? romfs->GetSubdirectory("nx") : nullptr;
    const auto package2 = nx ? nx->GetFile("package2") : nullptr;
    if (!package2) {
        LOG_WARNING(Service_FS, "save transfer: no BootImagePackage package2 in the system NAND");
        return std::nullopt;
    }
    const auto bytes = package2->ReadAllBytes();
    cached = FindFsKeySources(bytes);
    LOG_INFO(Service_FS, "save transfer: FS key sources {}", cached ? "found" : "not found");
    return cached;
}

} // namespace Service::FileSystem::SaveTransfer

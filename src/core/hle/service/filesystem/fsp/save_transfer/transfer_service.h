// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Save-data transfer v2 for Harbor: the manager, exporter and importer behind fsp-srv cmd 82,
// which HLE olsc also calls directly. Behaviour of the 23.0.1 FS (fs-transfer.md §9): one
// challenge and one random key seed per manager; a full export keeps that seed, a key seed
// package (download, delta export) replaces it; the archive is the save's console image in
// AGS0 chunks with the SID0 initial data.

#pragma once

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "common/common_types.h"
#include "core/file_sys/fs_save_data_types.h"
#include "core/hle/result.h"
#include "core/hle/service/filesystem/fsp/save_transfer/transfer_archive.h"
#include "core/hle/service/filesystem/fsp/save_transfer/transfer_crypto.h"
#include "core/hle/service/filesystem/fsp/save_transfer/transfer_keys.h"

namespace Core {
class System;
}

namespace Service::FileSystem {
class SaveDataController;
}

namespace Service::FileSystem::SaveTransfer {

constexpr Result ResultKeySeedPackageMac{ErrorModule::FS, 7002};
constexpr Result ResultKeySeedPackageSignature{ErrorModule::FS, 7003};
constexpr Result ResultKeySeedPackageChallenge{ErrorModule::FS, 7004};
constexpr Result ResultImportDataVerification{ErrorModule::FS, 7005};
constexpr Result ResultInitialDataMac{ErrorModule::FS, 7006};
constexpr Result ResultKeySeedPackageVersion{ErrorModule::FS, 7037};
constexpr Result ResultTransferNotReady{ErrorModule::FS, 6000};
constexpr Result ResultTransferKeysMissing{ErrorModule::FS, 7031};

/// The key seed package verification key: OpenPak's public modulus, or the 256-byte raw
/// modulus file named by HARBOR_KSP_MODULUS_FILE (tests with a throwaway key).
const Modulus& KeySeedPackageModulus();

class Exporter {
public:
    const FileSys::SaveDataExtraData& ExtraData() const;
    Result SetDivisionCount(s32 count);
    std::vector<u16> ChunkIds() const;
    /// A chunk's archive bytes (identical on every call).
    Result ReadChunk(u16 id, std::vector<u8>* out);
    /// The TransferKeySeed (16 bytes for attribute 0).
    std::array<u8, 0x10> KeySeed() const;
    Result GetInitialDataMac(Key128* out);
    void SetAad(const std::array<u8, 0x20>& aad);
    std::array<u8, 0x20> Aad() const;
    Result Finalize();

private:
    friend class TransferManager;
    FileSys::SaveDataExtraData extra_data{};
    std::array<u8, 0x10> key_seed{};
    std::unique_ptr<ArchiveWriter> writer;
    bool finalized{};
};

class Importer {
public:
    const FileSys::SaveDataExtraData& ExtraData() const;
    std::array<u8, 0x20> Aad() const;
    std::vector<u16> ChunkIds() const;
    /// Bytes of a data chunk; a chunk may arrive in several pushes.
    Result Push(u16 id, std::span<const u8> bytes);
    /// The save written back once every chunk is in (FinalizeImport).
    Result Finalize();
    Result Cancel();

private:
    friend class TransferManager;
    Core::System* system{};
    std::shared_ptr<SaveDataController> controller;
    FileSys::SaveDataSpaceId space{};
    FileSys::SaveDataAttribute attribute{};
    FileSys::SaveDataExtraData extra_data{};
    ArchiveReader reader;
    std::map<u16, std::vector<u8>> pending;
    bool finalized{};
};

class TransferManager {
public:
    explicit TransferManager(Core::System& system);

    const Key128& Challenge() const;
    Result SetKeySeedPackage(std::span<const u8> package);

    Result OpenFullExporter(std::shared_ptr<Exporter>* out, FileSys::SaveDataSpaceId space,
                            u64 save_data_id);
    /// OpenSaveDataImporter: the archive's initial data (the 8 KiB meta chunk), the creation
    /// info's user, the space. Needs a key seed package first.
    Result OpenImporter(std::shared_ptr<Importer>* out, std::span<const u8> initial_data,
                        const FileSys::UserId& user_id, FileSys::SaveDataSpaceId space);

private:
    Result Keys(const Key128& seed, Key128* stream_key, Key128* mac_key) const;

    Core::System& system;
    std::shared_ptr<SaveDataController> controller;
    Key128 challenge{};
    std::array<u8, 0x20> own_seed{};
    std::optional<KeySeedPackageContent> package;
};

} // namespace Service::FileSystem::SaveTransfer

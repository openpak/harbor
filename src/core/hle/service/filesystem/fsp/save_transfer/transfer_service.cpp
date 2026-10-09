// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include "core/hle/service/filesystem/fsp/save_transfer/transfer_service.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#include <openssl/sha.h>

#include "common/fs/file.h"
#include "common/logging.h"
#include "core/core.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/errors.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/save_image/save_image.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/hle/service/filesystem/fsp/fs_i_save_data_info_reader.h"
#include "core/hle/service/filesystem/save_data_controller.h"

namespace Service::FileSystem::SaveTransfer {

namespace {

// OpenPak's key seed package signing key, public modulus (RSA-2048, e = 65537): what consoles
// with OpenPak's fs patch and Harbor verify packages from the saves server against.
constexpr Modulus OpenPakKeySeedPackageModulus{
    0xb9, 0xf3, 0xed, 0xe7, 0x09, 0x3f, 0x7a, 0xf3, 0x2e, 0xfa, 0xa8, 0xb1,
    0x81, 0x27, 0x96, 0xc6, 0x8a, 0x10, 0x98, 0x59, 0xce, 0x30, 0x9c, 0x98,
    0x07, 0x94, 0x4a, 0x81, 0xed, 0xc7, 0xf3, 0xe6, 0x8e, 0xe4, 0x0c, 0xf9,
    0x3f, 0x31, 0xcc, 0x9e, 0x0a, 0xea, 0x7a, 0xb6, 0x59, 0x25, 0x6b, 0xe0,
    0x2b, 0x51, 0xde, 0xcb, 0xc1, 0x63, 0xa9, 0x30, 0x93, 0x25, 0x6b, 0x90,
    0x6a, 0x0e, 0x5c, 0xed, 0xc4, 0x11, 0x2b, 0x2d, 0xc4, 0x9f, 0x9a, 0xb5,
    0xb0, 0x86, 0x3c, 0x77, 0xb1, 0x95, 0x16, 0xfb, 0x6e, 0x3c, 0xc9, 0x05,
    0xc1, 0xca, 0x52, 0x42, 0x3f, 0x50, 0xfb, 0xfe, 0xf3, 0xe7, 0xa5, 0x80,
    0x83, 0xf6, 0x05, 0xdc, 0xd6, 0xb5, 0x6f, 0xc8, 0x75, 0xc9, 0x80, 0xac,
    0xf2, 0x45, 0x69, 0xce, 0xbf, 0x7f, 0x69, 0x4a, 0x33, 0x7b, 0x01, 0xfc,
    0xa2, 0x9d, 0x61, 0x58, 0x0c, 0x23, 0xf0, 0xda, 0xbf, 0x05, 0x01, 0x01,
    0xee, 0x6d, 0x1e, 0x9c, 0xb8, 0xc8, 0xf0, 0x57, 0x88, 0x61, 0xd7, 0x11,
    0x1f, 0x73, 0x0a, 0x12, 0x80, 0x29, 0x1a, 0x33, 0xc5, 0x8c, 0xd8, 0x25,
    0xfb, 0x83, 0x84, 0xe7, 0xca, 0x58, 0xab, 0xbb, 0x04, 0xf2, 0x07, 0x47,
    0xa8, 0x17, 0xff, 0xb3, 0x1f, 0xdb, 0x9b, 0x32, 0xa1, 0x59, 0x35, 0x8c,
    0xc5, 0xa1, 0x1c, 0xce, 0xf0, 0x2f, 0xea, 0x5e, 0x07, 0x1b, 0xc0, 0xa8,
    0x78, 0x28, 0x9e, 0xb8, 0xc5, 0xf5, 0x79, 0x5e, 0xa5, 0x4d, 0xc1, 0x55,
    0x53, 0x5e, 0xb2, 0x08, 0xeb, 0x54, 0x88, 0x49, 0x71, 0x8a, 0x67, 0x32,
    0x27, 0x09, 0xe9, 0xa0, 0xf5, 0x6e, 0xa6, 0x5a, 0xf4, 0xf3, 0x0a, 0x1a,
    0x90, 0x36, 0xaf, 0x60, 0x01, 0xe6, 0x88, 0xe6, 0xea, 0x2a, 0x55, 0xaa,
    0xaa, 0x1d, 0xb8, 0xe0, 0x6e, 0xcb, 0x6b, 0x11, 0x32, 0xfa, 0x16, 0xb4,
    0x77, 0x49, 0x53, 0x9f
};

std::span<const u8> Bytes(const FileSys::SaveDataExtraData& extra_data) {
    return {reinterpret_cast<const u8*>(&extra_data), sizeof(extra_data)};
}

Result FindSave(SaveDataController& controller, FileSys::SaveDataSpaceId space, u64 save_data_id,
                FileSys::SaveDataAttribute* out) {
    if (controller.FindSaveDataById(out, space, save_data_id).IsSuccess()) {
        return ResultSuccess;
    }
    for (const auto& info : ISaveDataInfoReader::FindAllSaves(controller, space)) {
        if (info.save_id_unknown != save_data_id || info.type == FileSys::SaveDataType::System) {
            continue;
        }
        FileSys::UserId user_id{};
        std::memcpy(user_id.data(), info.user_id.data(), sizeof(user_id));
        *out = FileSys::SaveDataAttribute::Make(info.title_id, info.type, user_id, info.save_id,
                                                info.index, info.rank);
        return ResultSuccess;
    }
    return FileSys::ResultTargetNotFound;
}

/// The save's sizes; a save made before extra data was kept takes its title's NACP sizes.
FileSys::SaveImage::SaveGeometry Geometry(Core::System& system,
                                          const FileSys::SaveDataExtraData& extra_data) {
    FileSys::SaveImage::SaveGeometry geometry{};
    geometry.data_size = extra_data.available_size;
    geometry.journal_size = extra_data.journal_size;
    geometry.save_data_type = static_cast<u8>(extra_data.attr.type);
    if (geometry.data_size == 0) {
        const FileSys::PatchManager pm{extra_data.attr.program_id,
                                       system.GetFileSystemController(),
                                       system.GetContentProvider()};
        if (const auto nacp = pm.GetControlMetadata().first) {
            const bool device = extra_data.attr.type == FileSys::SaveDataType::Device;
            geometry.data_size = static_cast<s64>(device ? nacp->GetDeviceSaveDataSize()
                                                         : nacp->GetDefaultNormalSaveSize());
            geometry.journal_size = static_cast<s64>(nacp->GetDefaultJournalSaveSize());
        }
    }
    return geometry;
}

} // namespace

const Modulus& KeySeedPackageModulus() {
    static const Modulus modulus = [] {
        Modulus out = OpenPakKeySeedPackageModulus;
        if (const char* path = std::getenv("HARBOR_KSP_MODULUS_FILE"); path && *path) {
            Common::FS::IOFile file{std::filesystem::path{path}, Common::FS::FileAccessMode::Read,
                                    Common::FS::FileType::BinaryFile};
            if (file.IsOpen() && file.GetSize() == out.size() &&
                file.ReadSpan(std::span<u8>{out}) == out.size()) {
                LOG_WARNING(Service_FS, "save transfer: key seed packages verified against a "
                                        "test key (HARBOR_KSP_MODULUS_FILE)");
            } else {
                out = OpenPakKeySeedPackageModulus;
            }
        }
        return out;
    }();
    return modulus;
}

// ---- Exporter --------------------------------------------------------------------------------

const FileSys::SaveDataExtraData& Exporter::ExtraData() const {
    return extra_data;
}

Result Exporter::SetDivisionCount(s32 count) {
    R_UNLESS(writer->SetDivisionCount(count), FileSys::ResultOutOfRange);
    R_SUCCEED();
}

std::vector<u16> Exporter::ChunkIds() const {
    return writer->ChunkIds();
}

Result Exporter::ReadChunk(u16 id, std::vector<u8>* out) {
    R_UNLESS(writer->DivisionCount() > 0, ResultTransferNotReady);
    auto chunk = writer->Chunk(id);
    R_UNLESS(chunk.has_value(), FileSys::ResultTargetNotFound);
    *out = std::move(*chunk);
    R_SUCCEED();
}

std::array<u8, 0x10> Exporter::KeySeed() const {
    return key_seed;
}

Result Exporter::GetInitialDataMac(Key128* out) {
    R_UNLESS(writer->DivisionCount() > 0, ResultTransferNotReady);
    *out = writer->InitialDataMac();
    R_SUCCEED();
}

void Exporter::SetAad(const std::array<u8, 0x20>& aad) {
    writer->SetAad(aad);
}

std::array<u8, 0x20> Exporter::Aad() const {
    return writer->Aad();
}

Result Exporter::Finalize() {
    R_UNLESS(writer->DivisionCount() > 0, ResultTransferNotReady);
    finalized = true;
    R_SUCCEED();
}

// ---- Importer --------------------------------------------------------------------------------

const FileSys::SaveDataExtraData& Importer::ExtraData() const {
    return extra_data;
}

std::array<u8, 0x20> Importer::Aad() const {
    return reader.Content().aad;
}

std::vector<u16> Importer::ChunkIds() const {
    return reader.ChunkIds();
}

Result Importer::Push(u16 id, std::span<const u8> bytes) {
    const int n = id - FirstDataChunkId;
    R_UNLESS(id >= FirstDataChunkId && n < reader.Content().division_count,
             FileSys::ResultTargetNotFound);
    auto& buffer = pending[id];
    buffer.insert(buffer.end(), bytes.begin(), bytes.end());
    const auto expected = static_cast<std::size_t>(reader.Content().chunk_sizes[n]);
    R_UNLESS(buffer.size() <= expected, ResultImportDataVerification);
    if (buffer.size() == expected) {
        const auto error = reader.Push(id, buffer);
        pending.erase(id);
        R_UNLESS(error == ArchiveReader::Error::None, ResultImportDataVerification);
    }
    R_SUCCEED();
}

Result Importer::Finalize() {
    R_UNLESS(reader.Complete(), ResultTransferNotReady);
    FileSys::VirtualDir dir{};
    if (controller->OpenSaveData(&dir, space, attribute).IsError()) {
        R_TRY(controller->CreateSaveData(&dir, space, attribute));
    }
    const auto stream = std::make_shared<FileSys::VectorVfsFile>(reader.Stream());
    const auto& content = reader.Content();
    FileSys::SaveImage::ImageParameters parameters{content.hash_algorithm,
                                                   content.is_integrity_seed_enabled != 0,
                                                   content.hash_salt};
    const auto geometry = Geometry(*system, extra_data);
    R_UNLESS(FileSys::SaveImage::ImportSaveImage(stream, parameters, geometry, dir),
             ResultImportDataVerification);
    // The save keeps the archive's extra data (its time stamp and commit id travel with it),
    // under this console's attribute.
    FileSys::SaveDataExtraData imported = extra_data;
    imported.attr = attribute;
    R_TRY(controller->WriteSaveDataExtraData(space, attribute, imported));
    finalized = true;
    LOG_INFO(Service_FS, "save transfer: imported the save of {:016X} ({} chunks)",
             attribute.program_id, content.division_count);
    R_SUCCEED();
}

Result Importer::Cancel() {
    pending.clear();
    R_SUCCEED();
}

// ---- Manager ---------------------------------------------------------------------------------

TransferManager::TransferManager(Core::System& system_)
    : system{system_}, controller{system_.GetFileSystemController().OpenSaveDataController()} {
    RandomBytes(challenge);
    RandomBytes(own_seed);
}

const Key128& TransferManager::Challenge() const {
    return challenge;
}

Result TransferManager::Keys(const Key128& seed, Key128* stream_key, Key128* mac_key) const {
    const auto sources = InstalledFsKeySources(system);
    R_UNLESS(sources.has_value(), ResultTransferKeysMissing);
    const auto stream = DeriveTransferKey(*sources, KeyIndex::Stream, StreamKeyGeneration, seed);
    const auto mac = DeriveTransferKey(*sources, KeyIndex::InitialDataMac, StreamKeyGeneration);
    R_UNLESS(stream && mac, ResultTransferKeysMissing);
    *stream_key = *stream;
    *mac_key = *mac;
    R_SUCCEED();
}

Result TransferManager::SetKeySeedPackage(std::span<const u8> bytes) {
    R_UNLESS(!package.has_value(), ResultTransferNotReady);
    R_UNLESS(bytes.size() >= sizeof(KeySeedPackage), FileSys::ResultOutOfRange);
    const auto sources = InstalledFsKeySources(system);
    R_UNLESS(sources.has_value(), ResultTransferKeysMissing);
    const auto key_generation = std::max<int>(bytes[8], 4);
    const auto body_key = DeriveTransferKey(*sources, KeyIndex::KeySeedPackage, key_generation);
    R_UNLESS(body_key.has_value(), ResultTransferKeysMissing);
    const auto opened = OpenKeySeedPackage(bytes.first<0x200>(), *body_key,
                                           KeySeedPackageModulus(), challenge);
    switch (opened.error) {
    case PackageError::None:
        break;
    case PackageError::Version:
        R_THROW(ResultKeySeedPackageVersion);
    case PackageError::Mac:
        R_THROW(ResultKeySeedPackageMac);
    case PackageError::Signature:
        R_THROW(ResultKeySeedPackageSignature);
    case PackageError::Challenge:
        R_THROW(ResultKeySeedPackageChallenge);
    }
    package = opened.content;
    LOG_INFO(Service_FS, "save transfer: key seed package accepted");
    R_SUCCEED();
}

Result TransferManager::OpenFullExporter(std::shared_ptr<Exporter>* out,
                                         FileSys::SaveDataSpaceId space, u64 save_data_id) {
    Key128 seed{};
    if (package) {
        seed = package->key_seed;
    } else {
        std::memcpy(seed.data(), own_seed.data(), seed.size());
    }
    Key128 stream_key{};
    Key128 mac_key{};
    return OpenExporter(out, space, save_data_id, seed, &stream_key, &mac_key);
}

Result TransferManager::OpenDiffExporter(std::shared_ptr<Exporter>* out,
                                         std::span<const u8> initial_data,
                                         FileSys::SaveDataSpaceId space, u64 save_data_id) {
    R_UNLESS(package.has_value(), ResultTransferNotReady);
    std::shared_ptr<Exporter> exporter;
    Key128 stream_key{};
    Key128 mac_key{};
    R_TRY(OpenExporter(&exporter, space, save_data_id, package->key_seed, &stream_key, &mac_key));
    ArchiveReader base;
    switch (base.Open(initial_data, stream_key, mac_key, package->initial_data_mac)) {
    case ArchiveReader::Error::None:
        break;
    case ArchiveReader::Error::Mac:
        R_THROW(ResultInitialDataMac);
    default:
        R_THROW(ResultImportDataVerification);
    }
    R_UNLESS(exporter->writer->SetBase(base.Content()), FileSys::ResultOutOfRange);
    LOG_INFO(Service_FS, "save transfer: diff export, {} of {} chunks changed",
             exporter->writer->ChunkIds().size() - 1, exporter->writer->DivisionCount());
    *out = std::move(exporter);
    R_SUCCEED();
}

Result TransferManager::OpenExporter(std::shared_ptr<Exporter>* out,
                                     FileSys::SaveDataSpaceId space, u64 save_data_id,
                                     const Key128& seed, Key128* stream_key, Key128* mac_key) {
    FileSys::SaveDataAttribute attribute{};
    R_TRY(FindSave(*controller, space, save_data_id, &attribute));
    auto exporter = std::make_shared<Exporter>();
    R_TRY(controller->ReadSaveDataExtraData(&exporter->extra_data, space, attribute));
    if (exporter->extra_data.commit_id == 0) {
        // As FS does (0x8d3f0): a save without a commit id goes by its extra data's digest.
        std::array<u8, 32> digest{};
        SHA256(Bytes(exporter->extra_data).data(), sizeof(exporter->extra_data), digest.data());
        std::memcpy(&exporter->extra_data.commit_id, digest.data(), 8);
    }
    FileSys::VirtualDir dir{};
    R_TRY(controller->OpenSaveData(&dir, space, attribute));
    // Harbor keeps no save thumbnails: the codec puts the console's zero thumbnail first.
    const auto image =
        FileSys::SaveImage::ExportSaveImage(dir, Geometry(system, exporter->extra_data));
    R_UNLESS(image.has_value(), ResultTransferNotReady);
    ArchiveSource source;
    for (const auto& [kind, file] : image->storages) {
        const auto bytes = file->ReadAllBytes();
        source.stream.insert(source.stream.end(), bytes.begin(), bytes.end());
    }
    source.integrity_seed = image->parameters.integrity_seed;
    std::memcpy(source.extra_data.data(), &exporter->extra_data, source.extra_data.size());

    R_TRY(Keys(seed, stream_key, mac_key));
    std::memcpy(exporter->key_seed.data(), seed.data(), seed.size());
    exporter->writer = std::make_unique<ArchiveWriter>(std::move(source), *stream_key, *mac_key);
    LOG_INFO(Service_FS, "save transfer: exporting the save of {:016X}", attribute.program_id);
    *out = std::move(exporter);
    R_SUCCEED();
}

Result TransferManager::OpenImporter(std::shared_ptr<Importer>* out,
                                     std::span<const u8> initial_data,
                                     const FileSys::UserId& user_id,
                                     FileSys::SaveDataSpaceId space) {
    R_UNLESS(package.has_value(), ResultTransferNotReady);
    Key128 stream_key{};
    Key128 mac_key{};
    R_TRY(Keys(package->key_seed, &stream_key, &mac_key));
    auto importer = std::make_shared<Importer>();
    switch (importer->reader.Open(initial_data, stream_key, mac_key, package->initial_data_mac)) {
    case ArchiveReader::Error::None:
        break;
    case ArchiveReader::Error::Mac:
        R_THROW(ResultInitialDataMac);
    default:
        R_THROW(ResultImportDataVerification);
    }
    std::memcpy(&importer->extra_data, importer->reader.Content().extra_data.data(),
                sizeof(importer->extra_data));
    const auto& source = importer->extra_data.attr;
    const bool device = source.type == FileSys::SaveDataType::Device;
    importer->attribute = FileSys::SaveDataAttribute::Make(
        source.program_id, source.type, device ? FileSys::UserId{} : user_id,
        FileSys::InvalidSystemSaveDataId, source.index, source.rank);
    importer->system = &system;
    importer->controller = controller;
    importer->space = space;
    *out = std::move(importer);
    R_SUCCEED();
}

} // namespace Service::FileSystem::SaveTransfer

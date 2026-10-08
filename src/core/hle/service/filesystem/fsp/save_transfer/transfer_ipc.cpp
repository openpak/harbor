// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include "core/hle/service/filesystem/fsp/save_transfer/transfer_ipc.h"

#include <algorithm>
#include <cstring>

#include "core/file_sys/errors.h"
#include "core/hle/service/cmif_serialization.h"

namespace Service::FileSystem::SaveTransfer {

IChunkExporter::IChunkExporter(Core::System& system_, std::vector<u8> bytes_)
    : ServiceFramework{system_, "ISaveDataChunkExporter"}, bytes{std::move(bytes_)} {
    static const FunctionInfo functions[] = {
        {0, D<&IChunkExporter::Pull>, "Pull"},
        {16, D<&IChunkExporter::GetRestRawDataSize>, "GetRestRawDataSize"},
    };
    RegisterHandlers(functions);
}

Result IChunkExporter::Pull(Out<u64> out_size, OutBuffer<BufferAttr_HipcMapAlias> out_buffer,
                            u64 size) {
    const std::size_t length = std::min({static_cast<std::size_t>(size), out_buffer.size(),
                                         bytes.size() - position});
    std::memcpy(out_buffer.data(), bytes.data() + position, length);
    position += length;
    *out_size = length;
    R_SUCCEED();
}

Result IChunkExporter::GetRestRawDataSize(Out<s64> out_size) {
    *out_size = static_cast<s64>(bytes.size() - position);
    R_SUCCEED();
}

IChunkImporter::IChunkImporter(Core::System& system_, std::shared_ptr<Importer> importer_, u16 id_)
    : ServiceFramework{system_, "ISaveDataChunkImporter"}, importer{std::move(importer_)}, id{id_} {
    static const FunctionInfo functions[] = {
        {0, D<&IChunkImporter::Push>, "Push"},
    };
    RegisterHandlers(functions);
}

Result IChunkImporter::Push(InBuffer<BufferAttr_HipcMapAlias> buffer, u64 size) {
    R_RETURN(importer->Push(id, buffer.first(std::min<std::size_t>(buffer.size(), size))));
}

IChunkIterator::IChunkIterator(Core::System& system_, std::vector<u16> ids_)
    : ServiceFramework{system_, "ISaveDataChunkIterator"}, ids{std::move(ids_)} {
    static const FunctionInfo functions[] = {
        {0, D<&IChunkIterator::Next>, "Next"},
        {1, D<&IChunkIterator::IsEnd>, "IsEnd"},
        {16, D<&IChunkIterator::GetId>, "GetId"},
    };
    RegisterHandlers(functions);
}

Result IChunkIterator::Next() {
    index = std::min(index + 1, ids.size());
    R_SUCCEED();
}

Result IChunkIterator::IsEnd(Out<bool> out_is_end) {
    *out_is_end = index >= ids.size();
    R_SUCCEED();
}

Result IChunkIterator::GetId(Out<u32> out_id) {
    R_UNLESS(index < ids.size(), FileSys::ResultOutOfRange);
    *out_id = ids[index];
    R_SUCCEED();
}

IDivisionExporter::IDivisionExporter(Core::System& system_, std::shared_ptr<Exporter> exporter_)
    : ServiceFramework{system_, "ISaveDataDivisionExporter"}, exporter{std::move(exporter_)} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, D<&IDivisionExporter::SetDivisionCount>, "SetDivisionCount"},
        {1, D<&IDivisionExporter::ReadSaveDataExtraData>, "ReadSaveDataExtraData"},
        {16, D<&IDivisionExporter::OpenSaveDataDiffChunkIterator>, "OpenSaveDataDiffChunkIterator"},
        {48, D<&IDivisionExporter::OpenSaveDataChunkExporter>, "OpenSaveDataChunkExporter"},
        {66, D<&IDivisionExporter::CancelExport>, "CancelExport"},
        {67, nullptr, "SuspendExport"},
        {71, D<&IDivisionExporter::GetInitialDataMac>, "GetInitialDataMac"},
        {72, D<&IDivisionExporter::FinalizeExport>, "FinalizeExport"},
        {73, D<&IDivisionExporter::GetInitialDataMacKeyGeneration>, "GetInitialDataMacKeyGeneration"},
        {74, D<&IDivisionExporter::GetKeySeed>, "GetKeySeed"},
        {75, nullptr, "GetLocalKeySeedPackage"},
        {80, D<&IDivisionExporter::GetImportInitialDataAad>, "GetImportInitialDataAad"},
        {81, D<&IDivisionExporter::SetExportInitialDataAad>, "SetExportInitialDataAad"},
        {96, nullptr, "GetReportInfo"},
    };
    // clang-format on
    RegisterHandlers(functions);
}

Result IDivisionExporter::SetDivisionCount(s32 count) {
    R_RETURN(exporter->SetDivisionCount(count));
}

Result IDivisionExporter::ReadSaveDataExtraData(OutBuffer<BufferAttr_HipcMapAlias> out_buffer) {
    const auto& extra_data = exporter->ExtraData();
    std::memset(out_buffer.data(), 0, out_buffer.size());
    std::memcpy(out_buffer.data(), &extra_data, std::min(out_buffer.size(), sizeof(extra_data)));
    R_SUCCEED();
}

Result IDivisionExporter::OpenSaveDataDiffChunkIterator(
    Out<SharedPointer<IChunkIterator>> out_iterator) {
    *out_iterator = std::make_shared<IChunkIterator>(system, exporter->ChunkIds());
    R_SUCCEED();
}

Result IDivisionExporter::OpenSaveDataChunkExporter(
    Out<SharedPointer<IChunkExporter>> out_exporter, u32 id) {
    std::vector<u8> bytes;
    R_TRY(exporter->ReadChunk(static_cast<u16>(id), &bytes));
    *out_exporter = std::make_shared<IChunkExporter>(system, std::move(bytes));
    R_SUCCEED();
}

Result IDivisionExporter::CancelExport() {
    R_SUCCEED();
}

Result IDivisionExporter::GetInitialDataMac(Out<Key128> out_mac) {
    R_RETURN(exporter->GetInitialDataMac(out_mac.Get()));
}

Result IDivisionExporter::FinalizeExport() {
    R_RETURN(exporter->Finalize());
}

Result IDivisionExporter::GetInitialDataMacKeyGeneration(Out<s32> out_generation) {
    *out_generation = StreamKeyGeneration;
    R_SUCCEED();
}

Result IDivisionExporter::GetKeySeed(OutBuffer<BufferAttr_HipcMapAlias> out_buffer,
                                     Out<u32> out_size) {
    const auto seed = exporter->KeySeed();
    std::memset(out_buffer.data(), 0, out_buffer.size());
    const std::size_t length = std::min(out_buffer.size(), seed.size());
    std::memcpy(out_buffer.data(), seed.data(), length);
    *out_size = static_cast<u32>(length);
    R_SUCCEED();
}

Result IDivisionExporter::GetImportInitialDataAad(Out<std::array<u8, 0x20>> out_aad) {
    *out_aad = exporter->Aad();
    R_SUCCEED();
}

Result IDivisionExporter::SetExportInitialDataAad(std::array<u8, 0x20> aad) {
    exporter->SetAad(aad);
    R_SUCCEED();
}

IDivisionImporter::IDivisionImporter(Core::System& system_, std::shared_ptr<Importer> importer_)
    : ServiceFramework{system_, "ISaveDataDivisionImporter"}, importer{std::move(importer_)} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, D<&IDivisionImporter::ReadSaveDataExtraData>, "ReadSaveDataExtraData"},
        {16, D<&IDivisionImporter::OpenSaveDataDiffChunkIterator>, "OpenSaveDataDiffChunkIterator"},
        {32, D<&IDivisionImporter::InitializeImport>, "InitializeImport"},
        {33, D<&IDivisionImporter::FinalizeImport>, "FinalizeImport"},
        {34, D<&IDivisionImporter::CancelImport>, "CancelImport"},
        {35, nullptr, "GetImportContext"},
        {36, nullptr, "SuspendImport"},
        {37, D<&IDivisionImporter::FinalizeImport>, "FinalizeImportWithoutSwap"},
        {48, D<&IDivisionImporter::OpenSaveDataChunkImporter>, "OpenSaveDataChunkImporter"},
        {64, D<&IDivisionImporter::GetImportInitialDataAad>, "GetImportInitialDataAad"},
        {80, nullptr, "GetReportInfo"},
    };
    // clang-format on
    RegisterHandlers(functions);
}

Result IDivisionImporter::ReadSaveDataExtraData(OutBuffer<BufferAttr_HipcMapAlias> out_buffer) {
    const auto& extra_data = importer->ExtraData();
    std::memset(out_buffer.data(), 0, out_buffer.size());
    std::memcpy(out_buffer.data(), &extra_data, std::min(out_buffer.size(), sizeof(extra_data)));
    R_SUCCEED();
}

Result IDivisionImporter::OpenSaveDataDiffChunkIterator(
    Out<SharedPointer<IChunkIterator>> out_iterator) {
    *out_iterator = std::make_shared<IChunkIterator>(system, importer->ChunkIds());
    R_SUCCEED();
}

Result IDivisionImporter::InitializeImport(Out<s64> out_remaining, s64 size) {
    // Harbor makes the save at FinalizeImport; nothing to prepare in steps.
    *out_remaining = 0;
    R_SUCCEED();
}

Result IDivisionImporter::FinalizeImport() {
    R_RETURN(importer->Finalize());
}

Result IDivisionImporter::CancelImport() {
    R_RETURN(importer->Cancel());
}

Result IDivisionImporter::OpenSaveDataChunkImporter(
    Out<SharedPointer<IChunkImporter>> out_importer, u32 id) {
    *out_importer = std::make_shared<IChunkImporter>(system, importer, static_cast<u16>(id));
    R_SUCCEED();
}

Result IDivisionImporter::GetImportInitialDataAad(Out<std::array<u8, 0x20>> out_aad) {
    *out_aad = importer->Aad();
    R_SUCCEED();
}

ISaveDataTransferManagerVersion2::ISaveDataTransferManagerVersion2(Core::System& system_)
    : ServiceFramework{system_, "ISaveDataTransferManagerVersion2"}, manager{system_} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, D<&ISaveDataTransferManagerVersion2::GetChallenge>, "GetChallenge"},
        {16, D<&ISaveDataTransferManagerVersion2::SetKeySeedPackage>, "SetKeySeedPackage"},
        {17, nullptr, "SetLocalKeySeedPackage"},
        {32, D<&ISaveDataTransferManagerVersion2::OpenSaveDataFullExporter>, "OpenSaveDataFullExporter"},
        {33, nullptr, "OpenSaveDataDiffExporter"},
        {34, nullptr, "OpenSaveDataExporterByContext"},
        {63, D<&ISaveDataTransferManagerVersion2::OpenSaveDataImporter>, "OpenSaveDataImporter"},
        {68, nullptr, "OpenSaveDataImporterByContext"},
        {69, nullptr, "CancelSuspendingImport"},
        {70, nullptr, "CancelSuspendingImportByAttribute"},
        {80, nullptr, "SwapSecondary"},
    };
    // clang-format on
    RegisterHandlers(functions);
}

Result ISaveDataTransferManagerVersion2::GetChallenge(
    OutBuffer<BufferAttr_HipcMapAlias> out_challenge) {
    R_UNLESS(out_challenge.size() >= 0x10, FileSys::ResultOutOfRange);
    std::memcpy(out_challenge.data(), manager.Challenge().data(), 0x10);
    R_SUCCEED();
}

Result ISaveDataTransferManagerVersion2::SetKeySeedPackage(
    InBuffer<BufferAttr_HipcMapAlias> package) {
    R_RETURN(manager.SetKeySeedPackage(package));
}

Result ISaveDataTransferManagerVersion2::OpenSaveDataFullExporter(
    Out<SharedPointer<IDivisionExporter>> out_exporter, FileSys::SaveDataSpaceId space,
    u64 save_data_id) {
    std::shared_ptr<Exporter> exporter;
    R_TRY(manager.OpenFullExporter(&exporter, space, save_data_id));
    *out_exporter = std::make_shared<IDivisionExporter>(system, std::move(exporter));
    R_SUCCEED();
}

Result ISaveDataTransferManagerVersion2::OpenSaveDataImporter(
    Out<SharedPointer<IDivisionImporter>> out_importer,
    InBuffer<BufferAttr_HipcMapAlias> initial_data, InBuffer<BufferAttr_HipcMapAlias> creation_info,
    FileSys::SaveDataSpaceId space) {
    // SaveDataCreationInfo2 as olsc fills it: the user at +0x10 (olsc-transfer.md §3.1).
    FileSys::UserId user_id{};
    if (creation_info.size() >= 0x20) {
        std::memcpy(user_id.data(), creation_info.data() + 0x10, sizeof(user_id));
    }
    std::shared_ptr<Importer> importer;
    R_TRY(manager.OpenImporter(&importer, initial_data, user_id, space));
    *out_importer = std::make_shared<IDivisionImporter>(system, std::move(importer));
    R_SUCCEED();
}

} // namespace Service::FileSystem::SaveTransfer

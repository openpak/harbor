// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// fsp-srv cmd 82 OpenSaveDataTransferManagerVersion2 and its objects, with the command ids the
// 23.0.1 olsc sends (olsc-transfer.md §2.1), over transfer_service. Harbor's olsc is HLE and
// calls transfer_service directly; these serve any guest that asks fs itself.

#pragma once

#include <memory>

#include "core/hle/service/cmif_types.h"
#include "core/hle/service/filesystem/fsp/save_transfer/transfer_service.h"
#include "core/hle/service/service.h"

namespace Service::FileSystem::SaveTransfer {

class IChunkExporter final : public ServiceFramework<IChunkExporter> {
public:
    IChunkExporter(Core::System& system_, std::vector<u8> bytes_);

private:
    Result Pull(Out<u64> out_size, OutBuffer<BufferAttr_HipcMapAlias> out_buffer, u64 size);
    Result GetRestRawDataSize(Out<s64> out_size);

    std::vector<u8> bytes;
    std::size_t position{};
};

class IChunkImporter final : public ServiceFramework<IChunkImporter> {
public:
    IChunkImporter(Core::System& system_, std::shared_ptr<Importer> importer_, u16 id_);

private:
    Result Push(InBuffer<BufferAttr_HipcMapAlias> buffer, u64 size);

    std::shared_ptr<Importer> importer;
    u16 id;
};

class IChunkIterator final : public ServiceFramework<IChunkIterator> {
public:
    IChunkIterator(Core::System& system_, std::vector<u16> ids_);

private:
    Result Next();
    Result IsEnd(Out<bool> out_is_end);
    Result GetId(Out<u32> out_id);

    std::vector<u16> ids;
    std::size_t index{};
};

class IDivisionExporter final : public ServiceFramework<IDivisionExporter> {
public:
    IDivisionExporter(Core::System& system_, std::shared_ptr<Exporter> exporter_);

private:
    Result SetDivisionCount(s32 count);
    Result ReadSaveDataExtraData(OutBuffer<BufferAttr_HipcMapAlias> out_buffer);
    Result OpenSaveDataDiffChunkIterator(Out<SharedPointer<IChunkIterator>> out_iterator);
    Result OpenSaveDataChunkExporter(Out<SharedPointer<IChunkExporter>> out_exporter, u32 id);
    Result CancelExport();
    Result GetInitialDataMac(Out<Key128> out_mac);
    Result FinalizeExport();
    Result GetInitialDataMacKeyGeneration(Out<s32> out_generation);
    Result GetKeySeed(OutBuffer<BufferAttr_HipcMapAlias> out_buffer, Out<u32> out_size);
    Result GetImportInitialDataAad(Out<std::array<u8, 0x20>> out_aad);
    Result SetExportInitialDataAad(std::array<u8, 0x20> aad);

    std::shared_ptr<Exporter> exporter;
};

class IDivisionImporter final : public ServiceFramework<IDivisionImporter> {
public:
    IDivisionImporter(Core::System& system_, std::shared_ptr<Importer> importer_);

private:
    Result ReadSaveDataExtraData(OutBuffer<BufferAttr_HipcMapAlias> out_buffer);
    Result OpenSaveDataDiffChunkIterator(Out<SharedPointer<IChunkIterator>> out_iterator);
    Result InitializeImport(Out<s64> out_remaining, s64 size);
    Result FinalizeImport();
    Result CancelImport();
    Result OpenSaveDataChunkImporter(Out<SharedPointer<IChunkImporter>> out_importer, u32 id);
    Result GetImportInitialDataAad(Out<std::array<u8, 0x20>> out_aad);

    std::shared_ptr<Importer> importer;
};

class ISaveDataTransferManagerVersion2 final
    : public ServiceFramework<ISaveDataTransferManagerVersion2> {
public:
    explicit ISaveDataTransferManagerVersion2(Core::System& system_);

private:
    Result GetChallenge(OutBuffer<BufferAttr_HipcMapAlias> out_challenge);
    Result SetKeySeedPackage(InBuffer<BufferAttr_HipcMapAlias> package);
    Result OpenSaveDataFullExporter(Out<SharedPointer<IDivisionExporter>> out_exporter,
                                    FileSys::SaveDataSpaceId space, u64 save_data_id);
    Result OpenSaveDataDiffExporter(Out<SharedPointer<IDivisionExporter>> out_exporter,
                                    InBuffer<BufferAttr_HipcMapAlias> initial_data,
                                    FileSys::SaveDataSpaceId space, u64 save_data_id);
    Result OpenSaveDataImporter(Out<SharedPointer<IDivisionImporter>> out_importer,
                                InBuffer<BufferAttr_HipcMapAlias> initial_data,
                                InBuffer<BufferAttr_HipcMapAlias> creation_info,
                                FileSys::SaveDataSpaceId space);

    TransferManager manager;
};

} // namespace Service::FileSystem::SaveTransfer

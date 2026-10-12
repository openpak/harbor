// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include "common/uuid.h"
#include "core/hle/service/cmif_types.h"
#include "core/hle/service/ns/ns_types.h"
#include "core/hle/service/service.h"

namespace OpenPak {
class AsyncWorker;
}
namespace Kernel {
class KTransferMemory;
}

namespace Service::OLSC {

class IAsyncResult;
class TransferQueue;

class IOlscServiceForApplication final : public ServiceFramework<IOlscServiceForApplication> {
public:
    explicit IOlscServiceForApplication(Core::System& system_);
    ~IOlscServiceForApplication() override;

private:
    Result Initialize(ClientProcessId process_id);
    Result VerifySaveDataBackupLicenseAsync(Out<SharedPointer<IAsyncResult>> out_task,
                                            Common::UUID user);
    Result GetSaveDataBackupSetting(Out<u8> out_save_data_backup_setting, Common::UUID user);
    Result SetSaveDataBackupSettingEnabled(bool enabled, NS::Uid user);
    Result SetCustomData(Common::UUID user, InBuffer<BufferAttr_HipcMapAlias> data);
    Result DeleteSaveDataBackupSetting(Common::UUID user);
    Result GetSaveDataBackupInfoCache(
        OutLargeData<std::array<u8, 0x410>, BufferAttr_HipcMapAlias> out_info, Common::UUID user);
    Result UpdateSaveDataBackupInfoCacheAsync(Out<SharedPointer<IAsyncResult>> out_task,
                                              Common::UUID user);
    Result UploadSaveDataBackupAsync(Out<SharedPointer<IAsyncResult>> out_task, Common::UUID user);
    Result ListDownloadableSaveDataBackupInfoAsync(Out<SharedPointer<IAsyncResult>> out_task,
                                                   u32 memory_size, NS::Uid user,
                                                   InCopyHandle<Kernel::KTransferMemory> memory);
    Result DownloadSaveDataBackupAsync(Out<SharedPointer<IAsyncResult>> out_task, Common::UUID user,
                                       u64 archive_id);
    Result TransferBackupAsync(Out<SharedPointer<IAsyncResult>> out_task, Common::UUID user,
                               u64 archive_id, bool download);

    bool initialized{};
    u64 application_id{};
    std::shared_ptr<TransferQueue> queue;
    std::shared_ptr<OpenPak::AsyncWorker> worker;
};

} // namespace Service::OLSC

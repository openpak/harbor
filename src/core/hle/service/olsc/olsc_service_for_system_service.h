// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <memory>

#include "common/uuid.h"
#include "core/hle/service/cmif_types.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/service.h"

namespace Kernel {
class KEvent;
class KReadableEvent;
} // namespace Kernel

namespace OpenPak {
class AsyncWorker;
namespace Olsc {
class DaemonSettings;
}
} // namespace OpenPak

namespace Service::OLSC {

class TransferQueue;

class IAsyncLatestVersionPlayablePlatformsRequest;
class IDaemonController;
class IRemoteStorageController;
class ITransferTaskListController;

struct DataTransferPolicy {
    u8 upload_policy;
    u8 download_policy;
};

struct TransferTaskErrorInfo {
    Common::UUID uid;
    u64 application_id;
    u8 unknown_0x18;
    std::array<u8, 7> reserved_0x19;
    u64 unknown_0x20;
    u32 error_code;
    u32 reserved_0x2C;
};
static_assert(sizeof(TransferTaskErrorInfo) == 0x30, "TransferTaskErrorInfo has incorrect size.");

class IOlscServiceForSystemService final : public ServiceFramework<IOlscServiceForSystemService> {
public:
    explicit IOlscServiceForSystemService(Core::System& system_);
    ~IOlscServiceForSystemService() override;

private:
    Result GetTransferTaskListController(
        Out<SharedPointer<ITransferTaskListController>> out_interface);
    Result GetRemoteStorageController(Out<SharedPointer<IRemoteStorageController>> out_interface,
                                      Common::UUID user_id);
    Result GetDaemonController(Out<SharedPointer<IDaemonController>> out_interface);
    Result GetDataTransferPolicy(Out<DataTransferPolicy> out_policy, u64 application_id);
    Result DeleteDataTransferPolicyCache(u64 application_id);
    Result ClearDataTransferPolicyCache();
    Result GetDataTransferPolicy209(Out<DataTransferPolicy> out_policy, u64 application_id);
    Result RequestLatestVersionPlayablePlatforms(
        Out<SharedPointer<IAsyncLatestVersionPlayablePlatformsRequest>> out_request,
        u64 application_id);
    Result GetTransferTaskErrorInfo(Out<TransferTaskErrorInfo> out_info, Common::UUID uid, u64 application_id);
    Result RemoveLastErrorInfo(Common::UUID uuid, u64 application_id);
    Result GetOlscServiceForSystemService(
        Out<SharedPointer<IOlscServiceForSystemService>> out_interface);

    /// Runs the network work behind the async objects (the archive list).
    std::shared_ptr<OpenPak::AsyncWorker> worker;
    /// The session's transfer task queue (RSC 1/12 register, TTLC reads it).
    std::shared_ptr<TransferQueue> queue;
};

/// The 23.x request behind olsc:s 1001: which platforms the title's latest version plays on.
/// Done at once (the sysmodule asks pegasus, which OpenPak does not serve).
class IAsyncLatestVersionPlayablePlatformsRequest final
    : public ServiceFramework<IAsyncLatestVersionPlayablePlatformsRequest> {
public:
    explicit IAsyncLatestVersionPlayablePlatformsRequest(Core::System& system_);
    ~IAsyncLatestVersionPlayablePlatformsRequest() override;

private:
    Result GetResult();
    Result Cancel();
    Result HasDone(Out<bool> out_done);
    Result GetSystemEvent(OutCopyHandle<Kernel::KReadableEvent> out_event);
    Result GetPlayablePlatforms(Out<u32> out_platforms);

    KernelHelpers::ServiceContext service_context;
    Kernel::KEvent* event{};
};

} // namespace Service::OLSC

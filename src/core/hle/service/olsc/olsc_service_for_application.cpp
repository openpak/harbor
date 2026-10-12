// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include "common/redact.h"
#include "core/core.h"
#include "core/hle/kernel/k_transfer_memory.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/olsc/olsc_service_for_application.h"
#include "core/hle/service/olsc/remote_storage_controller.h"
#include "core/hle/service/olsc/transfer_runner.h"
#include "core/openpak/async_worker.h"
#include "core/openpak/olsc.h"
#include "core/openpak/session.h"

namespace Service::OLSC {

// Command 14 places the UID at byte 8 after its one-byte enabled flag.
// Common::UUID alone has byte alignment and would consume the padding as the UID.
static_assert(alignof(NS::Uid) == 8 && sizeof(NS::Uid) == 0x10);
static_assert(GetInRawDataSize<std::tuple<bool, NS::Uid>>() == 0x18);
static_assert(GetInRawDataSize<std::tuple<u32, NS::Uid>>() == 0x18);

IOlscServiceForApplication::IOlscServiceForApplication(Core::System& system_)
    : ServiceFramework{system_, "olsc:u"}, queue{TransferQueue::For(system_)},
      worker{std::make_shared<OpenPak::AsyncWorker>(system_, "olsc:u")} {
    // clang-format off
        static const FunctionInfo functions[] = {
            {0, D<&IOlscServiceForApplication::Initialize>, "Initialize"},
            {10, D<&IOlscServiceForApplication::VerifySaveDataBackupLicenseAsync>, "VerifySaveDataBackupLicenseAsync"},
            {13, D<&IOlscServiceForApplication::GetSaveDataBackupSetting>, "GetSaveDataBackupSetting"},
            {14, D<&IOlscServiceForApplication::SetSaveDataBackupSettingEnabled>, "SetSaveDataBackupSettingEnabled"},
            {15, D<&IOlscServiceForApplication::SetCustomData>, "SetCustomData"},
            {16, D<&IOlscServiceForApplication::DeleteSaveDataBackupSetting>, "DeleteSaveDataBackupSetting"},
            {18, D<&IOlscServiceForApplication::GetSaveDataBackupInfoCache>, "GetSaveDataBackupInfoCache"},
            {19, D<&IOlscServiceForApplication::UpdateSaveDataBackupInfoCacheAsync>, "UpdateSaveDataBackupInfoCacheAsync"},
            {22, nullptr, "DeleteSaveDataBackupAsync"},
            {25, D<&IOlscServiceForApplication::ListDownloadableSaveDataBackupInfoAsync>, "ListDownloadableSaveDataBackupInfoAsync"},
            {26, D<&IOlscServiceForApplication::DownloadSaveDataBackupAsync>, "DownloadSaveDataBackupAsync"},
            {27, D<&IOlscServiceForApplication::UploadSaveDataBackupAsync>, "UploadSaveDataBackupAsync"},
            {9010, nullptr, "VerifySaveDataBackupLicenseAsyncForDebug"},
            {9013, nullptr, "GetSaveDataBackupSettingForDebug"},
            {9014, nullptr, "SetSaveDataBackupSettingEnabledForDebug"},
            {9015, nullptr, "SetCustomDataForDebug"},
            {9016, nullptr, "DeleteSaveDataBackupSettingForDebug"},
            {9018, nullptr, "GetSaveDataBackupInfoCacheForDebug"},
            {9019, nullptr, "UpdateSaveDataBackupInfoCacheAsyncForDebug"},
            {9022, nullptr, "DeleteSaveDataBackupAsyncForDebug"},
            {9025, nullptr, "ListDownloadableSaveDataBackupInfoAsyncForDebug"},
            {9026, nullptr, "DownloadSaveDataBackupAsyncForDebug"},
        };
    // clang-format on

    RegisterHandlers(functions);
}

IOlscServiceForApplication::~IOlscServiceForApplication() = default;

Result IOlscServiceForApplication::Initialize(ClientProcessId process_id) {
    application_id = system.ResolveCallerProgramId(*process_id);
    R_UNLESS(application_id != 0, (Result{ErrorModule::OLSC, 101}));
    initialized = true;
    R_SUCCEED();
}

Result IOlscServiceForApplication::VerifySaveDataBackupLicenseAsync(
    Out<SharedPointer<IAsyncResult>> out_task, Common::UUID user) {
    R_UNLESS(initialized, (Result{ErrorModule::OLSC, 101}));
    R_UNLESS(user.IsValid(), (Result{ErrorModule::OLSC, 100}));
    *out_task = std::make_shared<IAsyncResult>(system, worker, [uid = user.RawString()] {
        // OpenPak's account module reports Subscribed for linked accounts (acc 143/170).
        // An absent or unlinked account must not be given a successful backup license.
        return OpenPak::Session::Enabled() && OpenPak::Session::HasDeviceAccount(uid) &&
                       OpenPak::Session::Linked(uid)
                   ? ResultSuccess
                   : Result{ErrorModule::OLSC, 10};
    });
    R_SUCCEED();
}

Result IOlscServiceForApplication::GetSaveDataBackupSetting(Out<u8> out_setting,
                                                            Common::UUID user) {
    R_UNLESS(initialized, (Result{ErrorModule::OLSC, 101}));
    R_UNLESS(user.IsValid(), (Result{ErrorModule::OLSC, 100}));
    const auto uid = user.RawString();
    const auto record = queue->Settings()->Backup(uid, application_id);
    *out_setting = OpenPak::Session::Linked(uid) && record ? (record->enabled ? 1 : 2) : 0;
    R_SUCCEED();
}

Result IOlscServiceForApplication::SetSaveDataBackupSettingEnabled(bool enabled, NS::Uid user) {
    R_UNLESS(initialized, (Result{ErrorModule::OLSC, 101}));
    R_UNLESS(user.uuid.IsValid(), (Result{ErrorModule::OLSC, 100}));
    if (OpenPak::Session::Linked(user.uuid.RawString())) {
        queue->Settings()->SetBackup(user.uuid.RawString(), application_id, enabled);
        queue->MakeDue(user.uuid);
    }
    R_SUCCEED();
}

Result IOlscServiceForApplication::SetCustomData(Common::UUID user,
                                                 InBuffer<BufferAttr_HipcMapAlias> data) {
    R_UNLESS(initialized, (Result{ErrorModule::OLSC, 101}));
    R_UNLESS(user.IsValid(), (Result{ErrorModule::OLSC, 100}));
    const auto end = std::find(data.begin(), data.end(), u8{0});
    R_UNLESS(data.size() <= 0x400 && (data.empty() || end != data.end()), (Result{0x60a}));
    if (!OpenPak::Session::Linked(user.RawString()))
        R_SUCCEED();
    const std::string desc{data.begin(), end};
    R_UNLESS(queue->Settings()->SetBackupCustomData(user.RawString(), application_id, desc),
             (Result{ErrorModule::OLSC, 1000}));
    R_SUCCEED();
}

Result IOlscServiceForApplication::DeleteSaveDataBackupSetting(Common::UUID user) {
    R_UNLESS(initialized, (Result{ErrorModule::OLSC, 101}));
    R_UNLESS(user.IsValid(), (Result{ErrorModule::OLSC, 100}));
    if (OpenPak::Session::Linked(user.RawString()))
        queue->Settings()->RemoveBackup(user.RawString(), application_id);
    R_SUCCEED();
}

Result IOlscServiceForApplication::GetSaveDataBackupInfoCache(
    OutLargeData<std::array<u8, 0x410>, BufferAttr_HipcMapAlias> out_info, Common::UUID user) {
    R_UNLESS(initialized, (Result{ErrorModule::OLSC, 101}));
    R_UNLESS(user.IsValid(), (Result{ErrorModule::OLSC, 100}));
    R_UNLESS(OpenPak::Session::Linked(user.RawString()), (Result{ErrorModule::OLSC, 1210}));
    const auto archives = OpenPak::Olsc::ArchiveCache::Instance().Get(user.RawString(), 3);
    const auto archive =
        archives ? OpenPak::Olsc::LatestFor(*archives, application_id) : std::nullopt;
    R_UNLESS(archive, (Result{ErrorModule::OLSC, 1200}));
    *out_info = OpenPak::Olsc::PackBackupInfo(*archive);
    R_SUCCEED();
}

Result IOlscServiceForApplication::UpdateSaveDataBackupInfoCacheAsync(
    Out<SharedPointer<IAsyncResult>> out_task, Common::UUID user) {
    R_UNLESS(initialized, (Result{ErrorModule::OLSC, 101}));
    R_UNLESS(user.IsValid(), (Result{ErrorModule::OLSC, 100}));
    *out_task = std::make_shared<IAsyncResult>(system, worker, [uid = user.RawString()] {
        if (!OpenPak::Session::Linked(uid))
            return Result{ErrorModule::OLSC, 10};
        return OpenPak::Olsc::ArchiveCache::Instance().RefreshNow(uid, 3)
                   ? ResultSuccess
                   : Result{ErrorModule::OLSC, 9000};
    });
    R_SUCCEED();
}

Result IOlscServiceForApplication::UploadSaveDataBackupAsync(
    Out<SharedPointer<IAsyncResult>> out_task, Common::UUID user) {
    return TransferBackupAsync(out_task, user, 0, false);
}

Result IOlscServiceForApplication::DownloadSaveDataBackupAsync(
    Out<SharedPointer<IAsyncResult>> out_task, Common::UUID user, u64 archive_id) {
    return TransferBackupAsync(out_task, user, archive_id, true);
}

Result IOlscServiceForApplication::ListDownloadableSaveDataBackupInfoAsync(
    Out<SharedPointer<IAsyncResult>> out_task, u32 memory_size, NS::Uid user,
    InCopyHandle<Kernel::KTransferMemory> memory) {
    R_UNLESS(initialized, (Result{ErrorModule::OLSC, 101}));
    R_UNLESS(user.uuid.IsValid() && memory.Get() && (memory_size & 0xfff) == 0 &&
                 memory_size <= memory.Get()->GetSize(),
             (Result{ErrorModule::OLSC, 100}));
    const std::size_t capacity = memory_size / 0x438;
    auto work = [queue = queue, user = user.uuid, app = application_id,
                 capacity](std::stop_token stop, std::vector<u8>& value) -> Result {
        if (!OpenPak::Session::Linked(user.RawString()) ||
            !OpenPak::Session::HasDeviceAccount(user.RawString()))
            return Result{ErrorModule::OLSC, 10};
        const auto list = queue->DeviceBackups(user, app, capacity);
        if (list.error != OpenPak::Olsc::Transfer::Error::None)
            return Result{ErrorModule::OLSC, static_cast<u32>(list.error)};
        if (stop.stop_requested())
            return Result{ErrorModule::OLSC, 0};
        for (const auto& archive : list.archives) {
            const auto info = OpenPak::Olsc::PackBackupInfo(archive);
            value.insert(value.end(), info.begin(), info.end());
        }
        return ResultSuccess;
    };
    *out_task = std::make_shared<IAsyncResult>(
        system, worker, std::move(work), std::function<std::array<u8, 0x30>()>{}, memory.Get());
    R_SUCCEED();
}

Result IOlscServiceForApplication::TransferBackupAsync(Out<SharedPointer<IAsyncResult>> out_task,
                                                       Common::UUID user, u64 archive_id,
                                                       bool download) {
    R_UNLESS(initialized, (Result{ErrorModule::OLSC, 101}));
    R_UNLESS(user.IsValid(), (Result{ErrorModule::OLSC, 100}));
    struct State {
        std::mutex mutex;
        std::array<u8, 0x30> bytes{};
    };
    auto state = std::make_shared<State>();
    auto work = [queue = queue, user, app = application_id, archive_id, download,
                 state](std::stop_token stop, std::vector<u8>&) -> Result {
        if (!OpenPak::Session::Linked(user.RawString()) ||
            !OpenPak::Session::HasDeviceAccount(user.RawString()))
            return Result{ErrorModule::OLSC, 10};
        if (stop.stop_requested())
            return Result{ErrorModule::OLSC, 0};
        // Application-driven backup owns the queue until it finishes, like native OLSC.
        struct AutonomyHold {
            std::shared_ptr<TransferQueue> queue;
            bool execution_held{true};
            explicit AutonomyHold(std::shared_ptr<TransferQueue> queue_) : queue{std::move(queue_)} {
                queue->HoldAutonomy(true);
                queue->HoldExecution(true);
            }
            void ReleaseExecution() {
                if (execution_held) {
                    queue->HoldExecution(false);
                    execution_held = false;
                }
            }
            ~AutonomyHold() {
                ReleaseExecution();
                queue->HoldAutonomy(false);
            }
        } hold{queue};
        if (const auto current = queue->Current()) {
            queue->Cancel(current->id);
            while (queue->Current()) {
                if (stop.stop_requested())
                    return Result{ErrorModule::OLSC, 0};
                std::this_thread::sleep_for(std::chrono::milliseconds{200});
            }
        }
        u64 id{};
        const auto result = download ? queue->RegisterDeviceDownload(id, user, app, archive_id, stop)
                                     : queue->RegisterDeviceUpload(id, user, app, true, RankBasic, stop);
        if (result.IsError())
            return result;
        hold.ReleaseExecution();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{300};
        bool started = false;
        while (!stop.stop_requested()) {
            const auto tasks = queue->Tasks();
            const auto task = std::find_if(tasks.begin(), tasks.end(), [id](const auto& candidate) {
                return candidate.id == id;
            });
            if (task == tasks.end())
                return Result{ErrorModule::OLSC, 2801};
            const auto progress = queue->ProgressOf(id, 1);
            {
                std::lock_guard lock{state->mutex};
                const u64 registered = 1;
                std::memcpy(state->bytes.data() + 0x10, &registered, 8);
                if (progress) {
                    std::memcpy(state->bytes.data() + 8, &(*progress)[0], 8);
                    std::memcpy(state->bytes.data() + 0x18, &(*progress)[1], 8);
                }
                if (task->running) {
                    started = true;
                    std::memcpy(state->bytes.data() + 0x20, &registered, 8);
                }
                if (task->state == 2 || task->state == 3) {
                    state->bytes[0] = 1;
                    std::memcpy(state->bytes.data() + 0x28, &task->last_result, 4);
                }
            }
            if (task->state == 2 || task->state == 3) {
                const auto last = task->last_result;
                queue->Delete(id);
                return Result{last};
            }
            if (!started && std::chrono::steady_clock::now() >= deadline) {
                queue->Cancel(id);
                return Result{ErrorModule::OLSC, 2800};
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{200});
        }
        queue->Cancel(id);
        while (const auto current = queue->Current()) {
            if (current->id != id)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds{200});
        }
        queue->Delete(id);
        return Result{ErrorModule::OLSC, 0};
    };
    *out_task = std::make_shared<IAsyncResult>(system, worker, std::move(work), [state] {
        std::lock_guard lock{state->mutex};
        return state->bytes;
    });
    R_SUCCEED();
}

} // namespace Service::OLSC

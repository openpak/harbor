// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>

#include "core/hle/service/cmif_types.h"
#include "core/hle/service/service.h"

namespace Service::OLSC {

class INativeHandleHolder;
class IStopperObject;

/// nn::olsc::TransferTaskInfo (0x30), as the 23.0.1 sysmodule fills it from a task (0x1a9850,
/// 0x1a96b0): +0x00 task id, +0x08 type (u8; 0-1 and 2-3 are the two kinds the filters select),
/// +0x09 state (u8; 1, 2 or 3 for the running task, 0, 2 or 3 for a queued one), +0x0a a u8 the
/// task keeps beside it, +0x10 u64, +0x18 the user (Uid, 0x10), +0x28 u64 (the application id
/// by position). Field meanings beyond the id are not established.
using TransferTaskInfo = std::array<u8, 0x30>;

class ITransferTaskListController final : public ServiceFramework<ITransferTaskListController> {
public:
    explicit ITransferTaskListController(Core::System& system_);
    ~ITransferTaskListController() override;

private:
    Result GetTransferTaskEndEventNativeHandleHolder(Out<SharedPointer<INativeHandleHolder>> out_holder);
    Result GetTransferTaskStartEventNativeHandleHolder(Out<SharedPointer<INativeHandleHolder>> out_holder);
    Result StopNextTransferTaskExecution(Out<SharedPointer<IStopperObject>> out_stopper);
    Result GetTransferTaskCount(Out<u32> out_count, u8 filter);
    Result GetTransferTaskInfo(Out<TransferTaskInfo> out_info, u8 filter, u64 task_id);
    Result ListTransferTaskInfo(Out<s32> out_count,
                                OutArray<TransferTaskInfo, BufferAttr_HipcMapAlias> out_infos,
                                u8 filter, s32 offset);
    Result GetTransferTaskProgress(Out<std::array<u8, 0x18>> out_progress, u8 filter, u64 task_id);
    Result GetCurrentTransferTaskInfo(Out<TransferTaskInfo> out_info, u8 filter);
    Result FindTransferTaskInfo(Out<TransferTaskInfo> out_info, std::array<u64, 4> query);
};

} // namespace Service::OLSC

// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>

#include "core/hle/service/cmif_types.h"
#include "core/hle/service/service.h"

namespace Core {
class System;
}

namespace Kernel {
class KTransferMemory;
}

namespace Service::News {

class IDownloadContext;
class NewsShared;

class INewsService final : public ServiceFramework<INewsService> {
public:
    explicit INewsService(Core::System& system_, std::shared_ptr<NewsShared> shared_);
    ~INewsService() override;

private:
    Result PostLocalNews(InBuffer<BufferAttr_HipcAutoSelect> buffer_data);

    Result SetPassphrase(u64 application_id, InBuffer<BufferAttr_HipcPointer> buffer_data);

    Result GetSubscriptionStatus(Out<u32> out_status, InBuffer<BufferAttr_HipcPointer> topic);

    Result GetTopicList(Out<s32> out_count, OutBuffer<BufferAttr_HipcMapAlias> out_topics,
                        s32 filter);

    Result GetSaveDataUsage(Out<u64> out_used, Out<u64> out_total);

    Result IsSystemUpdateRequired(Out<bool> out_is_system_update_required, u64 version);

    Result IsSystemVersionSufficient(Out<bool> out_is_sufficient, u8 format_version,
                                     u8 semantics_version);

    Result Download(OutInterface<IDownloadContext> out_interface, u64 capacity,
                    InCopyHandle<Kernel::KTransferMemory> transfer_memory,
                    InBuffer<BufferAttr_HipcPointer> url);

    Result SetSubscriptionStatus(InBuffer<BufferAttr_HipcPointer> topic, u32 status);

    Result RequestAutoSubscription(u64 application_id);

    Result ClearStorage();

    Result ClearSubscriptionStatusAll();

    Result Unknown50100();

    std::shared_ptr<NewsShared> shared;
};

} // namespace Service::News

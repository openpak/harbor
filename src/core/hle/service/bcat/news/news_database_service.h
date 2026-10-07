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

namespace Service::News {

class NewsShared;

// nn::news::detail::ipc::INewsDatabaseService (IDeviceNewsDatabaseService since 19.0.0), over
// NewsStorage. qlaunch 22.5.0 calls Count, GetList (1000) and the two integer updates.
// CountWithKey (2) and UpdateStringValue (5) are not called and not implemented.
class INewsDatabaseService final : public ServiceFramework<INewsDatabaseService> {
public:
    explicit INewsDatabaseService(Core::System& system_, std::shared_ptr<NewsShared> shared_);
    ~INewsDatabaseService() override;

private:
    Result Count(Out<s32> out_count, InBuffer<BufferAttr_HipcPointer> where);

    Result UpdateIntegerValue(InBuffer<BufferAttr_HipcPointer> column, s32 value,
                              InBuffer<BufferAttr_HipcPointer> where);

    Result UpdateIntegerValueWithAddition(InBuffer<BufferAttr_HipcPointer> column, s32 value,
                                          InBuffer<BufferAttr_HipcPointer> where);

    Result GetListV1(Out<s32> out_count, OutBuffer<BufferAttr_HipcMapAlias> out_buffer_data,
                     InBuffer<BufferAttr_HipcPointer> where_phrase,
                     InBuffer<BufferAttr_HipcPointer> order_by_phrase, s32 offset);

    Result GetList(Out<s32> out_count, OutBuffer<BufferAttr_HipcMapAlias> out_buffer_data,
                   InBuffer<BufferAttr_HipcPointer> where_phrase,
                   InBuffer<BufferAttr_HipcPointer> order_by_phrase, s32 offset);

    std::shared_ptr<NewsShared> shared;
};

} // namespace Service::News

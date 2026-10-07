// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "core/hle/service/bcat/news/news_record.h"
#include "core/hle/service/cmif_types.h"
#include "core/hle/service/service.h"

namespace Core {
class System;
}

namespace Service::News {

class NewsShared;

// nn::news::detail::ipc::INewsDataService: one article of the news store, opened by its news:/data
// name or by the record GetList returned. qlaunch 22.5.0 opens with OpenWithNewsRecord, then reads.
class INewsDataService final : public ServiceFramework<INewsDataService> {
public:
    explicit INewsDataService(Core::System& system_, std::shared_ptr<NewsShared> shared_);
    ~INewsDataService() override;

private:
    Result OpenFile(const std::string& file_name);

    Result Open(InBuffer<BufferAttr_HipcMapAlias> name);
    Result OpenWithNewsRecordV1(NewsRecordV1 record_buffer);
    Result OpenWithNewsRecord(NewsRecord record_buffer);
    Result Read(Out<u64> out_size, s64 offset, OutBuffer<BufferAttr_HipcMapAlias> out_buffer);
    Result GetSize(Out<s64> out_size);

    std::shared_ptr<NewsShared> shared;
    std::optional<std::vector<u8>> opened_payload;
};

} // namespace Service::News

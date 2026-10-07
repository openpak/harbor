// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstring>

#include "common/logging.h"
#include "core/hle/service/bcat/news/news_data_service.h"
#include "core/hle/service/bcat/news/news_shared.h"
#include "core/hle/service/cmif_serialization.h"

// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).

namespace Service::News {
namespace {

// The console opens news:/data/<name>; a missing file is fs's PathNotFound (unverified that news
// passes it on).
constexpr Result ResultNotFound{ErrorModule::FS, 1};
// 2125-0004, the SDK's own answer when the object it needs is not there: nothing has been opened.
constexpr Result ResultNotOpened{ErrorModule::News, 4};

} // namespace

INewsDataService::INewsDataService(Core::System& system_, std::shared_ptr<NewsShared> shared_)
    : ServiceFramework{system_, "INewsDataService"}, shared{std::move(shared_)} {
    static const FunctionInfo functions[] = {
        {0, D<&INewsDataService::Open>, "Open"},
        {1, D<&INewsDataService::OpenWithNewsRecordV1>, "OpenWithNewsRecordV1"},
        {2, D<&INewsDataService::Read>, "Read"},
        {3, D<&INewsDataService::GetSize>, "GetSize"},
        {1001, D<&INewsDataService::OpenWithNewsRecord>, "OpenWithNewsRecord"},
    };
    RegisterHandlers(functions);
}

INewsDataService::~INewsDataService() = default;

Result INewsDataService::OpenFile(const std::string& file_name) {
    // A miss is a miss: no other article stands in for the one asked for.
    opened_payload = shared->store.Payload(file_name);

    LOG_DEBUG(Service_BCAT, "News data open: {}",
              opened_payload ? std::to_string(opened_payload->size()) + " bytes" : "not stored");

    R_UNLESS(opened_payload.has_value(), ResultNotFound);
    R_SUCCEED();
}

Result INewsDataService::Open(InBuffer<BufferAttr_HipcMapAlias> name) {
    const auto* text = reinterpret_cast<const char*>(name.data());
    R_RETURN(OpenFile(std::string(text, strnlen(text, name.size()))));
}

Result INewsDataService::OpenWithNewsRecordV1(NewsRecordV1 record) {
    R_RETURN(OpenFile(NewsRow::FileNameOf(RecordString(record.news_id), RecordString(record.user_id))));
}

Result INewsDataService::OpenWithNewsRecord(NewsRecord record) {
    R_RETURN(OpenFile(NewsRow::FileNameOf(RecordString(record.news_id), RecordString(record.user_id))));
}

Result INewsDataService::Read(Out<u64> out_size, s64 offset,
                              OutBuffer<BufferAttr_HipcMapAlias> out_buffer) {
    R_UNLESS(opened_payload.has_value(), ResultNotOpened);

    const auto& payload = *opened_payload;
    const size_t start = static_cast<size_t>(std::clamp<s64>(offset, 0, payload.size()));
    const size_t length = (std::min)(payload.size() - start, out_buffer.size());

    if (length > 0) {
        std::memcpy(out_buffer.data(), payload.data() + start, length);
    }

    *out_size = length;
    R_SUCCEED();
}

Result INewsDataService::GetSize(Out<s64> out_size) {
    R_UNLESS(opened_payload.has_value(), ResultNotOpened);

    *out_size = static_cast<s64>(opened_payload->size());
    R_SUCCEED();
}

} // namespace Service::News

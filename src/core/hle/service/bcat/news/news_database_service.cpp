// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstring>

#include "core/hle/service/bcat/news/news_database_service.h"
#include "core/hle/service/bcat/news/news_query.h"
#include "core/hle/service/bcat/news/news_shared.h"
#include "core/hle/service/cmif_serialization.h"

// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).

namespace Service::News {
namespace {

// 2125-0203: what hardware returned for a WHERE it would not evaluate. A query outside NewsQuery's
// grammar fails the same way.
constexpr Result ResultInvalidQuery{ErrorModule::News, 203};

std::string_view TextOf(std::span<const u8> buffer) {
    const auto* text = reinterpret_cast<const char*>(buffer.data());
    return {text, strnlen(text, buffer.size())};
}

} // namespace

INewsDatabaseService::INewsDatabaseService(Core::System& system_,
                                           std::shared_ptr<NewsShared> shared_)
    : ServiceFramework{system_, "INewsDatabaseService"}, shared{std::move(shared_)} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, D<&INewsDatabaseService::GetListV1>, "GetListV1"},
        {1, D<&INewsDatabaseService::Count>, "Count"},
        {2, nullptr, "CountWithKey"},
        {3, D<&INewsDatabaseService::UpdateIntegerValue>, "UpdateIntegerValue"},
        {4, D<&INewsDatabaseService::UpdateIntegerValueWithAddition>, "UpdateIntegerValueWithAddition"},
        {5, nullptr, "UpdateStringValue"},
        {1000, D<&INewsDatabaseService::GetList>, "GetList"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

INewsDatabaseService::~INewsDatabaseService() = default;

Result INewsDatabaseService::Count(Out<s32> out_count, InBuffer<BufferAttr_HipcPointer> where) {
    try {
        *out_count = static_cast<s32>(shared->store.Query(TextOf(where), {}).size());
    } catch (const QueryError& e) {
        LOG_WARNING(Service_BCAT, "News count query rejected: {}", e.what());
        R_RETURN(ResultInvalidQuery);
    }
    R_SUCCEED();
}

Result INewsDatabaseService::UpdateIntegerValue(InBuffer<BufferAttr_HipcPointer> column,
                                                s32 value,
                                                InBuffer<BufferAttr_HipcPointer> where) {
    try {
        const int rows = shared->store.Update(TextOf(column), value, false, TextOf(where));
        LOG_DEBUG(Service_BCAT, "News set {} {}: {} row(s)", TextOf(column), value, rows);
    } catch (const QueryError& e) {
        LOG_WARNING(Service_BCAT, "News update of '{}' rejected: {}", TextOf(column), e.what());
        R_RETURN(ResultInvalidQuery);
    }
    R_SUCCEED();
}

Result INewsDatabaseService::UpdateIntegerValueWithAddition(
    InBuffer<BufferAttr_HipcPointer> column, s32 value, InBuffer<BufferAttr_HipcPointer> where) {
    try {
        const int rows = shared->store.Update(TextOf(column), value, true, TextOf(where));
        LOG_DEBUG(Service_BCAT, "News add {} {}: {} row(s)", TextOf(column), value, rows);
    } catch (const QueryError& e) {
        LOG_WARNING(Service_BCAT, "News update of '{}' rejected: {}", TextOf(column), e.what());
        R_RETURN(ResultInvalidQuery);
    }
    R_SUCCEED();
}

Result INewsDatabaseService::GetListV1(Out<s32> out_count,
                                       OutBuffer<BufferAttr_HipcMapAlias> out_buffer,
                                       InBuffer<BufferAttr_HipcPointer> where,
                                       InBuffer<BufferAttr_HipcPointer> order, s32 offset) {
    std::vector<NewsRow> rows;
    try {
        rows = shared->store.Query(TextOf(where), TextOf(order));
    } catch (const QueryError& e) {
        LOG_WARNING(Service_BCAT, "News list query rejected: {}", e.what());
        R_RETURN(ResultInvalidQuery);
    }

    const size_t start = static_cast<size_t>((std::max)(offset, 0));
    const size_t capacity = out_buffer.size() / sizeof(NewsRecordV1);
    const size_t count = (std::min)(start < rows.size() ? rows.size() - start : 0, capacity);

    for (size_t i = 0; i < count; ++i) {
        NewsRecordV1 record;
        rows[start + i].ToWireV1(record);
        std::memcpy(out_buffer.data() + i * sizeof(NewsRecordV1), &record, sizeof(record));
    }

    *out_count = static_cast<s32>(count);
    R_SUCCEED();
}

Result INewsDatabaseService::GetList(Out<s32> out_count,
                                     OutBuffer<BufferAttr_HipcMapAlias> out_buffer,
                                     InBuffer<BufferAttr_HipcPointer> where,
                                     InBuffer<BufferAttr_HipcPointer> order, s32 offset) {
    std::vector<NewsRow> rows;
    try {
        rows = shared->store.Query(TextOf(where), TextOf(order));
    } catch (const QueryError& e) {
        LOG_WARNING(Service_BCAT, "News list query rejected: {}", e.what());
        R_RETURN(ResultInvalidQuery);
    }

    const size_t start = static_cast<size_t>((std::max)(offset, 0));
    const size_t capacity = out_buffer.size() / sizeof(NewsRecord);
    const size_t count = (std::min)(start < rows.size() ? rows.size() - start : 0, capacity);

    for (size_t i = 0; i < count; ++i) {
        NewsRecord record;
        rows[start + i].ToWire(record);
        std::memcpy(out_buffer.data() + i * sizeof(NewsRecord), &record, sizeof(record));
    }

    LOG_DEBUG(Service_BCAT, "News list from {}: {} of {}", start, count, rows.size());
    *out_count = static_cast<s32>(count);
    R_SUCCEED();
}

} // namespace Service::News

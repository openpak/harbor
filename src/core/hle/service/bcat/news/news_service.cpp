// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <cstring>

#include "common/redact.h"
#include "core/hle/kernel/k_transfer_memory.h"
#include "core/hle/service/bcat/news/download_context.h"
#include "core/hle/service/bcat/news/news_service.h"
#include "core/hle/service/bcat/news/news_shared.h"
#include "core/hle/service/cmif_serialization.h"

// The commands follow the Ryujinx Harbor (MIT-licensed Ryujinx base).

namespace Service::News {
namespace {

// 2125-0002, the SDK's own invalid-argument result.
constexpr Result ResultInvalidArgument{ErrorModule::News, 2};

// The 16 MiB quota of the news save data. Nothing enforces it.
constexpr u64 SaveDataQuota = 16ULL * 1024 * 1024;

std::string TopicOf(std::span<const u8> buffer) {
    const auto* text = reinterpret_cast<const char*>(buffer.data());
    return std::string(text, strnlen(text, buffer.size()));
}

} // namespace

INewsService::INewsService(Core::System& system_, std::shared_ptr<NewsShared> shared_)
    : ServiceFramework{system_, "INewsService"}, shared{std::move(shared_)} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {10100, D<&INewsService::PostLocalNews>, "PostLocalNews"},
        {20100, D<&INewsService::SetPassphrase>, "SetPassphrase"},
        {30100, D<&INewsService::GetSubscriptionStatus>, "GetSubscriptionStatus"},
        {30101, D<&INewsService::GetTopicList>, "GetTopicList"}, //3.0.0+
        {30110, D<&INewsService::GetSaveDataUsage>, "GetSaveDataUsage"}, //6.0.0+
        {30200, D<&INewsService::IsSystemUpdateRequired>, "IsSystemUpdateRequired"},
        {30201, D<&INewsService::IsSystemVersionSufficient>, "Unknown30201"}, //8.0.0+
        {30210, nullptr, "Unknown30210"}, //10.0.0+
        {30300, D<&INewsService::RequestImmediateReception>, "RequestImmediateReception"},
        {30400, nullptr, "DecodeArchiveFile"}, //3.0.0-18.1.0
        {30500, D<&INewsService::Download>, "Download"}, //8.0.0+
        {30900, nullptr, "Unknown30900"}, //1.0.0
        {30901, nullptr, "Unknown30901"}, //1.0.0
        {30902, nullptr, "Unknown30902"}, //1.0.0
        {40100, D<&INewsService::SetSubscriptionStatus>, "SetSubscriptionStatus"},
        {40101, D<&INewsService::RequestAutoSubscription>, "RequestAutoSubscription"}, //3.0.0+
        {40200, D<&INewsService::ClearStorage>, "ClearStorage"},
        {40201, D<&INewsService::ClearSubscriptionStatusAll>, "ClearSubscriptionStatusAll"},
        {50100, D<&INewsService::Unknown50100>, "Unknown50100"}, //23.0.0+
        {90100, nullptr, "GetNewsDatabaseDump"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

INewsService::~INewsService() = default;

Result INewsService::PostLocalNews(InBuffer<BufferAttr_HipcAutoSelect> buffer_data) {
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();

    R_UNLESS(shared->store.Post(buffer_data, now), ResultInvalidArgument);

    LOG_INFO(Service_BCAT, "Local news posted ({} bytes msgpack)", buffer_data.size());
    shared->SignalNewlyArrived();
    R_SUCCEED();
}

Result INewsService::SetPassphrase(u64 application_id,
                                   InBuffer<BufferAttr_HipcPointer> buffer_data) {
    // The passphrase decrypts delivered news archives; nothing is delivered that way, so it is
    // accepted and not kept.
    LOG_INFO(Service_BCAT, "News passphrase set for {:016x} ({} bytes); not kept", application_id,
             buffer_data.size());
    R_SUCCEED();
}

Result INewsService::GetSubscriptionStatus(Out<u32> out_status,
                                           InBuffer<BufferAttr_HipcPointer> topic) {
    *out_status = shared->GetSubscription(TopicOf(topic));
    LOG_DEBUG(Service_BCAT, "News subscription status of '{}': {}", TopicOf(topic), *out_status);
    R_SUCCEED();
}

Result INewsService::GetTopicList(Out<s32> out_count, OutBuffer<BufferAttr_HipcMapAlias> out_topics,
                                  s32 filter) {
    // 0x20-byte topic id strings: qlaunch passes room for 0x400 of them with filter 2 and asks each
    // one's subscription status. The filter's values are unknown and not applied; every topic the
    // news store holds is listed.
    constexpr size_t TopicIdSize = 0x20;

    const auto topics = shared->store.Topics();
    const size_t count = (std::min)(topics.size(), out_topics.size() / TopicIdSize);

    std::memset(out_topics.data(), 0, count * TopicIdSize);
    for (size_t i = 0; i < count; ++i) {
        std::memcpy(out_topics.data() + i * TopicIdSize, topics[i].data(),
                    (std::min)(topics[i].size(), TopicIdSize - 1));
    }

    LOG_DEBUG(Service_BCAT, "GetTopicList (filter {}): {} topic(s)", filter, count);
    *out_count = static_cast<s32>(count);
    R_SUCCEED();
}

Result INewsService::GetSaveDataUsage(Out<u64> out_used, Out<u64> out_total) {
    // The stored articles' bytes against a fixed quota (switchbrew: usage and size of the news save
    // data 0x8000000000000090).
    *out_used = shared->store.UsedBytes();
    *out_total = SaveDataQuota;
    R_SUCCEED();
}

Result INewsService::IsSystemUpdateRequired(Out<bool> out_is_system_update_required,
                                            u64 version) {
    // No system update is ever required of this unit.
    LOG_DEBUG(Service_BCAT, "IsSystemUpdateRequired for {:#x}: no", version);
    *out_is_system_update_required = false;
    R_SUCCEED();
}

Result INewsService::IsSystemVersionSufficient(Out<bool> out_is_sufficient, u8 format_version,
                                               u8 semantics_version) {
    // Unknown contract: unnamed on switchbrew (two input bytes). HOME calls it when a channel page
    // opens from Find Channels, with the channel archive's format and semantics versions. Its meaning
    // is read from the page: false shows "A system update is required to display the latest news
    // articles for this channel", true lists them. This system can show them.
    LOG_WARNING(Service_BCAT,
                "unknown contract: INewsService 30201 ({}, {}); returning true", format_version,
                semantics_version);
    *out_is_sufficient = true;
    R_SUCCEED();
}

Result INewsService::Download(OutInterface<IDownloadContext> out_interface, u64 capacity,
                              InCopyHandle<Kernel::KTransferMemory> transfer_memory,
                              InBuffer<BufferAttr_HipcPointer> url) {
    // HOME's News scene downloads the channel catalog (Find Channels) and channel records this way:
    // the news module fetches a container, decodes it into the transfer memory and reports through
    // the context. Here the decoded payload comes straight from OpenPak's news edge.
    *out_interface = std::make_shared<IDownloadContext>(system, shared, TopicOf(url),
                                                        transfer_memory.Get(), capacity);
    R_SUCCEED();
}

Result INewsService::SetSubscriptionStatus(InBuffer<BufferAttr_HipcPointer> topic, u32 status) {
    shared->SetSubscription(TopicOf(topic), status);
    R_SUCCEED();
}

Result INewsService::RequestAutoSubscription(u64 application_id) {
    LOG_DEBUG(Service_BCAT, "Auto subscription requested for {:016x}; no delivery source yet",
              application_id);
    R_SUCCEED();
}

Result INewsService::ClearStorage() {
    shared->store.Clear();
    LOG_INFO(Service_BCAT, "News storage cleared");
    R_SUCCEED();
}

Result INewsService::ClearSubscriptionStatusAll() {
    shared->ClearSubscriptions();
    LOG_INFO(Service_BCAT, "News subscriptions cleared");
    R_SUCCEED();
}

Result INewsService::RequestImmediateReception(InBuffer<BufferAttr_HipcPointer> topic) {
    // Asks the module to receive a channel now: here, fill from OpenPak, and only for a followed one.
    const auto name = TopicOf(topic);
    LOG_INFO(Service_BCAT, "News immediate reception requested for '{}'", name);
    if (shared->IsFollowed(name)) {
        shared->StartFill();
    }
    R_SUCCEED();
}

Result INewsService::Unknown50100() {
    LOG_UNKNOWN_CONTRACT(Service_BCAT, "news 50100; returning success with no output");
    R_SUCCEED();
}

} // namespace Service::News

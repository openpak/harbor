// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "common/common_types.h"

namespace Service::News {

// nn::news::NewsRecord as the guest sees it, 0x80 bytes. Verified against qlaunch 22.5.0: the 0x80
// stride, news_id at 0, decoration_type at 0x60, read at 0x64 (ListReadNum counts it), newly at 0x68.
// The rest is switchbrew's table. Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).
struct NewsRecord {
    std::array<char, 0x18> news_id{};
    std::array<char, 0x18> user_id{};
    std::array<char, 0x20> topic_id{};
    s64 received_at{};
    std::array<u8, 8> unknown_58{};
    s32 decoration_type{};
    s32 read{};
    s32 newly{};
    s32 displayed{};
    s32 feedback{};
    s32 unknown_74{};
    s32 extra_1{};
    s32 extra_2{};
};
static_assert(sizeof(NewsRecord) == 0x80);
static_assert(offsetof(NewsRecord, received_at) == 0x50);
static_assert(offsetof(NewsRecord, decoration_type) == 0x60);
static_assert(offsetof(NewsRecord, read) == 0x64);
static_assert(offsetof(NewsRecord, newly) == 0x68);
static_assert(offsetof(NewsRecord, displayed) == 0x6C);
static_assert(offsetof(NewsRecord, feedback) == 0x70);
static_assert(offsetof(NewsRecord, extra_1) == 0x78);
static_assert(offsetof(NewsRecord, extra_2) == 0x7C);

// nn::news::NewsRecordV1, 0x48 bytes, from switchbrew; qlaunch 22.5.0 never asks for it.
struct NewsRecordV1 {
    std::array<char, 0x18> news_id{};
    std::array<char, 0x18> user_id{};
    s64 received_at{};
    s32 read{};
    s32 newly{};
    s32 displayed{};
    s32 unknown_44{};
};
static_assert(sizeof(NewsRecordV1) == 0x48);
static_assert(offsetof(NewsRecordV1, read) == 0x38);

enum class IntColumn : size_t {
    ReceivedAt,
    PublishedAt,
    ExpireAt,
    PickupLimit,
    EssentialPickupLimit,
    Priority,
    DeletionPriority,
    AgeLimit,
    Surprise,
    DecorationType,
    Point,
    Read,
    Newly,
    Displayed,
    OptedIn,
    PointStatus,
    Feedback,
    Extra1,
    Extra2,
    Reputation,
    Count,
};

// The console's SQLite column names (switchbrew NewsDatabase, plus `reputation`, which qlaunch orders
// its main list by).
inline constexpr std::array<std::string_view, static_cast<size_t>(IntColumn::Count)>
    IntegerColumnNames{
        "received_at",  "published_at", "expire_at", "pickup_limit", "essential_pickup_limit",
        "priority",     "deletion_priority", "age_limit", "surprise", "decoration_type",
        "point",        "read",         "newly",     "displayed",    "opted_in",
        "point_status", "feedback",     "extra_1",   "extra_2",      "reputation",
    };

using NewsValue = std::variant<s64, std::string>;

// One row of the news database.
struct NewsRow {
    std::string news_id;
    std::string user_id;
    std::string topic_id;
    std::string application_ids;
    std::array<s64, static_cast<size_t>(IntColumn::Count)> ints{};

    s64& Int(IntColumn column) {
        return ints[static_cast<size_t>(column)];
    }
    s64 Int(IntColumn column) const {
        return ints[static_cast<size_t>(column)];
    }

    static std::optional<IntColumn> FindIntegerColumn(std::string_view name);
    static bool IsColumn(std::string_view name);

    // The value of a column by name, or nothing when it is not one.
    std::optional<NewsValue> Get(std::string_view column) const;

    // The article's name in news:/data, as the console's OpenWithNewsRecord builds it.
    std::string FileName() const;
    static std::string FileNameOf(std::string_view news_id, std::string_view user_id);

    // The row a PostLocalNews payload makes, or nothing when the payload is not a news msgpack. A local
    // post is stored as "LS" and the 20-digit id with no user, opted_in = 1 (OpenPak news server's
    // local format). decoration_type is not in the article; 1 is Eden's default.
    static std::optional<NewsRow> FromPayload(std::span<const u8> payload, s64 received_at);

    void ToWire(NewsRecord& out) const;
    void ToWireV1(NewsRecordV1& out) const;
};

// The NUL-terminated id of a fixed record field.
std::string RecordString(std::span<const char> field);

} // namespace Service::News

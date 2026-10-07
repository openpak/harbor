// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include "core/hle/service/bcat/news/news_record.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "core/hle/service/bcat/news/msgpack.h"

// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).

namespace Service::News {
namespace {

constexpr std::array<std::string_view, 4> TextColumns{"news_id", "user_id", "topic_id",
                                                      "application_ids"};

bool IsStringType(u8 byte) {
    return (byte & 0xE0) == 0xA0 || byte == 0xD9 || byte == 0xDA || byte == 0xDB;
}

bool IsMapType(u8 byte) {
    return (byte & 0xF0) == 0x80 || byte == 0xDE || byte == 0xDF;
}

bool IsUnsignedType(u8 byte) {
    return byte <= 0x7F || (byte >= 0xCC && byte <= 0xCF);
}

bool IsIntegerType(u8 byte) {
    return IsUnsignedType(byte) || byte >= 0xE0 || (byte >= 0xD0 && byte <= 0xD3);
}

// An integer value of any msgpack width, as a signed 64-bit value.
bool ReadInteger(MsgPack::Reader& reader, s64& value) {
    if (IsUnsignedType(reader.PeekType())) {
        u64 unsigned_value{};
        if (!reader.ReadUInt(unsigned_value)) {
            return false;
        }
        value = static_cast<s64>(unsigned_value);
        return true;
    }
    return reader.ReadInt(value);
}

void CopyText(std::span<char> field, const std::string& text) {
    std::memset(field.data(), 0, field.size());
    std::memcpy(field.data(), text.data(), (std::min)(text.size(), field.size() - 1));
}

} // namespace

std::string RecordString(std::span<const char> field) {
    const size_t length = strnlen(field.data(), field.size());
    return std::string(field.data(), length);
}

std::optional<IntColumn> NewsRow::FindIntegerColumn(std::string_view name) {
    const auto it = std::find(IntegerColumnNames.begin(), IntegerColumnNames.end(), name);
    if (it == IntegerColumnNames.end()) {
        return std::nullopt;
    }
    return static_cast<IntColumn>(it - IntegerColumnNames.begin());
}

bool NewsRow::IsColumn(std::string_view name) {
    return std::find(TextColumns.begin(), TextColumns.end(), name) != TextColumns.end() ||
           FindIntegerColumn(name).has_value();
}

std::optional<NewsValue> NewsRow::Get(std::string_view column) const {
    if (column == "news_id") {
        return news_id;
    }
    if (column == "user_id") {
        return user_id;
    }
    if (column == "topic_id") {
        return topic_id;
    }
    if (column == "application_ids") {
        return application_ids;
    }
    if (const auto index = FindIntegerColumn(column)) {
        return Int(*index);
    }
    return std::nullopt;
}

std::string NewsRow::FileNameOf(std::string_view news_id, std::string_view user_id) {
    if (user_id.empty()) {
        return "D00000000000000000000_" + std::string(news_id) + ".msgpack";
    }
    return "U" + std::string(user_id) + "_" + std::string(news_id) + ".msgpack";
}

std::string NewsRow::FileName() const {
    return FileNameOf(news_id, user_id);
}

std::optional<NewsRow> NewsRow::FromPayload(std::span<const u8> payload, s64 received_at) {
    MsgPack::Reader reader{payload};

    size_t count{};
    if (!reader.ReadMapHeader(count)) {
        return std::nullopt;
    }

    NewsRow row;
    bool have_id = false;
    u64 id{};
    s64 essential_pickup_limit = 0;
    s64 decoration_type = 1;

    for (size_t i = 0; i < count; ++i) {
        std::string key;
        if (!reader.ReadString(key)) {
            return std::nullopt;
        }

        const u8 type = reader.PeekType();
        s64 number{};

        if (key == "news_id") {
            // Only an unsigned integer is an id.
            if (!IsUnsignedType(type) || !reader.ReadUInt(id)) {
                return std::nullopt;
            }
            have_id = true;
        } else if (key == "topic_id" && IsStringType(type)) {
            if (!reader.ReadString(row.topic_id)) {
                return std::nullopt;
            }
        } else if (key == "essential_pickup" && IsMapType(type)) {
            size_t inner_count{};
            if (!reader.ReadMapHeader(inner_count)) {
                return std::nullopt;
            }
            for (size_t j = 0; j < inner_count; ++j) {
                std::string inner_key;
                if (!reader.ReadString(inner_key)) {
                    return std::nullopt;
                }
                if (inner_key == "pickup_limit" && IsIntegerType(reader.PeekType())) {
                    if (!ReadInteger(reader, essential_pickup_limit)) {
                        return std::nullopt;
                    }
                } else if (!reader.SkipValue()) {
                    return std::nullopt;
                }
            }
        } else if (IsIntegerType(type) &&
                   (key == "published_at" || key == "expire_at" || key == "pickup_limit" ||
                    key == "priority" || key == "deletion_priority" || key == "age_limit" ||
                    key == "surprise" || key == "decoration_type")) {
            if (!ReadInteger(reader, number)) {
                return std::nullopt;
            }
            if (key == "decoration_type") {
                decoration_type = number;
            } else if (const auto column = FindIntegerColumn(key)) {
                row.Int(*column) = number;
            }
        } else if (!reader.SkipValue()) {
            return std::nullopt;
        }
    }

    if (!have_id) {
        return std::nullopt;
    }

    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "LS%020llu", static_cast<unsigned long long>(id));
    row.news_id = buffer;
    // OpenPak articles carry no application_ids at the root, so qlaunch's N_PMATCH on it matches
    // nothing.
    row.Int(IntColumn::EssentialPickupLimit) = essential_pickup_limit;
    row.Int(IntColumn::DecorationType) = decoration_type;
    row.Int(IntColumn::ReceivedAt) = received_at;
    row.Int(IntColumn::Newly) = 1;
    row.Int(IntColumn::OptedIn) = 1;
    return row;
}

void NewsRow::ToWire(NewsRecord& out) const {
    out = {};
    CopyText(out.news_id, news_id);
    CopyText(out.user_id, user_id);
    CopyText(out.topic_id, topic_id);
    out.received_at = Int(IntColumn::ReceivedAt);
    out.decoration_type = static_cast<s32>(Int(IntColumn::DecorationType));
    out.read = static_cast<s32>(Int(IntColumn::Read));
    out.newly = static_cast<s32>(Int(IntColumn::Newly));
    out.displayed = static_cast<s32>(Int(IntColumn::Displayed));
    out.feedback = static_cast<s32>(Int(IntColumn::Feedback));
    out.extra_1 = static_cast<s32>(Int(IntColumn::Extra1));
    out.extra_2 = static_cast<s32>(Int(IntColumn::Extra2));
}

void NewsRow::ToWireV1(NewsRecordV1& out) const {
    out = {};
    CopyText(out.news_id, news_id);
    CopyText(out.user_id, user_id);
    out.received_at = Int(IntColumn::ReceivedAt);
    out.read = static_cast<s32>(Int(IntColumn::Read));
    out.newly = static_cast<s32>(Int(IntColumn::Newly));
    out.displayed = static_cast<s32>(Int(IntColumn::Displayed));
}

} // namespace Service::News

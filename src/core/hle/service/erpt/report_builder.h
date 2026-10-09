// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// erpt contexts and the report body, as firmware 23.0.1's erpt checks and writes them.
//
// A context is a 0x160-byte ContextEntry (u32 version, u32 field count <= 20, u32 category, then
// 20 16-byte FieldEntry {u32 id, u8 type, value at +8}) with an array buffer beside it; array and
// string values are {u32 start, u32 size} byte ranges in that buffer. Every field must exist in
// the field table with the entry's type and category. A report is a MessagePack map of field
// name to value, one flat map over every context it gathers, which is what qlaunch's Error
// History reads. Fields marked private in the table are never written.
//
// Standard library only, so it can be checked on its own: see erpt_store_selfcheck.cpp.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "core/hle/service/erpt/erpt_fields.h"
#include "core/hle/service/erpt/msgpack_writer.h"

namespace Service::ERPT {

enum FieldType : std::uint8_t {
    FieldType_U64 = 0,
    FieldType_U32 = 1,
    FieldType_I64 = 2,
    FieldType_I32 = 3,
    FieldType_String = 4,
    FieldType_U8Array = 5,
    FieldType_U32Array = 6,
    FieldType_U64Array = 7,
    FieldType_I32Array = 8,
    FieldType_I64Array = 9,
    FieldType_Bool = 10,
    FieldType_U16 = 11,
    FieldType_U8 = 12,
    FieldType_I16 = 13,
    FieldType_I8 = 14,
    FieldType_I8Array = 15,
};

constexpr std::uint32_t Category_ErrorInfo = 1;
constexpr std::uint32_t Category_ApplicationInfo = 16;
constexpr std::uint32_t Category_ErrorInfoAuto = 59;
constexpr std::uint32_t Category_SystemSettingInfo = 151;

constexpr std::size_t ContextEntrySize = 0x160;
constexpr std::size_t MaxFieldsPerContext = 20;
constexpr std::size_t MaxArrayBufferSize = 0x18000;

/// erpt results (module 147).
enum class ErptError {
    None,
    InvalidArgument,       // 2147-0007
    NotFound,              // 2147-0008
    FieldCategoryMismatch, // 2147-0009
    FieldTypeMismatch,     // 2147-0010
    RequiredContextMissing, // 2147-0014
    RequiredFieldMissing,  // 2147-0015
    ArrayFieldTooLarge,    // 2147-0018
};

inline const FieldInfo* FindField(std::uint32_t id) {
    const auto it = std::lower_bound(FieldTable.begin(), FieldTable.end(), id,
                                     [](const FieldInfo& f, std::uint32_t v) { return f.id < v; });
    return it != FieldTable.end() && it->id == id ? &*it : nullptr;
}

inline const FieldInfo* FindFieldByName(std::string_view name) {
    for (const auto& f : FieldTable) {
        if (name == f.name) {
            return &f;
        }
    }
    return nullptr;
}

/// A field's value: integers, a bool, a string or byte array (bytes), or an integer array.
using FieldValue =
    std::variant<std::uint64_t, std::int64_t, bool, std::string, std::vector<std::uint8_t>,
                 std::vector<std::uint64_t>, std::vector<std::int64_t>>;

struct Field {
    const FieldInfo* info;
    FieldValue value;
};

struct Context {
    std::uint32_t category{};
    std::vector<Field> fields;

    const Field* Get(std::uint32_t id) const {
        for (const auto& f : fields) {
            if (f.info->id == id) {
                return &f;
            }
        }
        return nullptr;
    }

    void Set(std::uint32_t id, FieldValue value) {
        const auto* info = FindField(id);
        if (info == nullptr) {
            return;
        }
        for (auto& f : fields) {
            if (f.info == info) {
                f.value = std::move(value);
                return;
            }
        }
        fields.push_back({info, std::move(value)});
    }
};

namespace Detail {

template <typename T>
T Load(std::span<const std::uint8_t> data, std::size_t offset) {
    T value{};
    std::memcpy(&value, data.data() + offset, sizeof(T));
    return value;
}

template <typename T, typename Out>
std::vector<Out> LoadArray(std::span<const std::uint8_t> bytes) {
    std::vector<Out> out(bytes.size() / sizeof(T));
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<Out>(Load<T>(bytes, i * sizeof(T)));
    }
    return out;
}

} // namespace Detail

/// Reads and checks fields given as FieldEntry records over an array buffer, for one category.
inline ErptError ParseFields(std::uint32_t category, std::span<const std::uint8_t> entries,
                             std::size_t field_count, std::span<const std::uint8_t> array_buffer,
                             Context& out) {
    using namespace Detail;
    if (field_count > MaxFieldsPerContext || entries.size() < field_count * 0x10 ||
        array_buffer.size() > MaxArrayBufferSize) {
        return ErptError::InvalidArgument;
    }
    out.category = category;
    out.fields.clear();
    for (std::size_t i = 0; i < field_count; ++i) {
        const auto entry = entries.subspan(i * 0x10, 0x10);
        const auto id = Load<std::uint32_t>(entry, 0);
        const auto type = Load<std::uint8_t>(entry, 4);
        const auto* info = FindField(id);
        if (info == nullptr || type > FieldType_I8Array) {
            return ErptError::InvalidArgument;
        }
        if (info->type != type) {
            return ErptError::FieldTypeMismatch;
        }
        if (info->category != category) {
            return ErptError::FieldCategoryMismatch;
        }
        FieldValue value;
        switch (type) {
        case FieldType_U64:
            value = Load<std::uint64_t>(entry, 8);
            break;
        case FieldType_U32:
            value = std::uint64_t{Load<std::uint32_t>(entry, 8)};
            break;
        case FieldType_U16:
            value = std::uint64_t{Load<std::uint16_t>(entry, 8)};
            break;
        case FieldType_U8:
            value = std::uint64_t{Load<std::uint8_t>(entry, 8)};
            break;
        case FieldType_I64:
            value = Load<std::int64_t>(entry, 8);
            break;
        case FieldType_I32:
            value = std::int64_t{Load<std::int32_t>(entry, 8)};
            break;
        case FieldType_I16:
            value = std::int64_t{Load<std::int16_t>(entry, 8)};
            break;
        case FieldType_I8:
            value = std::int64_t{Load<std::int8_t>(entry, 8)};
            break;
        case FieldType_Bool:
            value = Load<std::uint8_t>(entry, 8) != 0;
            break;
        default: {
            const auto start = Load<std::uint32_t>(entry, 8);
            const auto size = Load<std::uint32_t>(entry, 12);
            if (std::uint64_t{start} + size > array_buffer.size()) {
                return ErptError::InvalidArgument;
            }
            if (size > info->max_size) {
                return ErptError::ArrayFieldTooLarge;
            }
            const auto bytes = array_buffer.subspan(start, size);
            switch (type) {
            case FieldType_String: {
                const auto* text = reinterpret_cast<const char*>(bytes.data());
                value = std::string(text, strnlen(text, bytes.size()));
                break;
            }
            case FieldType_U8Array:
                value = std::vector<std::uint8_t>(bytes.begin(), bytes.end());
                break;
            case FieldType_U32Array:
                value = LoadArray<std::uint32_t, std::uint64_t>(bytes);
                break;
            case FieldType_U64Array:
                value = LoadArray<std::uint64_t, std::uint64_t>(bytes);
                break;
            case FieldType_I32Array:
                value = LoadArray<std::int32_t, std::int64_t>(bytes);
                break;
            case FieldType_I64Array:
                value = LoadArray<std::int64_t, std::int64_t>(bytes);
                break;
            case FieldType_I8Array:
                value = LoadArray<std::int8_t, std::int64_t>(bytes);
                break;
            default:
                return ErptError::InvalidArgument;
            }
            break;
        }
        }
        out.fields.push_back({info, std::move(value)});
    }
    return ErptError::None;
}

/// A 0x160-byte ContextEntry and its array buffer.
inline ErptError ParseContext(std::span<const std::uint8_t> entry,
                              std::span<const std::uint8_t> array_buffer, Context& out) {
    if (entry.size() != ContextEntrySize) {
        return ErptError::InvalidArgument;
    }
    const auto field_count = Detail::Load<std::uint32_t>(entry, 4);
    const auto category = Detail::Load<std::uint32_t>(entry, 8);
    return ParseFields(category, entry.subspan(0x10, MaxFieldsPerContext * 0x10), field_count,
                       array_buffer, out);
}

/// What CreateReport asks of the caller's context: ErrorInfo with an ErrorCode string.
inline ErptError CheckReportContext(const Context& context) {
    if (context.category != Category_ErrorInfo) {
        return ErptError::RequiredContextMissing;
    }
    const auto* code = context.Get(10);
    if (code == nullptr || !std::holds_alternative<std::string>(code->value) ||
        std::get<std::string>(code->value).size() >= 16) {
        return ErptError::RequiredFieldMissing;
    }
    return ErptError::None;
}

/// The report body: one flat map over the contexts, earlier contexts winning a duplicate name,
/// private fields left out.
inline std::vector<std::uint8_t> BuildReportBody(std::span<const Context* const> contexts) {
    std::vector<const Field*> fields;
    std::vector<std::uint32_t> seen;
    for (const auto* context : contexts) {
        for (const auto& field : context->fields) {
            if (field.info->is_private ||
                std::find(seen.begin(), seen.end(), field.info->id) != seen.end()) {
                continue;
            }
            seen.push_back(field.info->id);
            fields.push_back(&field);
        }
    }
    MsgPackWriter w;
    w.MapHeader(static_cast<std::uint32_t>(fields.size()));
    for (const auto* field : fields) {
        w.String(field->info->name);
        std::visit(
            [&](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, std::uint64_t>) {
                    w.UInt(v);
                } else if constexpr (std::is_same_v<T, std::int64_t>) {
                    w.Int(v);
                } else if constexpr (std::is_same_v<T, bool>) {
                    w.Bool(v);
                } else if constexpr (std::is_same_v<T, std::string>) {
                    w.String(v);
                } else if constexpr (std::is_same_v<T, std::vector<std::uint8_t>>) {
                    w.Binary(v);
                } else if constexpr (std::is_same_v<T, std::vector<std::uint64_t>>) {
                    w.ArrayHeader(static_cast<std::uint32_t>(v.size()));
                    for (const auto e : v) {
                        w.UInt(e);
                    }
                } else {
                    w.ArrayHeader(static_cast<std::uint32_t>(v.size()));
                    for (const auto e : v) {
                        w.Int(e);
                    }
                }
            },
            field->value);
    }
    return w.Data();
}

/// "%02x..." of the 16 UUID bytes in the 8-4-4-4-12 grouping (ReportIdentifier).
inline std::string ReportIdentifierString(std::span<const std::uint8_t> uuid) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (std::size_t i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            out.push_back('-');
        }
        out.push_back(digits[uuid[i] >> 4]);
        out.push_back(digits[uuid[i] & 0xf]);
    }
    return out;
}

} // namespace Service::ERPT

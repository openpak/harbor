// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// The MessagePack subset error reports use: a map of field names to integers, booleans, strings,
// binary data and arrays of integers. Big endian, smallest encoding that holds the value, as the
// MessagePack specification asks. Standard library only, so it can be checked on its own (see
// erpt_store_selfcheck.cpp).

#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace Service::ERPT {

class MsgPackWriter {
public:
    void MapHeader(std::uint32_t count) {
        if (count < 16) {
            Byte(static_cast<std::uint8_t>(0x80 | count));
        } else if (count <= 0xffff) {
            Byte(0xde);
            Be(count, 2);
        } else {
            Byte(0xdf);
            Be(count, 4);
        }
    }

    void ArrayHeader(std::uint32_t count) {
        if (count < 16) {
            Byte(static_cast<std::uint8_t>(0x90 | count));
        } else if (count <= 0xffff) {
            Byte(0xdc);
            Be(count, 2);
        } else {
            Byte(0xdd);
            Be(count, 4);
        }
    }

    void String(std::string_view text) {
        const auto size = static_cast<std::uint32_t>(text.size());
        if (size < 32) {
            Byte(static_cast<std::uint8_t>(0xa0 | size));
        } else if (size <= 0xff) {
            Byte(0xd9);
            Be(size, 1);
        } else if (size <= 0xffff) {
            Byte(0xda);
            Be(size, 2);
        } else {
            Byte(0xdb);
            Be(size, 4);
        }
        out.insert(out.end(), text.begin(), text.end());
    }

    void Binary(std::span<const std::uint8_t> data) {
        const auto size = static_cast<std::uint32_t>(data.size());
        if (size <= 0xff) {
            Byte(0xc4);
            Be(size, 1);
        } else if (size <= 0xffff) {
            Byte(0xc5);
            Be(size, 2);
        } else {
            Byte(0xc6);
            Be(size, 4);
        }
        out.insert(out.end(), data.begin(), data.end());
    }

    void Bool(bool value) {
        Byte(value ? 0xc3 : 0xc2);
    }

    void UInt(std::uint64_t value) {
        if (value < 0x80) {
            Byte(static_cast<std::uint8_t>(value));
        } else if (value <= 0xff) {
            Byte(0xcc);
            Be(value, 1);
        } else if (value <= 0xffff) {
            Byte(0xcd);
            Be(value, 2);
        } else if (value <= 0xffffffff) {
            Byte(0xce);
            Be(value, 4);
        } else {
            Byte(0xcf);
            Be(value, 8);
        }
    }

    void Int(std::int64_t value) {
        if (value >= 0) {
            UInt(static_cast<std::uint64_t>(value));
        } else if (value >= -32) {
            Byte(static_cast<std::uint8_t>(value));
        } else if (value >= INT8_MIN) {
            Byte(0xd0);
            Be(static_cast<std::uint64_t>(value), 1);
        } else if (value >= INT16_MIN) {
            Byte(0xd1);
            Be(static_cast<std::uint64_t>(value), 2);
        } else if (value >= INT32_MIN) {
            Byte(0xd2);
            Be(static_cast<std::uint64_t>(value), 4);
        } else {
            Byte(0xd3);
            Be(static_cast<std::uint64_t>(value), 8);
        }
    }

    const std::vector<std::uint8_t>& Data() const {
        return out;
    }

private:
    void Byte(std::uint8_t value) {
        out.push_back(value);
    }

    void Be(std::uint64_t value, int bytes) {
        for (int i = bytes - 1; i >= 0; --i) {
            out.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
        }
    }

    std::vector<std::uint8_t> out;
};

} // namespace Service::ERPT

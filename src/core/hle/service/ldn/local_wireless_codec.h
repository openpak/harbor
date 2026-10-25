// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Service::LDN::LocalWirelessCodec {
constexpr std::size_t HeaderSize = 12;
constexpr std::size_t MaxPayload = 0x480; // NetworkInfo; NodeInfo is 0x40.

inline std::uint16_t Read16(std::span<const std::uint8_t> data, std::size_t at) {
    return static_cast<std::uint16_t>(data[at] | (data[at + 1] << 8));
}

inline bool ValidHeader(std::span<const std::uint8_t> data) {
    return data.size() >= HeaderSize && data[0] == 0 && data[1] == 0x14 && data[2] == 0x45 &&
           data[3] == 0x11 && data[4] <= 3 && data[5] <= 1 && Read16(data, 6) <= MaxPayload &&
           Read16(data, 8) <= MaxPayload;
}

// Transmit uncompressed frames (supported by the peer); accept its zero-run encoding.
inline std::vector<std::uint8_t> Encode(std::uint8_t type, std::span<const std::uint8_t> body) {
    if (type > 3 || body.size() > MaxPayload) {
        return {};
    }
    std::vector<std::uint8_t> result{0,
                                     0x14,
                                     0x45,
                                     0x11,
                                     type,
                                     0,
                                     static_cast<std::uint8_t>(body.size()),
                                     static_cast<std::uint8_t>(body.size() >> 8),
                                     0,
                                     0,
                                     0,
                                     0};
    result.insert(result.end(), body.begin(), body.end());
    return result;
}

inline std::optional<std::vector<std::uint8_t>> Decode(std::span<const std::uint8_t> frame) {
    if (!ValidHeader(frame) || frame.size() != HeaderSize + Read16(frame, 6)) {
        return std::nullopt;
    }
    const auto body = frame.subspan(HeaderSize);
    if (frame[5] == 0) {
        return std::vector<std::uint8_t>{body.begin(), body.end()};
    }
    const auto expected = Read16(frame, 8);
    std::vector<std::uint8_t> result;
    result.reserve(expected);
    for (std::size_t i = 0; i < body.size(); ++i) {
        std::size_t count = 1;
        if (body[i] == 0) {
            if (++i == body.size()) {
                return std::nullopt;
            }
            count += body[i];
            if (result.size() + count > expected) {
                return std::nullopt;
            }
            result.insert(result.end(), count, 0);
        } else {
            if (result.size() == expected) {
                return std::nullopt;
            }
            result.push_back(body[i]);
        }
    }
    if (result.size() != expected) {
        return std::nullopt;
    }
    return result;
}
} // namespace Service::LDN::LocalWirelessCodec

// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <span>
#include <cstring>
#include <string>

#include "common/common_types.h"

namespace Service::PSC {

/// One overlay notification: 0x80 bytes the server never looks inside. Senders lay it out as a u32
/// tag, a u32 payload size, then up to 0x78 bytes of payload.
using OverlayNotification = std::array<u64, 0x10>;
static_assert(sizeof(OverlayNotification) == 0x80, "OverlayNotification has incorrect size");

/// Builds a notification from a tag and up to 0x78 bytes of payload (the rest is truncated).
inline OverlayNotification MakeNotification(u32 tag, std::span<const u8> payload = {}) {
    constexpr std::size_t max_payload = sizeof(OverlayNotification) - 8;
    const auto size = std::min(payload.size(), max_payload);
    OverlayNotification notification{};
    notification[0] = tag | (u64{static_cast<u32>(size)} << 32);
    std::memcpy(reinterpret_cast<u8*>(notification.data()) + 8, payload.data(), size);
    return notification;
}

constexpr u32 GetNotificationTag(const OverlayNotification& notification) {
    return static_cast<u32>(notification[0]);
}

constexpr u32 GetNotificationPayloadSize(const OverlayNotification& notification) {
    return static_cast<u32>(notification[0] >> 32);
}

/// The name a sender opens and a receiver attaches: 16 bytes, compared whole.
struct SourceName {
    std::array<char, 0x10> name;

    std::string GetString() const {
        return std::string(name.data(), strnlen(name.data(), name.size()));
    }

    bool operator==(const SourceName&) const = default;
    auto operator<=>(const SourceName&) const = default;
};
static_assert(sizeof(SourceName) == 0x10, "SourceName has incorrect size");

} // namespace Service::PSC

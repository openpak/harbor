// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <atomic>
#include <common/socket_types.h>
#include <mutex>

namespace Network {

struct EmuNetState {

    static EmuNetState& Get();

    EmuNetState(const EmuNetState&) = delete;
    EmuNetState& operator=(const EmuNetState&) = delete;
    EmuNetState(EmuNetState&&) = delete;
    EmuNetState& operator=(EmuNetState&&) = delete;

    std::atomic<bool> wifi_enabled{true};
    std::atomic<bool> ethernet_enabled{true};
    // Guest wake-on-WLAN configuration. Harbor uses the host network and does
    // not program host radio wake timers or alter TCP socket keepalive behavior.
    std::atomic<s32> wowl_delayed_wake_time{0};
    std::atomic<s32> wowl_tcp_keepalive_timeout{0};

    std::mutex mtx;
    bool connected = false;
    bool via_wifi = false;
    char ssid[20] = {};
    u8 bars = 0;
    bool secure = false;
    IPv4Address ip{}, mask{}, gw{};

private:
    EmuNetState() = default;
};

void RefreshFromHost();
u8 QualityToBars(u8 quality);

} // namespace Network

// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Service::LDN {

// First four values are the ldn_mitm wire opcodes. Disconnects use TCP EOF, not packets.
enum class LocalWirelessMessage : std::uint8_t {
    Scan,
    ScanResponse,
    Connect,
    Sync,
    Disconnect,
    Destroy,
};

struct LocalWirelessPacket {
    LocalWirelessMessage type{};
    std::array<std::uint8_t, 4> peer{};
    std::vector<std::uint8_t> data;
};

// Owns a single IO thread. Callbacks run on it; Send/SetHosting only enqueue work.
// Destroy outside the callback's mutex: Stop joins the IO thread.
class LocalWirelessTransport {
public:
    using Address = std::array<std::uint8_t, 4>;
    using Receiver = std::function<void(LocalWirelessPacket)>;
    static constexpr std::uint16_t Port = 11452;

    LocalWirelessTransport();
    ~LocalWirelessTransport();
    bool Start(Address address, Address mask, Receiver receiver, std::string& error);
    void Stop();
    void SetHosting(bool hosting);
    void Send(LocalWirelessPacket packet);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace Service::LDN

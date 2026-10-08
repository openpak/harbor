// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Self-check for psm's battery notices (translated from the Ryujinx Harbor's
// OverlaySendersTests.BatteryNoticesFollowPscsBands). Eden's unit tests are off in the Harbor
// build, so this is a standalone program:
//
//   g++ -std=c++23 -I <src> host_battery_selfcheck.cpp -o battery_selfcheck && ./battery_selfcheck
//
// It prints "battery self-check: OK" and exits 0, or names the failed check and exits 1.

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "core/hle/service/psc/ovln/ovln_agent.h"
#include "core/hle/service/ptm/host_battery.h"

using namespace Service::PTM;
using namespace Service::PSC;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAILED line %d: %s\n", __LINE__, #cond);                         \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)

namespace {

SourceName Overlay() {
    SourceName name{};
    constexpr char text[] = "overlay";
    for (std::size_t i = 0; i + 1 < sizeof(text); ++i) {
        name.name[i] = text[i];
    }
    return name;
}

// psm's poll, as PSM::PollBattery runs it: the first reading announces nothing.
struct Poller {
    OvlnAgent& agent;
    std::shared_ptr<OvlnAgent::SenderState> sender;
    bool seeded{};
    BatteryState last{};

    void Update(int percentage, ChargerType charger) {
        const BatteryState now = ClampBattery(percentage, charger);
        if (!seeded) {
            seeded = true;
            last = now;
            return;
        }
        if (now == last) {
            return;
        }
        const BatteryNotices notices = CompareBattery(last, now);
        last = now;
        const auto payload = BatteryNoticePayload(now);
        if (notices.charger_changed) {
            CHECK(agent.Send(*sender, OverlaySendOption,
                             MakeNotification(OverlayTagChargerChanged, payload),
                             0) == OvlnStatus::Success);
        }
        if (notices.low) {
            CHECK(agent.Send(*sender, OverlaySendOption,
                             MakeNotification(OverlayTagBatteryLow, payload),
                             0) == OvlnStatus::Success);
        }
    }
};

struct Received {
    u32 tag;
    std::vector<u8> payload;
};

std::vector<Received> Drain(OvlnAgent& agent, OvlnAgent::ReceiverState& receiver) {
    std::vector<Received> out;
    OverlayNotification notification{};
    u64 tick{};
    while (agent.Receive(receiver, notification, tick) == OvlnStatus::Success) {
        const auto* bytes = reinterpret_cast<const u8*>(notification.data()) + 8;
        out.push_back({GetNotificationTag(notification),
                       std::vector<u8>(bytes, bytes + GetNotificationPayloadSize(notification))});
    }
    return out;
}

std::vector<u32> Tags(const std::vector<Received>& messages) {
    std::vector<u32> tags;
    for (const auto& message : messages) {
        tags.push_back(message.tag);
    }
    return tags;
}

} // namespace

int main() {
    CHECK(BatteryBand(1) == 0 && BatteryBand(2) == 1 && BatteryBand(5) == 1 &&
          BatteryBand(6) == 2 && BatteryBand(15) == 2 && BatteryBand(16) == 3);

    OvlnAgent agent;
    OvlnAgent::ReceiverState receiver{[] {}, [] {}};
    CHECK(agent.AddSource(receiver, Overlay()) == OvlnStatus::Success);

    Poller battery{agent, nullptr};
    CHECK(agent.OpenSender(Overlay(), OverlaySenderQueueLength, battery.sender) ==
          OvlnStatus::Success);

    // The first reading is the starting point; nothing changed after it.
    battery.Update(100, ChargerType::RegularCharger);
    battery.Update(100, ChargerType::RegularCharger);
    CHECK(Drain(agent, receiver).empty());

    battery.Update(40, ChargerType::Unplugged);
    auto unplugged = Drain(agent, receiver);
    CHECK(unplugged.size() == 1);
    CHECK(unplugged[0].tag == 3001); // the charger state changed
    CHECK((unplugged[0].payload == std::vector<u8>{40, 0, 0, 0, 0, 0, 0, 0}));

    battery.Update(15, ChargerType::Unplugged);
    CHECK(Tags(Drain(agent, receiver)) == std::vector<u32>{3002}); // into the 6-15 % band

    battery.Update(10, ChargerType::Unplugged);
    CHECK(Drain(agent, receiver).empty()); // the same band

    battery.Update(5, ChargerType::Unplugged);
    CHECK(Tags(Drain(agent, receiver)) == std::vector<u32>{3002}); // into the 2-5 % band

    battery.Update(1, ChargerType::Unplugged);
    CHECK(Tags(Drain(agent, receiver)) == std::vector<u32>{3002}); // into the last band

    battery.Update(30, ChargerType::RegularCharger);
    auto charging = Drain(agent, receiver);
    CHECK(Tags(charging) == std::vector<u32>{3001}); // charging again; a higher band is no notice
    CHECK((charging[0].payload == std::vector<u8>{30, 0, 0, 0, 1, 0, 0, 0}));

    // Queue 1, option 0x100: a second notice replaces one the overlay has not taken yet.
    battery.Update(20, ChargerType::Unplugged);
    battery.Update(4, ChargerType::Unplugged);
    CHECK(Tags(Drain(agent, receiver)) == std::vector<u32>{3002});

    std::puts("battery self-check: OK");
    return 0;
}

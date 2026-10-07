// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Self-check for the ovln agent (translated from the Ryujinx Harbor's OvlnAgentTests). Eden's unit
// tests are off in the Harbor build, so this is a standalone program:
//
//   g++ -std=c++23 -I <src> ovln_agent_selfcheck.cpp -o ovln_selfcheck && ./ovln_selfcheck
//
// It prints "ovln agent self-check: OK" and exits 0, or names the failed check and exits 1.

#include <cstdio>
#include <cstdlib>

#include "core/hle/service/psc/ovln/ovln_agent.h"

using namespace Service::PSC;

namespace {

int signals = 0;
int clears = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAILED line %d: %s\n", __LINE__, #cond);                         \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)

SourceName Name(const char* text) {
    SourceName name{};
    for (std::size_t i = 0; i < name.name.size() && text[i] != '\0'; ++i) {
        name.name[i] = text[i];
    }
    return name;
}

OverlayNotification Message(u32 tag) {
    OverlayNotification notification{};
    notification[0] = tag | (u64{4} << 32); // tag, then a 4 byte payload
    return notification;
}

std::unique_ptr<OvlnAgent::ReceiverState> NewReceiver() {
    return std::make_unique<OvlnAgent::ReceiverState>([] { ++signals; }, [] { ++clears; });
}

void Layouts() {
    CHECK(sizeof(OverlayNotification) == 0x80);
    CHECK(sizeof(SourceName) == 0x10);
    CHECK(Name("overlay").GetString() == "overlay");
    // A name that fills all 16 bytes has no terminator.
    CHECK(Name("0123456789abcdef").GetString() == "0123456789abcdef");
    CHECK(GetNotificationTag(Message(7004)) == 7004);
    CHECK(GetNotificationPayloadSize(Message(7004)) == 4);
}

void KeptUntilAReceiverAttachesThenOneFifo() {
    OvlnAgent agent;
    const auto overlay = Name("overlay");
    std::shared_ptr<OvlnAgent::SenderState> friends, home;
    CHECK(agent.OpenSender(overlay, 16, friends) == OvlnStatus::Success);
    CHECK(agent.OpenSender(overlay, 32, home) == OvlnStatus::Success);

    CHECK(agent.Send(*friends, 1, Message(7004), 100) == OvlnStatus::Success);
    CHECK(agent.Send(*home, 1, Message(23), 200) == OvlnStatus::Success);

    auto receiver = NewReceiver();
    CHECK(agent.AddSource(*receiver, overlay) == OvlnStatus::Success);
    CHECK(signals == 1);

    // A second receiver may not take the same source.
    auto other = NewReceiver();
    CHECK(agent.AddSource(*other, overlay) == OvlnStatus::SourceAlreadyAttached);

    OverlayNotification first{}, second{}, none{};
    u64 tick = 0;
    CHECK(agent.Receive(*receiver, first, tick) == OvlnStatus::Success);
    CHECK(tick == 100);
    CHECK(agent.Receive(*receiver, second, tick) == OvlnStatus::Success);
    CHECK(tick == 200);
    CHECK(agent.Receive(*receiver, none, tick) == OvlnStatus::NoMessage);
    CHECK(GetNotificationTag(first) == 7004 && GetNotificationPayloadSize(first) == 4);
    CHECK(GetNotificationTag(second) == 23);
    CHECK(clears == 1);

    // The next message signals again.
    CHECK(agent.Send(*friends, 1, Message(7002), 300) == OvlnStatus::Success);
    CHECK(signals == 2);
    agent.CloseReceiver(*receiver);
}

void ReceiverAddedFirstSeesLaterSends() {
    OvlnAgent agent;
    const auto overlay = Name("overlay");
    auto receiver = NewReceiver();
    CHECK(agent.AddSource(*receiver, overlay) == OvlnStatus::Success);
    const int before = signals;

    std::shared_ptr<OvlnAgent::SenderState> sender;
    CHECK(agent.OpenSender(overlay, 4, sender) == OvlnStatus::Success);
    CHECK(agent.Send(*sender, 1, Message(5), 1) == OvlnStatus::Success);
    CHECK(signals == before + 1);

    // A sender on another source is not seen.
    std::shared_ptr<OvlnAgent::SenderState> elsewhere;
    CHECK(agent.OpenSender(Name("elsewhere"), 4, elsewhere) == OvlnStatus::Success);
    CHECK(agent.Send(*elsewhere, 1, Message(6), 2) == OvlnStatus::Success);

    OverlayNotification got{};
    u64 tick = 0;
    CHECK(agent.Receive(*receiver, got, tick) == OvlnStatus::Success);
    CHECK(GetNotificationTag(got) == 5);
    // Empty: 2025-0012 (0x1819) is what the service answers for this.
    CHECK(agent.Receive(*receiver, got, tick) == OvlnStatus::NoMessage);
    agent.CloseReceiver(*receiver);
}

void FullQueuePolicies() {
    OvlnAgent agent;
    const auto overlay = Name("overlay");
    auto receiver = NewReceiver();
    CHECK(agent.AddSource(*receiver, overlay) == OvlnStatus::Success);

    // am, qlaunch: queue 1, option 0x100: a new message replaces the waiting one.
    std::shared_ptr<OvlnAgent::SenderState> replacing;
    CHECK(agent.OpenSender(overlay, 1, replacing) == OvlnStatus::Success);
    CHECK(agent.Send(*replacing, 0x100, Message(1), 0) == OvlnStatus::Success);
    CHECK(agent.Send(*replacing, 0x100, Message(2), 0) == OvlnStatus::Success);
    CHECK(agent.GetUnreceivedCount(*replacing) == 1);

    OverlayNotification kept{};
    u64 tick = 0;
    CHECK(agent.Receive(*receiver, kept, tick) == OvlnStatus::Success);
    CHECK(GetNotificationTag(kept) == 2);

    // Option 1 (byte 1 = 0): a full queue fails.
    std::shared_ptr<OvlnAgent::SenderState> failing;
    CHECK(agent.OpenSender(overlay, 1, failing) == OvlnStatus::Success);
    CHECK(agent.Send(*failing, 1, Message(3), 0) == OvlnStatus::Success);
    CHECK(agent.Send(*failing, 1, Message(4), 0) == OvlnStatus::QueueFull);

    std::shared_ptr<OvlnAgent::SenderState> unused;
    CHECK(agent.OpenSender(overlay, 0, unused) == OvlnStatus::InvalidArgument);
    CHECK(agent.Send(*failing, 2, Message(5), 0) == OvlnStatus::InvalidArgument);

    // Closing a sender drops what it queued and clears the event when nothing is left.
    const int clears_before = clears;
    agent.CloseSender(*failing);
    CHECK(clears == clears_before + 1);
    CHECK(agent.Receive(*receiver, kept, tick) == OvlnStatus::NoMessage);
    agent.CloseReceiver(*receiver);
}

} // namespace

int main() {
    Layouts();
    KeptUntilAReceiverAttachesThenOneFifo();
    ReceiverAddedFirstSeesLaterSends();
    FullQueuePolicies();
    std::puts("ovln agent self-check: OK");
    return 0;
}

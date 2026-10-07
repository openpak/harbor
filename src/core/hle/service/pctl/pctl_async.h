// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// The pairing commands' async half, as pctl runs it: each Async command starts work in one of two
// slots per session (round robin, a fresh non-zero id that neither slot holds; .text 0x2f460) and
// answers AsyncData plus an event the guest waits on; the matching Finish command takes the slot
// back and returns the work's result. An AsyncData that names no live slot is 2142-0221. Starting
// over a slot still running drops that work. This header is the bookkeeping only (ids and slots);
// the service runs the work on an OpenPak::AsyncWorker. Pure, so check/pctl_check.cpp tests it.
// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>

namespace Service::PCTL {

/// nn::pctl::detail::AsyncData: which of the session's two slots holds the work, and the id that
/// proves it is still the same work. 8 bytes, 4-aligned: FinishUnlinkPairing carries it at +4
/// after a bool (qlaunch's proxy 0x8560b20); pctl reads id at +0 and index at +4 (.text 0x2f5c8).
struct AsyncData {
    std::uint32_t id = 0;
    std::uint32_t index = 0;
};
static_assert(sizeof(AsyncData) == 8 && alignof(AsyncData) == 4);

/// nn::pctl::detail::PairingInfoBase: the moon device id, the state (1 found, 2 active) and a byte
/// set for a pairing copied from another console. The client never reads it; it hands it back.
struct PairingInfoBase {
    std::uint64_t device_id = 0;
    std::uint32_t state = 0;
    std::uint8_t is_copy = 0;
    std::uint8_t padding[3]{};
};
static_assert(sizeof(PairingInfoBase) == 0x10);

/// nn::pctl::detail::PairingAccountInfoBase: the device id and an owner's Nintendo Account id.
struct PairingAccountInfoBase {
    std::uint64_t device_id = 0;
    std::uint64_t nintendo_account_id = 0;
};
static_assert(sizeof(PairingAccountInfoBase) == 0x10);

class AsyncSlots {
public:
    static constexpr std::size_t Count = 2;

    /// Take the next slot (round robin) for new work: a fresh id neither slot holds. Whatever the
    /// slot held before is replaced (its work is dropped by the caller).
    AsyncData Begin() {
        const std::uint32_t index = next;
        next = (next + 1) % Count;
        std::uint32_t id;
        do {
            id = static_cast<std::uint32_t>(distribution(random));
        } while (id == ids[0] || id == ids[1]);
        ids[index] = id;
        return {id, index};
    }

    /// The slot `data` names while it is live, or nothing (2142-0221).
    std::optional<std::size_t> Find(const AsyncData& data) const {
        if (data.index >= Count || data.id == 0 || ids[data.index] != data.id) {
            return std::nullopt;
        }
        return data.index;
    }

    /// Hand the slot back: its id names nothing any more.
    void Release(std::size_t index) {
        ids[index] = 0;
    }

private:
    std::array<std::uint32_t, Count> ids{};
    std::uint32_t next = 0;
    std::mt19937 random{std::random_device{}()};
    std::uniform_int_distribution<std::uint32_t> distribution{1, 0x7FFFFFFE};
};

} // namespace Service::PCTL

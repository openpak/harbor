// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <span>
#include <string_view>
#include <string>

#include "common/common_types.h"
#include "core/hle/result.h"

namespace Core {
class System;
}

// [OpenPak] Requests Harbor's HLE modules send to the firmware's own programs, as the firmware's
// am would: a plain cmif request from a dummy thread of the serving process, through a session
// opened on its service port. Blocking; call from a thread of your own, never from a service
// handler.
namespace OpenPak::GuestClient {

/// Buffers of a request: a pointer (X descriptor) input and a map-alias (A descriptor) input.
struct Buffers {
    std::string_view pointer_in;
    std::span<const u8> map_in;
};

/// One request with plain u64 arguments and optional buffers. The result is the server's.
Result Call(Core::System& system, const std::string& service, u32 command,
            std::span<const u64> args, const Buffers& buffers = {});

/// What ns tells the firmware's npns at start: the device's digital-twin records
/// (c-appearance.main, c-appearance.controller, c-storage.summary via npns:s 50). npns does not
/// open its push connection before it holds all three.
void PublishDigitalTwin(Core::System& system);

/// What the starter did at the console's initial setup: hand News the passphrase it derives its
/// content key from (news:a SetPassphrase for qlaunch, string read from the installed starter).
/// Without it the news module decodes no catalog and fetches no channel.
void SetNewsPassphrase(Core::System& system);

/// Once per install: follow OpenPak's news and games channels in the firmware's news module, as
/// the OpenPak NRO did on the console (news:m CreateNewsService, SetSubscriptionStatus 2).
void SubscribeDefaultNews(Core::System& system);

/// What am tells the firmware's prepo at start: the operation mode. Until it has one, prepo
/// refuses every report and qlaunch retries forever. Waits for prepo:m to exist.
void NotifyOperationMode(Core::System& system);

} // namespace OpenPak::GuestClient

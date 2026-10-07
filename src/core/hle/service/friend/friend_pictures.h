// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Friends' profile pictures as friend:v 10110 GetFriendProfileImage hands them to the overlay
// applet: baseline JPEG of at most 0x20000 bytes, fetched once per URL over OpenPak's pinned client
// (the thumbnail host is a Nintendo name the console server answers for). overlayDisp drops every
// pop-up whose picture it cannot decode, so anything that is not a baseline JPEG is re-encoded.
//
// The IPC handler only reads the cache; a miss starts a fetch for the next call. The overlay
// notifier fetches the picture before it sends the notification that will ask for it.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Service::Friend::Pictures {

/// The largest picture 10110 may answer with (overlayDisp's buffer).
inline constexpr std::size_t MaxSize = 0x20000;

/// The picture URL of a friend or a looked-up user, or empty.
std::string UrlOf(std::uint64_t network_service_account_id);

/// The cached picture for `url`, or nothing. Never waits.
std::optional<std::vector<std::uint8_t>> Cached(const std::string& url);

/// Fetch `url` into the cache if it is not there. Blocks for the round trip: call it only from a
/// background thread.
bool Fetch(const std::string& url);

/// Start a Fetch on the friends module's background worker and return at once.
void FetchInBackground(const std::string& url);

/// `image` as a baseline JPEG that fits MaxSize, or nothing when it cannot be decoded. A baseline
/// JPEG that already fits is returned as it is.
std::optional<std::vector<std::uint8_t>> NormalizeJpeg(const std::vector<std::uint8_t>& image);

} // namespace Service::Friend::Pictures

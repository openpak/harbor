// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// The platform titles: what a friend on Pier, Harbor, a PS3 ... is listed as playing.
//
// OpenPak's server names where somebody is by the id of a title of its own (01fe00f<generation><n>000) and
// serves that title's control NCA -- its name and picture -- from atum, as a real console fetches
// any title it does not have. Harbor's ns is emulated and never downloads, so it asks atum itself
// and keeps what it got. The names and pictures live on the server only: nothing here knows which
// platforms there are.

#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace OpenPak::PlatformTitles {

/// The cached control NCA of a title the server named, or empty when it has not been fetched.
std::vector<std::uint8_t> Cached(std::uint64_t application_id);

/// Fetch the title's control NCA from the server in the background if it is not cached, then call
/// `done` on that thread. Never blocks, never throws, and a failure is retried no sooner than five
/// minutes later.
void Prefetch(std::uint64_t application_id, std::function<void()> done = {});

} // namespace OpenPak::PlatformTitles

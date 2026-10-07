// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later
// Derived from openpak-client 3fd9ba7 (baas_detail.h).

// What the baas_*.cpp files share and nobody else needs.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "core/openpak/baas.h"

namespace OpenPak::Baas::detail {

inline constexpr const char* JsonPatch = "application/json-patch+json";
inline constexpr const char* JsonType = "application/json";
inline constexpr const char* FormType = "application/x-www-form-urlencoded";

/// One request through whatever transport is installed. Never throws: a request that cannot be
/// made comes back as status 0, which maps like any other unreachable server.
Reply Send(const std::string& method, const std::string& host, const std::string& path_and_query,
           const std::string& content_type, const std::string& body);

/// A body, written compactly and in insertion order, UTF-8 left as it is -- the relaxed escaping
/// the console writes with. Invalid UTF-8 becomes U+FFFD rather than an exception.
inline std::string Dump(const Json& value) {
    return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

/// Replace the cached block list with `blocks` (the local half of a block or unblock).
void ReplaceBlocks(std::vector<Block> blocks);

/// An id as the module writes one, %016llx.
inline std::string Hex16(std::uint64_t value) {
    return fmt::format("{:016x}", value);
}

} // namespace OpenPak::Baas::detail

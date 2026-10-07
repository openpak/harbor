// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later
// Derived from openpak-client 3fd9ba7 (session_detail.h).

// What session.cpp lends the push connection and nobody else needs.

#pragma once

#include <memory>
#include <string>

#include "common/httplib.h"

namespace OpenPak::Session::detail {

/// A client for one Nintendo hostname that reaches the OpenPak server, pinned to its CA and
/// presenting the device certificate, as every request of the chain is.
std::unique_ptr<httplib::SSLClient> MakeClientFor(const std::string& host);

/// The configured server as a file-name key ("host-port"), or empty when none is.
std::string ServerKey();

} // namespace OpenPak::Session::detail

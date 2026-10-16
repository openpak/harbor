// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <string>
#include <string_view>

// [OpenPak] OPENPAK_TRACE_GUEST_IPC=1: every request a firmware program sends to another guest
// program's service (what the HLE trace cannot see), by service name and command, with the
// result. sm notes the service name of each session it hands out; the svc hook reads the SFCI and
// SFCO headers from the caller's TLS.
namespace OpenPak::GuestIpcTrace {

bool Enabled();
void NoteSession(const void* client_session, std::string_view service_name);
std::string SessionName(const void* client_session);

} // namespace OpenPak::GuestIpcTrace

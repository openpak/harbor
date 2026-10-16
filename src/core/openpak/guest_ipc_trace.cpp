// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <cstdlib>
#include <map>
#include <mutex>

#include "core/openpak/guest_ipc_trace.h"

namespace OpenPak::GuestIpcTrace {

namespace {
std::mutex mutex;
std::map<const void*, std::string> names;
} // namespace

bool Enabled() {
    static const bool enabled = std::getenv("OPENPAK_TRACE_GUEST_IPC") != nullptr;
    return enabled;
}

void NoteSession(const void* client_session, std::string_view service_name) {
    if (!Enabled()) {
        return;
    }
    std::scoped_lock lk{mutex};
    names[client_session] = std::string{service_name};
}

std::string SessionName(const void* client_session) {
    std::scoped_lock lk{mutex};
    const auto it = names.find(client_session);
    return it == names.end() ? std::string{"?"} : it->second;
}

} // namespace OpenPak::GuestIpcTrace

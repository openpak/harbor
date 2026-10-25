// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// [Harbor] Destination rewrite for titles that dial a hard-coded IPv4 address, never a name, so DNS
// redirection cannot reach them (Brawlhalla's user server: inet_aton then connect). The list is part
// of the signed ceiling (docs/signed-ceiling.md, "ip_redirects"): only a listed address:port is
// rewritten, to the target the signed file names, and nothing else changes. The guest keeps seeing
// the address it dialled (getpeername, recvfrom), as it would on a console whose route was changed.
//
// bsd asks through this hook so the socket service does not depend on OpenPak's code; the OpenPak
// side installs it (core/openpak/service_hooks.cpp).

#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <utility>

#include "common/socket_types.h"

namespace Service::Sockets::IpRedirect {

// The replacement for a guest destination, or nothing when the destination is not listed (or
// OpenPak is off). `stream` is true for a TCP connect, false for UDP.
using Rewrite = std::function<std::optional<Network::SockAddrIn>(const Network::SockAddrIn& to,
                                                                  bool stream)>;

inline std::mutex& HookMutex() {
    static std::mutex mutex;
    return mutex;
}

inline Rewrite& HookStorage() {
    static Rewrite rewrite;
    return rewrite;
}

inline void SetHook(Rewrite rewrite) {
    std::scoped_lock lock{HookMutex()};
    HookStorage() = std::move(rewrite);
}

inline std::optional<Network::SockAddrIn> For(const Network::SockAddrIn& to, bool stream) {
    Rewrite rewrite;
    {
        std::scoped_lock lock{HookMutex()};
        rewrite = HookStorage();
    }
    if (!rewrite) {
        return std::nullopt;
    }
    return rewrite(to, stream);
}

inline bool SameEndpoint(const Network::SockAddrIn& a, const Network::SockAddrIn& b) {
    return a.ip == b.ip && a.portno == b.portno;
}

} // namespace Service::Sockets::IpRedirect

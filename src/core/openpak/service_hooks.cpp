// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include "core/openpak/boot_connection.h"
#include "core/openpak/network_profile.h"
#include "core/openpak/service_hooks.h"

#if __has_include("core/hle/service/sockets/sfdnsres_redirect.h")
#include "core/hle/service/sockets/sfdnsres_redirect.h"
#define HARBOR_HAVE_DNS_HOOKS 1
#endif
#if __has_include("core/hle/service/sockets/bsd_ip_redirect.h")
#include "core/hle/service/sockets/bsd_ip_redirect.h"
#define HARBOR_HAVE_IP_REDIRECT_HOOK 1
#endif
#if __has_include("core/hle/service/ssl/ssl_trust.h")
#include "core/hle/service/ssl/ssl_trust.h"
#define HARBOR_HAVE_TLS_HOOKS 1
#endif

namespace OpenPak {

void InstallServiceHooks() {
#ifdef HARBOR_HAVE_DNS_HOOKS
    namespace Dns = Service::Sockets::Dns;
    // Names arrive normalised (lower case, no trailing dot, after nsd). Every hook answers from
    // memory: they run on guest IPC threads.
    Dns::SetHooks(Dns::Hooks{
        .enabled = [] { return Network::Enabled(); },
        .route =
            [](const std::string& host) {
                if (NetworkProfile::IsNever(host)) {
                    return Dns::RouteAnswer{Dns::Route::PassThrough, {}};
                }
                if (auto address = Network::RedirectFor(host); address && !address->empty()) {
                    return Dns::RouteAnswer{Dns::Route::Redirect, std::move(*address)};
                }
                // A redirect with nowhere to go is no answer: the ceiling check fails it closed.
                return Dns::RouteAnswer{Dns::Route::NoMatch, {}};
            },
        .in_ceiling =
            [](const std::string& host) {
                return NetworkProfile::NameAllowed(host, Network::PlatformName);
            },
    });
#endif
#ifdef HARBOR_HAVE_IP_REDIRECT_HOOK
    // Hard-coded destinations the signed ceiling lists (its "ip_redirects"); nothing while OpenPak
    // is off. Runs on guest IPC threads for every addressed datagram: answers from memory.
    Service::Sockets::IpRedirect::SetHook(
        [](const ::Network::SockAddrIn& to, bool stream) -> std::optional<::Network::SockAddrIn> {
            if (!Network::Enabled()) {
                return std::nullopt;
            }
            const auto target =
                NetworkProfile::IpRedirectFor(to.ip, to.portno, stream, Network::PlatformName);
            if (!target) {
                return std::nullopt;
            }
            return ::Network::SockAddrIn{.family = to.family, .ip = target->first,
                                         .portno = target->second};
        });
#endif
#ifdef HARBOR_HAVE_TLS_HOOKS
    // Only a CA that matched the profile's pin at boot; none while OpenPak is off.
    Service::SSL::Trust::SetPinnedCaPath([]() -> std::filesystem::path {
        return Network::Enabled() ? Network::CaPath() : std::filesystem::path{};
    });
#endif
}

} // namespace OpenPak

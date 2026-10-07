// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

namespace OpenPak {

/// Points the guest services at this process's OpenPak state: the sfdnsres/nsd redirect hooks
/// (Service::Sockets::Dns::SetHooks) and the pinned CA for guest TLS
/// (Service::SSL::Trust::SetPinnedCaPath). The hooks read the live profile, ceiling and CA, so a
/// later refresh needs no second call. BootConnection::Run calls it; it only takes effect once the
/// net stream's headers are in the tree (until then it is a no-op).
void InstallServiceHooks();

} // namespace OpenPak

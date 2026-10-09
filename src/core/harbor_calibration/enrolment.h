// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// The console's enrolment with OpenPak's device CA (docs/harbor/calibration.md, "Certificates"):
// once per console, after its first OpenPak sign-in, the calibration's unsigned certificates go to
// the server with proof that this console holds their keys, and the signed ones come back into the
// calibration. The console then presents the CA-issued TLS certificate, and says it is alive every
// 30 s, so the server can keep one live console per device id. A 409 device_id_in_use (another
// console is live with this identity) makes the Harbor menu offer "Make this a new console".
//
// A server without the device CA answers 503: the console stays unsigned and works as before.

#pragma once

namespace Harbor::Enrolment {

/// Installs the session hooks below (InstallClientIdentity does).
void Install();

/// Enrol this console if it is not yet. Blocking (network and RSA signing): the heartbeat's
/// thread only.
void EnrolIfNeeded();

/// Tell the server this console is alive (an enrolled console only). Blocking.
void SayAlive();

/// End this console's session at the server, so the same console may start again at once.
void EndSession();

} // namespace Harbor::Enrolment

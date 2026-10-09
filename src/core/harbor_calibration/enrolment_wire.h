// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// The enrolment's wire format (nx-baas harbor_devices.go is the other side):
//
//   POST /harbor/v1/devices/challenge  ->  {"nonce": "<base64 of 32 bytes>", "expires_in": 300}
//   POST /harbor/v1/devices            <-  {"nonce", "device_id" (16 hex digits), "serial",
//                                           "ecc_certificate", "eticket_certificate",
//                                           "device_certificate", "tls_certificate",
//                                           "proofs": {"ecc", "eticket", "device", "tls"}}
//                                      ->  200 {the four certificates, signed} | 409
//                                          {"error":"device_id_in_use","action":"make_new_console"}
//
// Byte fields are standard base64. Every proof signs ProofMessage(). No network and no emulator
// code here, so the enrolment check (check/enrolment_check.cpp) builds it alone.

#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"
#include "core/harbor_calibration/cal0.h"

namespace Harbor::Enrolment {

struct Challenge {
    std::string text;     ///< as the server sent it, echoed back
    std::vector<u8> raw;  ///< what the proofs sign
};

/// The challenge out of the server's answer; nothing when it carries none.
std::optional<Challenge> ParseChallenge(std::string_view body);

/// What every key signs: "OpenPak Harbor device enrolment v1", a NUL, the nonce, and the device id
/// (8 bytes, big-endian).
std::vector<u8> ProofMessage(std::span<const u8> nonce, u64 device_id);

/// The enrolment request for a calibration binary, with its proofs over `challenge`.
std::string RequestBody(std::span<const u8> cal0, const Cal0::Proofs& proofs,
                        const Challenge& challenge);

/// The signed certificates out of a 200 answer; nothing when one is missing.
std::optional<Cal0::SignedCertificates> ParseSigned(std::string_view body);

/// The "error" of an error answer, or empty.
std::string ErrorCode(std::string_view body);

} // namespace Harbor::Enrolment

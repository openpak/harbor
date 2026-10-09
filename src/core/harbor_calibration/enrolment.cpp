// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <atomic>
#include <chrono>
#include <mutex>

#include "common/logging.h"
#include "core/harbor_calibration/calibration.h"
#include "core/harbor_calibration/enrolment.h"
#include "core/harbor_calibration/enrolment_wire.h"
#include "core/openpak/session.h"
#include "core/openpak/session_detail.h"

namespace Harbor::Enrolment {

namespace {

using Clock = std::chrono::steady_clock;

std::mutex attempt_mutex;
u64 attempted_for = 0;
Clock::time_point next_attempt{};

/// Whether to try now; the next try then waits ten minutes, unless RetryAfter says otherwise.
bool TakeAttempt(u64 device_id) {
    std::scoped_lock lock{attempt_mutex};
    const auto now = Clock::now();
    if (attempted_for == device_id && now < next_attempt) {
        return false;
    }
    attempted_for = device_id;
    next_attempt = now + std::chrono::minutes(10);
    return true;
}

void RetryAfter(std::chrono::minutes wait) {
    std::scoped_lock lock{attempt_mutex};
    next_attempt = Clock::now() + wait;
}

} // namespace

void Install() {
    OpenPak::Session::SetDeviceHooks({
        .signed_in = [] { EnrolIfNeeded(); },
        .beat =
            [] {
                EnrolIfNeeded();
                SayAlive();
            },
        .offline = [] { EndSession(); },
    });
}

void EnrolIfNeeded() {
    const auto& calibration = Calibration::Get();
    if (calibration.Binary().empty() || calibration.Keys() != Calibration::KeyState::Ready ||
        !OpenPak::Session::Enabled()) {
        return;
    }
    if (calibration.Enrolled()) {
        // Enrolled: this server's client certificate is the CA-issued one (once per process; a
        // replacement that failed last time is made here).
        static std::atomic<bool> presented{false};
        if (!presented.load() && OpenPak::Session::detail::ReplaceClientCertificate()) {
            presented = true;
        }
        return;
    }
    const u64 device_id = calibration.DeviceId();
    if (device_id == 0 || !TakeAttempt(device_id)) {
        return;
    }

    auto client = OpenPak::Session::detail::MakeClientFor(OpenPak::Session::detail::DeviceHost);
    const auto challenge_reply =
        client->Post("/harbor/v1/devices/challenge", "", "application/json");
    if (!challenge_reply) {
        LOG_WARNING(Service_SET, "calibration: enrolment: the server did not answer ({})",
                    httplib::to_string(challenge_reply.error()));
        return;
    }
    if (challenge_reply->status == 503 || challenge_reply->status == 404) {
        // No device CA there yet: the console stays unsigned and works as before.
        LOG_INFO(Service_SET, "calibration: enrolment: OpenPak's device CA is not available ({}); "
                              "the certificates stay unsigned for now",
                 challenge_reply->status);
        RetryAfter(std::chrono::minutes(6 * 60));
        return;
    }
    const auto challenge = challenge_reply->status == 200
                               ? ParseChallenge(challenge_reply->body)
                               : std::nullopt;
    if (!challenge) {
        LOG_WARNING(Service_SET, "calibration: enrolment: no challenge ({})",
                    challenge_reply->status);
        return;
    }

    const auto proofs = calibration.SignProofs(ProofMessage(challenge->raw, device_id));
    if (!proofs) {
        LOG_WARNING(Service_SET, "calibration: enrolment: the console's keys could not sign");
        return;
    }
    const auto reply = client->Post("/harbor/v1/devices",
                                    RequestBody(calibration.Binary(), *proofs, *challenge),
                                    "application/json");
    if (!reply) {
        LOG_WARNING(Service_SET, "calibration: enrolment: the server did not answer ({})",
                    httplib::to_string(reply.error()));
        return;
    }
    if (OpenPak::Session::NoteDeviceAnswer(reply->status, reply->body)) {
        return; // another console is live with this identity; the menu offers a new one
    }
    const auto certificates = reply->status == 200 ? ParseSigned(reply->body) : std::nullopt;
    if (!certificates) {
        LOG_WARNING(Service_SET, "calibration: enrolment refused ({} {})", reply->status,
                    ErrorCode(reply->body));
        return;
    }
    if (!ApplyEnrolment(*certificates)) {
        return;
    }
    // Every OpenPak request presents the CA-issued certificate from now on.
    if (!OpenPak::Session::detail::ReplaceClientCertificate()) {
        LOG_WARNING(Service_SET, "calibration: enrolment: the TLS client certificate could not "
                                 "be replaced; the next sign-in tries again");
    }
}

void SayAlive() {
    const auto& calibration = Calibration::Get();
    if (!calibration.Enrolled() || !OpenPak::Session::Enabled()) {
        return;
    }
    auto client = OpenPak::Session::detail::MakeClientFor(OpenPak::Session::detail::DeviceHost);
    if (const auto reply = client->Post("/harbor/v1/devices/live", "", "application/json")) {
        OpenPak::Session::NoteDeviceAnswer(reply->status, reply->body);
    }
}

void EndSession() {
    const auto& calibration = Calibration::Get();
    if (!calibration.Enrolled() || !OpenPak::Session::Enabled()) {
        return;
    }
    auto client = OpenPak::Session::detail::MakeClientFor(OpenPak::Session::detail::DeviceHost);
    client->set_connection_timeout(3, 0);
    client->set_read_timeout(3, 0);
    client->Post("/harbor/v1/devices/release", "", "application/json");
}

} // namespace Harbor::Enrolment

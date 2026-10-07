// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/logging.h"
#include "core/frontend/applets/general.h"
#include "core/hle/service/pctl/pctl_pairing.h"

namespace Core::Frontend {

ParentalControlsApplet::~ParentalControlsApplet() = default;

DefaultParentalControlsApplet::~DefaultParentalControlsApplet() = default;

void DefaultParentalControlsApplet::Close() const {}

// [Harbor] Eden's host applet accepted every PIN. Harbor has no host stand-in for the firmware's
// PIN entry: with no PIN set there is nothing to verify; with one set the host cannot answer for
// the user and refuses (the firmware's own Auth applet asks in a HOME session).
static bool NoPinSet() {
    return Service::PCTL::Pairing::Current().InForce().pin.empty();
}

void DefaultParentalControlsApplet::VerifyPIN(std::function<void(bool)> finished,
                                              [[maybe_unused]] bool suspend_future_verification_temporarily) {
    const bool verified = NoPinSet();
    LOG_INFO(Service_AM, "Host PIN check (normal): {}",
             verified ? "no PIN is set" : "a PIN is set, refused without the firmware applet");
    finished(verified);
}

void DefaultParentalControlsApplet::VerifyPINForSettings(std::function<void(bool)> finished) {
    const bool verified = NoPinSet();
    LOG_INFO(Service_AM, "Host PIN check (settings): {}",
             verified ? "no PIN is set" : "a PIN is set, refused without the firmware applet");
    finished(verified);
}

void DefaultParentalControlsApplet::RegisterPIN(std::function<void()> finished) {
    LOG_WARNING(Service_AM, "Host PIN registration is not offered; no PIN was registered");
    finished();
}

void DefaultParentalControlsApplet::ChangePIN(std::function<void()> finished) {
    LOG_WARNING(Service_AM, "Host PIN change is not offered; the PIN is unchanged");
    finished();
}

PhotoViewerApplet::~PhotoViewerApplet() = default;

DefaultPhotoViewerApplet::~DefaultPhotoViewerApplet() = default;

void DefaultPhotoViewerApplet::Close() const {}

void DefaultPhotoViewerApplet::ShowPhotosForApplication(u64 title_id,
                                                        std::function<void()> finished) const {
    LOG_INFO(Service_AM,
             "Application requested frontend to display stored photos for title_id={:016X}",
             title_id);
    finished();
}

void DefaultPhotoViewerApplet::ShowAllPhotos(std::function<void()> finished) const {
    LOG_INFO(Service_AM, "Application requested frontend to display all stored photos.");
    finished();
}

} // namespace Core::Frontend

// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <chrono>
#include <exception>

#include <borealis.hpp>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "frontend_common/firmware_manager.h"
#include "harbor_setup/harbor_setup.h"
#include "harbor_setup/host.h"
#include "harbor_setup/keys.h"
#include "harbor_setup/view.h"
#include "harbor_setup/wizard.h"

// The text font (Noto Sans, OFL-1.1), embedded by CMake.
extern const unsigned char harbor_setup_font[];
extern const std::size_t harbor_setup_font_size;

namespace HarborSetup {

namespace {
Wizard* active_wizard = nullptr; // only touched on the wizard's own thread
} // namespace

void RequestBack() {
    brls::sync([] {
        if (active_wizard) {
            active_wizard->Back();
        }
    });
}

Readiness CheckReadiness() {
    Readiness readiness;
    readiness.keys_usable = KeysUsable();
    if (readiness.keys_usable) {
        const auto firmware = FirmwareManager::InspectInstalledFirmware();
        readiness.firmware_present = firmware.present;
        readiness.qlaunch = firmware.has_qlaunch;
        readiness.firmware_version = firmware.version;
        if (firmware.present && firmware.version.empty()) {
            readiness.firmware_problem =
                "The installed firmware cannot be read with these keys. Install it again.";
        } else if (firmware.present && !firmware.has_qlaunch) {
            readiness.firmware_problem = "The installed firmware " + firmware.version +
                                         " has no HOME menu (qlaunch). Install firmware " +
                                         std::string(FirmwareManager::HarborFirmwareVersion) + ".";
        }
    }
    readiness.record_completed = LoadRecord(RecordPath()).completed;
    return readiness;
}

int RunWizard(Host& host) {
    static bool ran = false;
    if (ran) {
        // Borealis keeps static state that cannot be rebuilt in the same process.
        LOG_ERROR(Frontend, "harbor: the setup can run once per process");
        return 1;
    }
    ran = true;

    const auto started = std::chrono::steady_clock::now();
    const auto readiness = CheckReadiness();
    const auto device = host.Probe();
    for (const auto& row : RunDeviceCheck(device)) {
        LOG_INFO(Frontend, "harbor: check {}: {} ({})", row.title,
                 row.level == CheckLevel::Pass   ? "pass"
                 : row.level == CheckLevel::Warn ? "warn"
                                                 : "FAIL",
                 row.value);
    }
    LOG_INFO(Frontend, "harbor: readiness keys={} firmware={} qlaunch={} record_completed={}",
             readiness.keys_usable, readiness.firmware_version.empty() ? "none" : readiness.firmware_version,
             readiness.qlaunch, readiness.record_completed);

    // The setup is English only; a fixed locale keeps Borealis from looking for other string tables.
    brls::Platform::APP_LOCALE_DEFAULT = brls::LOCALE_EN_US;

    int exit_code = 1;
    try {
#ifdef __ANDROID__
        VideoContext::FULLSCREEN = true;
#endif
        if (!brls::Application::init()) {
            LOG_ERROR(Frontend, "harbor: Borealis did not start");
            return 1;
        }
        brls::Application::createWindow("Harbor setup");
        brls::Application::loadFontFromMemory(brls::FONT_REGULAR,
                                              const_cast<unsigned char*>(harbor_setup_font),
                                              harbor_setup_font_size, false);

        // Buttons by position, as on a Switch: right A, bottom B, top X, left Y. Borealis reads SDL's
        // positional gamepad buttons as Xbox semantics (bottom = A), so its swap gives the Switch's.
        brls::Application::setSwapInputKeys(true);
        brls::Application::setGlobalQuit(false);

        // Focus: a static 3-unit mint ring over the lit row colour of each theme.
        brls::getStyle().addMetric("brls/highlight/stroke_width", 3);
        for (const bool dark : {false, true}) {
            brls::Theme theme = dark ? brls::Theme::getDarkTheme() : brls::Theme::getLightTheme();
            theme.addColor("brls/highlight/color1", nvgRGB(0x39, 0xef, 0xd2));
            theme.addColor("brls/highlight/color2", nvgRGB(0x39, 0xdf, 0xe4));
            theme.addColor("brls/highlight/background",
                           dark ? nvgRGB(0x20, 0x20, 0x20) : nvgRGB(0xff, 0xff, 0xff));
            theme.addColor("brls/click_pulse", nvgRGBA(0x39, 0xef, 0xd2, 38));
        }
        SetDarkTheme(SystemDarkTheme());

        FitCanvas();
        brls::Application::getWindowSizeChangedEvent()->subscribe([] { FitCanvas(); });

        Wizard wizard(host, RecordPath(), readiness, device,
                      [](std::function<void()> work) { brls::sync(work); });
        active_wizard = &wizard;
        wizard.finished = [&exit_code] {
            exit_code = 0;
            brls::Application::quit();
        };
        brls::Application::pushActivity(new WizardActivity(wizard), brls::TransitionAnimation::NONE);

        bool first = true;
        while (brls::Application::mainLoop()) {
            if (first) {
                first = false;
                LOG_INFO(Frontend, "harbor: setup window up after {} ms",
                         std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count());
            }
        }
        active_wizard = nullptr;
    } catch (const std::exception& e) {
        active_wizard = nullptr;
        LOG_ERROR(Frontend, "harbor: setup stopped: {}", e.what());
        return 1;
    }
    LOG_INFO(Frontend, "harbor: setup closed, {}", exit_code == 0 ? "completed" : "not completed");
    return exit_code;
}

} // namespace HarborSetup

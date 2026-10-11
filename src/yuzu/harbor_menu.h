// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "harbor_setup/menu.h"

class QOffscreenSurface;
class QWidget;

namespace InputCommon {
class InputSubsystem;
}

/// The desktop's side of the Harbor menu (harbor_setup/menu.h): the menu draws onto the console's
/// screen; this opens it (F1, Plus and Minus together) and carries out what it asks for.
class HarborMenu final : public HarborSetup::MenuHost {
public:
    /// What only the main window can do. Each runs on the GUI thread.
    struct Hooks {
        std::function<void()> restart;            ///< stop the console and boot HOME again
        std::function<void()> exit;               ///< close Harbor
        std::function<void()> save_config;        ///< write qt-config.ini
        std::function<void(bool)> joycons;        ///< detach (true) or attach the Joy-Cons
        std::function<void(std::function<void()>)> change_dll; ///< a Lossless.dll change, console stopped
        std::function<void(const std::string&)> console_action; ///< backup, restore, new-console
    };

    HarborMenu(QWidget* window, InputCommon::InputSubsystem& input, Hooks hooks);
    ~HarborMenu() override;

    /// F1 and the harness: opens the menu, or closes it when it is open.
    void Toggle();

    bool IsAndroid() const override {
        return false;
    }
    std::unique_ptr<HarborSetup::MenuGl> CreateMenuGl() override;
    bool HasHud() const override {
        return true;
    }
    std::optional<std::string> HudText() override;
    void HostNotice(const std::string& text) override;
    /// The latest performance figures (MainWindow's statsUpdated, twice a second).
    void SetStats(double fps, double speed);
    Core::System& System() override;
    InputCommon::InputSubsystem& Input() override;
    bool PlatformDarkTheme() override;
    void SaveConfig() override;

    bool JoyconsDetached() override;
    void SetJoyconsDetached(bool detached) override;
    void Restart() override;
    void SetupAgain() override;
    void Exit() override;

    bool ShowFps() override;
    void SetShowFps(bool show) override;
    bool HasDeviceLine() override {
        return true;
    }
    bool ShowDeviceLine() override;
    void SetShowDeviceLine(bool show) override;
    bool FrameGenSupported() override;
    void ChooseLosslessDll() override;
    void RemoveLosslessDll() override;

    // Graphics drivers: only with the harness's test store (harbor_test_drivers.h).
    bool HasDriverChoice() override;
    std::vector<HarborSetup::MenuChoice> Drivers() override;
    std::string CurrentDriver() override;
    void ChooseDriver(const std::string& id) override;
    HarborSetup::Host* DriverHost() override;
    HarborSetup::DeviceInfo DriverDevice() override;

    std::optional<HarborSetup::KeyboardDefaults> DefaultKeyboard() override;
    std::string KeyName(int code) override;
    void ConsoleAction(const std::string& name) override;
    /// "Save a copy…": the report copied where the user picks.
    void ShareCrashReport(const Common::HarborCrash::Report& report) override;

private:
    void OnGui(std::function<void()> work);
    void WatchChord();

    QWidget* window;
    InputCommon::InputSubsystem& input;
    /// The menu's offscreen surface: made here on the GUI thread (some platforms back it with a
    /// hidden window), drawn to from the menu's thread with a context made there.
    std::unique_ptr<QOffscreenSurface> gl_surface;
    bool threaded_gl = true;
    Hooks hooks;
    // Harbor Settings' Show FPS and device line, kept in harbor.ini (off by default).
    std::atomic_bool show_fps{false};
    std::atomic_bool show_device_line{false};
    std::atomic<double> fps{0.0};
    std::atomic<double> speed{0.0};
    // HostNotice: shown under the FPS line (or alone) until the deadline.
    std::mutex notice_mutex;
    std::string notice;
    std::chrono::steady_clock::time_point notice_until{};
    int player1_callback = -1;
    int handheld_callback = -1;
};

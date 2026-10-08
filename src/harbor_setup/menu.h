// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// The Harbor menu and Harbor Settings: one Borealis implementation for every frontend. The menu
// takes over the console's own screen: Borealis draws offscreen at the console's resolution and the
// presenter shows that picture in place of the guest's (video_core/host_screen.h), with the same
// scaling and letterboxing. The console keeps running behind it and gets no input: its controllers
// are in Eden's configuration mode and the touch screen belongs to the menu while it is open.

#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "common/settings_input.h"

namespace Core {
class System;
}

namespace InputCommon {
class InputSubsystem;
}

namespace HarborSetup {

/// One entry of a list the frontend fills (graphics drivers).
struct MenuChoice {
    std::string id;
    std::string label;
    std::string detail;
};

/// Keyboard mapping defaults (desktop): Eden's own, as Qt key codes.
struct KeyboardDefaults {
    std::array<int, Settings::NativeButton::NumButtons> buttons{};
    std::array<std::array<int, 4>, Settings::NativeAnalog::NumAnalogs> analogs{}; ///< up, down, left, right
    std::array<int, Settings::NativeAnalog::NumAnalogs> modifiers{};
};

/// What the menu needs from the frontend. The menu calls these on its own thread; anything that
/// touches the frontend's UI is moved to the UI thread by the implementation.
class MenuHost {
public:
    virtual ~MenuHost() = default;

    virtual bool IsAndroid() const = 0;
    virtual Core::System& System() = 0;
    virtual InputCommon::InputSubsystem& Input() = 0;

    /// The frontend's own light or dark mode, for before the console has a colour set.
    virtual bool PlatformDarkTheme() {
        return false;
    }
    /// Writes the configuration (input mappings, frame generation) where the frontend keeps it.
    virtual void SaveConfig() = 0;

    // The console.
    virtual bool JoyconsDetached() = 0;
    virtual void SetJoyconsDetached(bool detached) = 0;
    virtual void Restart() = 0;
    virtual void SetupAgain() = 0;
    virtual void Exit() = 0;

    // Display & Performance.
    virtual bool ShowFps() = 0;
    virtual void SetShowFps(bool show) = 0;
    /// The device and driver line under the FPS (Android).
    virtual bool HasDeviceLine() {
        return false;
    }
    virtual bool ShowDeviceLine() {
        return false;
    }
    virtual void SetShowDeviceLine(bool show) {}
    /// Whether this GPU can run frame generation at all.
    virtual bool FrameGenSupported() = 0;
    /// The user's Lossless.dll: picked, checked and installed (the console restarts).
    virtual void ChooseLosslessDll() = 0;
    virtual void RemoveLosslessDll() = 0;
    /// Graphics drivers (Android with an Adreno GPU).
    virtual bool HasDriverChoice() {
        return false;
    }
    virtual std::vector<MenuChoice> Drivers() {
        return {};
    }
    virtual std::string CurrentDriver() {
        return {};
    }
    /// Chooses the driver HOME and every title run with (the process restarts).
    virtual void ChooseDriver(const std::string& id) {}

    // Controllers and Input.
    /// Android's accessibility HOME key for controllers without one: shown while it is off.
    virtual bool HasControllerHomeKey() {
        return false;
    }
    virtual bool ControllerHomeKeyEnabled() {
        return true;
    }
    virtual void OpenControllerHomeKeySettings() {}
    /// Desktop: the keyboard's default mapping and key names; and the keys that drive the menu.
    virtual std::optional<KeyboardDefaults> DefaultKeyboard() {
        return std::nullopt;
    }
    virtual std::string KeyName(int code) {
        return "Key " + std::to_string(code);
    }

    // Touch controls (Android).
    virtual bool HasTouchControls() {
        return false;
    }
    virtual bool TouchControlsHidden() {
        return false;
    }
    virtual void SetTouchControlsHidden(bool hidden) {}
    virtual bool PowerControlShown() {
        return true;
    }
    virtual void SetPowerControlShown(bool shown) {}
    virtual int TouchOpacity() {
        return 100;
    }
    virtual void SetTouchOpacity(int percent) {}
    virtual int TouchScale() {
        return 50;
    }
    virtual void SetTouchScale(int percent) {}
    /// Leaves the menu and starts moving the touch controls on the screen.
    virtual void EditTouchLayout() {}
    virtual void ResetTouchLayout() {}

    // Harbor.
    /// Consent to send crash reports, once crash reporting exists (nullopt: not yet).
    virtual std::optional<bool> CrashReportConsent() {
        return std::nullopt;
    }
    virtual void SetCrashReportConsent(bool consent) {}
};

/// Where the menu opens.
enum class MenuPage {
    Menu,
    Settings,
    FrameGeneration,
    Controllers,
};

/// Once per process, before the first Open. The host outlives the menu.
void MenuSetHost(MenuHost& host);

/// Opens the menu (from Plus+Minus, Android back, the on-screen button or the desktop key). The
/// console's picture is replaced from the next frame. Safe from any thread.
void MenuOpen(MenuPage page = MenuPage::Menu);
/// Closes it: the console's picture comes back and its controllers get their input again.
void MenuClose();
bool MenuIsOpen();
/// Android back while the menu is open: the menu's own Back (on the top level it closes).
void MenuBack();
/// At shutdown: the menu's thread ends (its GL context and Borealis go with the process).
void MenuShutdown();

} // namespace HarborSetup

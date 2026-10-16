// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// The Harbor menu and Harbor Settings: one Borealis implementation for every frontend. The menu
// takes over the console's own screen: Borealis draws offscreen at the console's resolution and the
// presenter shows that picture in place of the guest's (video_core/host_screen.h), with the same
// scaling and letterboxing. The console keeps running behind it and gets no input: its controllers
// are in Eden's configuration mode and the touch screen belongs to the menu while it is open.

#pragma once

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/harbor_crash.h"
#include "common/param_package.h"
#include "common/settings_input.h"
#include "harbor_setup/device_check.h"

namespace Core {
class System;
}

namespace VideoCore::HostScreen {
struct Frame;
}

namespace InputCommon {
class InputSubsystem;
}

namespace HarborSetup {

class Host;

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

/// The desktop menu's offscreen OpenGL 3.3 core context, made by the frontend's toolkit so that it
/// works on every window system (WGL, CGL, GLX, EGL). Made, used and destroyed on the menu's thread.
class MenuGl {
public:
    virtual ~MenuGl() = default;
    virtual void* ProcAddress(const char* name) = 0;
};

/// What the menu needs from the frontend. The menu calls these on its own thread; anything that
/// touches the frontend's UI is moved to the UI thread by the implementation.
class MenuHost {
public:
    virtual ~MenuHost() = default;

    virtual bool IsAndroid() const = 0;
    /// Desktop: the menu's GL context, current on the calling thread (the menu's), or null when
    /// there is none. Android makes its own with EGL and does not use this.
    virtual std::unique_ptr<MenuGl> CreateMenuGl() {
        return nullptr;
    }
    virtual Core::System& System() = 0;
    virtual InputCommon::InputSubsystem& Input() = 0;
    /// The input devices InputCommon lists (Android: a copy taken on the UI thread, where its
    /// device list changes).
    virtual std::vector<Common::ParamPackage> InputDevices();
    /// Runs one of the menu thread's frames (Android: inside a JNI local frame).
    virtual void MenuThreadFrame(const std::function<void()>& frame) {
        frame();
    }

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

    /// The FPS line the menu draws on the console's screen (desktop; Android draws its own):
    /// its text (one or two lines), or nullopt while it is off. Asked twice a second.
    virtual bool HasHud() const {
        return false;
    }
    virtual std::optional<std::string> HudText() {
        return std::nullopt;
    }

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
    /// Desktop: the render scale follows the window at each console start (Automatic), unless a
    /// fixed one is chosen.
    virtual bool HasAutoResolution() {
        return false;
    }
    virtual bool AutoResolution() {
        return false;
    }
    virtual void SetAutoResolution(bool automatic) {}
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
    /// The setup's driver store and network (host.h), through which the menu downloads drivers
    /// from the setup's GitHub catalogue (driver_recovery.h); null where there is none. Its calls
    /// are made from a worker thread of the menu's.
    virtual Host* DriverHost() {
        return nullptr;
    }
    /// The device the catalogue is filtered for (its GPU, Android version and ABIs).
    virtual DeviceInfo DriverDevice() {
        return {};
    }

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
    /// The console's backup, restore and new identity ("backup", "restore", "new-console"): the
    /// console stops, the platform asks for the file and the passphrase, the console starts again.
    virtual void ConsoleAction(const std::string& name) {}
    /// A short host-side notice (Harbor's own, never inside the console's UI), from any thread:
    /// a toast on Android, a line on the console's screen for a few seconds on desktop.
    virtual void HostNotice(const std::string& text) {}
    /// Consent to send crash reports (common/harbor_crash.h; nullopt: not asked yet).
    virtual std::optional<bool> CrashReportConsent() {
        return Common::HarborCrash::CrashReportConsent();
    }
    virtual void SetCrashReportConsent(bool consent) {
        Common::HarborCrash::SetCrashReportConsent(consent);
    }
    /// The user's own way to pass a crash report on: Android's share sheet, a saved copy on the
    /// desktop. Harbor itself never sends one.
    virtual std::string ShareCrashReportLabel() {
        return IsAndroid() ? "Share" : "Save a copy…";
    }
    virtual void ShareCrashReport(const Common::HarborCrash::Report& report) {}

    // Harbor's own pages outside the console (MenuShowHostPage).
    /// What a host page asks of the frontend: "start-again", "set-up-again", "exit", "back" (B on
    /// the page itself), "driver-changed" (a driver was chosen or downloaded for the next start),
    /// "restart-with" (the driver failed to load: `argument` is chosen, "" the phone's own, and
    /// Harbor restarts into it).
    virtual void HostPageAction(const std::string& action, const std::string& argument) {}
    /// After a driver failed to load: the installed drivers that fit this GPU, except the chosen one.
    virtual std::vector<MenuChoice> FittingDrivers() {
        return {};
    }
};

/// Where the menu opens.
enum class MenuPage {
    Menu,
    Settings,
    FrameGeneration,
    Controllers,
    /// Harbor › This console, opened when OpenPak refuses this console's identity because another
    /// console is live with it (a copied NAND): it offers "Make this a new console".
    Console,
    /// Display & Performance › Graphics driver (Adreno): installed drivers and downloads.
    Drivers,
    /// Crash reports: asked once whether to keep them, or last time's report offered (view,
    /// share, delete). MenuOfferCrashReport opens it.
    CrashReport,
    /// A status line on the console's screen ("Stopping the console..."): MenuShowStatus.
    Status,
};

/// Harbor's own pages when no console picture is there to draw over: the launcher's status line,
/// "Harbor stopped" (with its reason, Start again, Graphics driver on Adreno, Set up again, Exit),
/// "Harbor cannot start" and the driver that did not load. The menu draws them into the frontend's
/// own surface instead of the console's screen.
///
/// Dialog is a system dialog (the console's confirmations and errors) stood on its own: `title`,
/// `text` and its `buttons` along the bottom, the first one being B's (Cancel, Close) and the last
/// one in the accent colour. What was chosen goes to `answered` (the menu's thread) once, as the
/// button's index. While a dialog is up the console's controllers belong to it.
enum class HostPageKind { Status, Stopped, CannotStart, DriverFailed, Dialog };

struct HostPage {
    HostPageKind kind = HostPageKind::Status;
    std::string text; ///< the status line, the stop reason or the driver problem; a dialog's body
    // Dialog only.
    std::string title;
    std::vector<std::string> buttons;
    int default_button = 0; ///< the button that has the focus first
    std::function<void(int)> answered;
};

/// Receives each drawn picture of a host page (on the menu's thread).
using HostFrameSink = std::function<void(std::shared_ptr<const VideoCore::HostScreen::Frame>)>;

/// The buttons a frontend's surface passes on, by the console's positions (A confirms, B goes back).
enum class HostButton { A, B, X, Y, Up, Down, Left, Right };

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

/// Once per process, when the console starts (after MenuSetHost): when the crash-report question
/// has not been answered, or a crash left a report not offered yet, the CrashReport page opens as
/// soon as the console's picture is on the screen (never earlier: a page that is not seen must not
/// take the console's input), and while the menu is closed.
void MenuOfferCrashReport();

/// A status line on the console's screen while the frontend stops it ("Stopping the console..."),
/// with nothing to choose; the console's input stays with the menu until it closes.
void MenuShowStatus(std::string text);

/// Shows a host page in the frontend's surface (any thread): pictures go to `sink`, input comes
/// from MenuHostButton and MenuHostTouch (and the console's controllers, for the harness). A page
/// shown while another is up replaces it. Needs MenuSetHost.
void MenuShowHostPage(HostPage page, HostFrameSink sink);
/// The surface goes away: the page closes and no picture is sent any more.
void MenuEndHostPage();
bool MenuHostPageShown();
/// Whether a frontend host is set, that is whether host pages can be drawn at all.
bool MenuHostReady();
void MenuHostButton(HostButton button, bool pressed);
/// A finger (or the mouse) on the surface: `x` and `y` from 0 to 1 across the picture.
void MenuHostTouch(int id, float x, float y, bool down);
/// A status line for the open host page from the frontend (a driver file it installed).
void MenuHostPageNotice(std::string text, bool bad);

} // namespace HarborSetup

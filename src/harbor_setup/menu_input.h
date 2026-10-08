// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Controllers and Input: the mappings behind the menu's page. Everything is Eden's own input
// bindings (common/settings_input.h parameter strings, InputCommon mapping and default mappings,
// EmulatedController params), the same ones Eden's controller configuration writes. Harbor adds a
// per-controller store, so a pad that comes back gets its mapping back, and the Power button,
// which the console has and Eden's controllers do not.

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "common/input.h"
#include "common/param_package.h"
#include "common/settings_input.h"

namespace Core {
class System;
}

namespace InputCommon {
class InputSubsystem;
}

namespace HarborSetup {

class MenuHost;

/// The buttons the page lists, in the console's order.
struct MappableButton {
    int index; ///< Settings::NativeButton
    const char* name;
};
extern const std::array<MappableButton, 18> MappableButtons;

/// A controller (or the keyboard) as InputCommon lists it.
struct InputDevice {
    Common::ParamPackage params;
    std::string name;
};

/// How a stick's direction is inverted.
enum class StickInvert { None, X, Y, Both };

/// The store of per-controller mappings and the Power button binding: harbor-input.json in the
/// config directory. Pure data, readable without a running console (the self-check uses it).
class InputStore {
public:
    explicit InputStore(std::filesystem::path path);

    /// The key a controller's mappings are kept under: engine and GUID (or name).
    static std::string DeviceKey(const Common::ParamPackage& device);

    struct Profile {
        std::array<std::string, Settings::NativeButton::NumButtons> buttons;
        std::array<std::string, Settings::NativeAnalog::NumAnalogs> analogs;
    };
    std::optional<Profile> Find(const std::string& key) const;
    void Put(const std::string& key, const Profile& profile);

    std::string Power() const;
    void SetPower(const std::string& param);

    bool Save() const;

private:
    void Load();

    mutable std::mutex mutex; // the menu and the controller watcher both use the store
    std::filesystem::path path;
    std::string power;
    std::vector<std::pair<std::string, Profile>> profiles;
};

/// A profile's parameters moved onto `device` (its current port and GUID): what was saved for a pad
/// works when it comes back on another port.
std::string RetargetParam(const std::string& param, const Common::ParamPackage& device);

/// The Power button: a binding from any input device. A short press puts the console to sleep (or
/// wakes it), held for a second it opens HOME's power menu, as on the console.
class PowerKey {
public:
    explicit PowerKey(Core::System& system);
    ~PowerKey();

    void Bind(const std::string& param);

private:
    void OnChange(bool pressed);
    void Run();

    Core::System& system;
    std::unique_ptr<Common::Input::InputDevice> device;
    std::mutex mutex;
    std::condition_variable cv;
    bool pressed = false;
    bool held_fired = false;
    bool stopping = false;
    std::uint64_t press_serial = 0;
    std::thread worker;
};

/// The mapping model the page shows. Player 0 is Player 1, whose mappings also go to Handheld:
/// Harbor keeps the two identical, so pads work whether the Joy-Cons are attached or not.
class InputMapping {
public:
    static constexpr int Players = 8;

    InputMapping(MenuHost& host, InputStore& store);

    std::vector<InputDevice> Devices() const;

    bool Connected(int player) const;
    void SetConnected(int player, bool connected);

    /// The device the player's mapping uses, as a name ("Keyboard", a pad's name, or "Not set").
    std::string DeviceName(int player) const;
    /// Maps the player to `device`: its saved mapping when it has one, else its defaults.
    void UseDevice(int player, const InputDevice& device);
    /// The defaults of the device the player uses now.
    void RestoreDefaults(int player);

    std::string ButtonText(int player, int button) const;
    std::string StickText(int player, int stick) const;
    void SetButton(int player, int button, const Common::ParamPackage& param);
    void SetStick(int player, int stick, const Common::ParamPackage& param);
    void ClearButton(int player, int button);

    /// Dead zone in percent; only for analog sticks (not keys).
    std::optional<int> DeadZone(int player, int stick) const;
    void SetDeadZone(int player, int stick, int percent);
    std::optional<StickInvert> Invert(int player, int stick) const;
    void SetInvert(int player, int stick, StickInvert invert);

    std::string PowerText() const;
    void SetPower(const Common::ParamPackage& param);
    void ClearPower();

    /// What a binding reads as on the page.
    std::string Describe(const Common::ParamPackage& param) const;

    /// Saves the player's mapping to the configuration and to its controller's store entry.
    void Commit(int player);

    /// The device the player's mapping uses; `connected` is false for a pad that is not there now.
    std::optional<InputDevice> CurrentDevice(int player, bool* connected = nullptr) const;
    /// A known controller on `player`: its saved mapping (moved to its port) when the mapping
    /// differs from it. Whether anything changed.
    bool ApplySaved(int player);

private:
    Common::ParamPackage ButtonParam(int player, int button) const;
    Common::ParamPackage StickParam(int player, int stick) const;
    void Apply(int player, const InputStore::Profile& profile);
    std::optional<InputStore::Profile> Defaults(const InputDevice& device) const;

    MenuHost& host;
    InputStore& store;
};

/// Press-to-assign through InputCommon's own mapping (the calls Eden's controller dialog makes):
/// the next button, axis or hat of any device. Keys need the keyboard's port 0 (the render window).
class InputCapture {
public:
    enum class Kind { Button, Stick };

    explicit InputCapture(InputCommon::InputSubsystem& input);
    ~InputCapture();

    void Begin(Kind kind);
    /// The input once one came. A stick asked for with keys or buttons returns after four presses
    /// (up, down, left, right) as Eden's analog-from-buttons.
    std::optional<Common::ParamPackage> Poll();
    /// Which direction a stick from buttons waits for (0 up .. 3 right), or -1.
    int StickStep() const {
        return stick_step;
    }
    void End();
    bool Active() const {
        return active;
    }

private:
    InputCommon::InputSubsystem& input;
    Kind kind = Kind::Button;
    bool active = false;
    int stick_step = -1;
    std::array<std::string, 4> directions;
};

/// The Harbor input store of this data directory.
InputStore& HarborInputStore();

/// Binds the Power button from the store for this console session (call when a console boots).
void StartPowerKey(Core::System& system);

/// As a console remembers pads: whenever a controller connects or comes back, on whichever player
/// it lands (a pad the frontend binds, or one a player was already mapped to), its saved mapping
/// is applied. Checks once a second; stops with StopControllerWatcher.
void StartControllerWatcher(MenuHost& host);
void StopControllerWatcher();

} // namespace HarborSetup

// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <functional>
#include <memory>

#include "harbor_setup/menu.h"

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
        std::function<bool()> fps_shown;          ///< the performance overlay is on
        std::function<void(bool)> show_fps;       ///< turn it on or off
        std::function<void(bool)> joycons;        ///< detach (true) or attach the Joy-Cons
        std::function<void(std::function<void()>)> change_dll; ///< a Lossless.dll change, console stopped
    };

    HarborMenu(QWidget* window, InputCommon::InputSubsystem& input, Hooks hooks);
    ~HarborMenu() override;

    /// F1 and the harness: opens the menu, or closes it when it is open.
    void Toggle();

    bool IsAndroid() const override {
        return false;
    }
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
    bool FrameGenSupported() override;
    void ChooseLosslessDll() override;
    void RemoveLosslessDll() override;

    std::optional<HarborSetup::KeyboardDefaults> DefaultKeyboard() override;
    std::string KeyName(int code) override;

private:
    void OnGui(std::function<void()> work);
    void WatchChord();

    QWidget* window;
    InputCommon::InputSubsystem& input;
    Hooks hooks;
    int player1_callback = -1;
    int handheld_callback = -1;
};

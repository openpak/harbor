// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

class QObject;
class QWidget;

namespace InputCommon {
class InputSubsystem;
}

/**
 * Test harness control channel. When the environment variable HARBOR_CONTROL names a path, a FIFO
 * is created there and line commands are read from it on a background thread:
 *
 *   press <button> [ms]   hold <button>   release <button>   stick <l|r> <x> <y> [ms]
 *   home   home-long   capture   power   power-tap   terminate
 *   screenshot <png path>   threads   quit   menu <name>
 *   type <text>   type-line <file> <n>   key <name>   detach   attach   dock
 *
 * Buttons reach the guest through the virtual gamepad (the same input path the Android overlay
 * uses), on both the Player 1 and the Handheld controller. Text and keys reach it as key presses
 * on the emulated USB keyboard (US layout), which the firmware's own software keyboard reads as a
 * real console reads a USB keyboard; while Eden's Qt keyboard dialog is open (swkbd HLE, outside a
 * HOME session) they go to that dialog instead. Every command is logged with the prefix
 * "harbor-control:", except that type-line logs only its length: it is for secrets (a test
 * account's password), read from line n of a file, the value alone when the line is key=value.
 */
class HarborControl {
public:
    struct Hooks {
        /// Runs on the GUI thread. Calls `done(success)` once the file is written (or failed).
        std::function<void(const std::string& path, std::function<void(bool)> done)> screenshot;
        /// Runs on the GUI thread.
        std::function<void()> quit;
        /// Runs on the control thread.
        std::function<void()> threads;
        /// Runs on the GUI thread: Eden's Qt software keyboard dialog, or another modal host
        /// dialog (the Harbor menu's), when one is showing, else null.
        std::function<QWidget*()> keyboard_dialog;
        /// Runs on the GUI thread: opens a Harbor menu entry by name ("frame-gen").
        std::function<void(const std::string& name)> menu;
        /// Runs on the GUI thread: the Joy-Cons off (true) or back on (false) the console's rails.
        std::function<void(bool detached)> joycons;
        /// Runs on the control thread: the power button held (F5), force-terminate (F10).
        std::function<void()> power;
        std::function<void()> terminate;
        /// Runs on the GUI thread: into or out of the dock (F6).
        std::function<void()> dock;
    };

    /// Returns nullptr unless HARBOR_CONTROL is set.
    static std::unique_ptr<HarborControl> CreateFromEnvironment(
        QObject* gui_context, InputCommon::InputSubsystem& input_subsystem, Hooks hooks);

    ~HarborControl();

private:
    HarborControl(std::string fifo_path, QObject* gui_context,
                  InputCommon::InputSubsystem& input_subsystem, Hooks hooks);

    void ReadLoop();
    void Execute(const std::string& line);
    void SetButton(int button, bool pressed);
    void PressFor(int button, int ms);
    /// Types `text`; never logs it. False (nothing typed) when a character has no key.
    bool TypeText(const std::string& text);
    /// One named key (Enter, Backspace, Tab, Escape, Space, Up, Down, Left, Right, Delete, Home, End).
    bool TypeKey(const std::string& name);
    /// The Qt dialog when it is showing (asked on the GUI thread), else null: use the USB keyboard.
    QWidget* KeyboardDialog();

    std::string fifo_path;
    QObject* gui_context;
    InputCommon::InputSubsystem& input_subsystem;
    Hooks hooks;
    int fifo_fd = -1;
    std::atomic<bool> stop{false};
    std::thread reader;
};

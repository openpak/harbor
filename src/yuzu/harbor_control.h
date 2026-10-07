// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

class QObject;

namespace InputCommon {
class InputSubsystem;
}

/**
 * Test harness control channel. When the environment variable HARBOR_CONTROL names a path, a FIFO
 * is created there and line commands are read from it on a background thread:
 *
 *   press <button> [ms]   hold <button>   release <button>   stick <l|r> <x> <y> [ms]
 *   home   home-long   capture   power
 *   screenshot <png path>   threads   quit
 *
 * Buttons reach the guest through the virtual gamepad (the same input path the Android overlay
 * uses), on both the Player 1 and the Handheld controller. Every command is logged with the
 * prefix "harbor-control:".
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

    std::string fifo_path;
    QObject* gui_context;
    InputCommon::InputSubsystem& input_subsystem;
    Hooks hooks;
    int fifo_fd = -1;
    std::atomic<bool> stop{false};
    std::thread reader;
};

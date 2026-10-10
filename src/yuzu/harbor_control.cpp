// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <QApplication>
#include <QKeyEvent>
#include <QMetaObject>
#include <QObject>
#include <QWidget>

#include "common/logging.h"
#include "common/settings.h"
#include "common/settings_input.h"
#include "common/thread.h"
#include "core/hle/service/omm/idle_policy.h"
#include "hid_core/frontend/motion_input.h"
#include "input_common/drivers/keyboard.h"
#include "input_common/drivers/virtual_gamepad.h"
#include "input_common/main.h"
#include "yuzu/harbor_control.h"
#include "yuzu/harbor_test_pad.h"

namespace {

using VirtualButton = InputCommon::VirtualGamepad::VirtualButton;

// Player 1 and Handheld; the firmware's button poller listens on both.
constexpr std::size_t PlayerPorts[] = {0, 8};

constexpr int DefaultPressMs = 100;
// am/button_poller.cpp: short press < 400 ms, long press >= 800 ms (500 ms in a HOME session).
constexpr int HomeShortMs = 100;
constexpr int HomeLongMs = 1200;

std::optional<VirtualButton> ParseButton(std::string_view name) {
    struct Entry {
        std::string_view name;
        VirtualButton button;
    };
    static constexpr Entry Table[] = {
        {"A", VirtualButton::ButtonA},         {"B", VirtualButton::ButtonB},
        {"X", VirtualButton::ButtonX},         {"Y", VirtualButton::ButtonY},
        {"L", VirtualButton::TriggerL},        {"R", VirtualButton::TriggerR},
        {"ZL", VirtualButton::TriggerZL},      {"ZR", VirtualButton::TriggerZR},
        {"PLUS", VirtualButton::ButtonPlus},   {"MINUS", VirtualButton::ButtonMinus},
        {"UP", VirtualButton::ButtonUp},       {"DOWN", VirtualButton::ButtonDown},
        {"LEFT", VirtualButton::ButtonLeft},   {"RIGHT", VirtualButton::ButtonRight},
        {"LSTICK", VirtualButton::StickL},     {"RSTICK", VirtualButton::StickR},
        {"HOME", VirtualButton::ButtonHome},   {"CAPTURE", VirtualButton::ButtonCapture},
    };
    std::string upper(name);
    for (auto& c : upper) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    for (const auto& entry : Table) {
        if (entry.name == upper) {
            return entry.button;
        }
    }
    return std::nullopt;
}

// Emulated USB keyboard timing. hid samples the keyboard every 8 ms, but the firmware's keyboard
// reader looks at the shared memory once per frame (30 fps in HOME at worst, and frames stretch
// under load), so a key must be held, and released, for several frames to be seen: a shorter hold
// drops the key, a shorter gap after a release merges two presses of the same key into one.
// `key-timing <hold> <gap>` changes them at run time.
std::atomic<int> KeyHoldMs{100};
std::atomic<int> KeyGapMs{100};

/// The guest's keyboard layout (set:sys GetKeyboardLayout): which character a key position makes.
/// `layout <us|uk>` sets it; drive.sh sends the run's own setting before it types.
enum class KeyLayout { Us, Uk };
std::atomic<KeyLayout> CurrentLayout{KeyLayout::Us};

/// One key: its USB usage (Settings::NativeKeyboard::Keys) in the current layout, whether it needs
/// Shift, and the Qt key for Eden's Qt keyboard dialog (which takes characters, not positions).
struct KeyStroke {
    int usage;
    bool shift;
    int qt_key;
    QString text;
};

std::optional<KeyStroke> StrokeFor(char c) {
    namespace K = Settings::NativeKeyboard;
    const QString text = QString(QChar::fromLatin1(c));
    if (c >= 'a' && c <= 'z') {
        return KeyStroke{K::A + (c - 'a'), false, Qt::Key_A + (c - 'a'), text};
    }
    if (c >= 'A' && c <= 'Z') {
        return KeyStroke{K::A + (c - 'A'), true, Qt::Key_A + (c - 'A'), text};
    }
    if (c >= '1' && c <= '9') {
        return KeyStroke{K::N1 + (c - '1'), false, Qt::Key_1 + (c - '1'), text};
    }
    struct Entry {
        char c;
        int usage;
        bool shift;
    };
    // Where UK differs from US among printable ASCII: '"' and '@' swap places, '#' and '~' are on
    // the extra key left of Return (usage 0x32), '\\' and '|' on the extra key right of left Shift
    // (usage 0x64).
    static constexpr Entry UkTable[] = {
        {'"', K::N2, true},     {'@', K::Quote, true},        {'#', K::Tilde, false},
        {'~', K::Tilde, true},  {'\\', K::Key102, false},     {'|', K::Key102, true},
    };
    if (CurrentLayout.load() == KeyLayout::Uk) {
        for (const auto& entry : UkTable) {
            if (entry.c == c) {
                return KeyStroke{entry.usage, entry.shift, static_cast<int>(c), text};
            }
        }
    }
    static constexpr Entry Table[] = {
        {'0', K::N0, false},           {' ', K::Space, false},       {'!', K::N1, true},
        {'@', K::N2, true},            {'#', K::N3, true},           {'$', K::N4, true},
        {'%', K::N5, true},            {'^', K::N6, true},           {'&', K::N7, true},
        {'*', K::N8, true},            {'(', K::N9, true},           {')', K::N0, true},
        {'-', K::Minus, false},        {'_', K::Minus, true},        {'=', K::Plus, false},
        {'+', K::Plus, true},          {'[', K::OpenBracket, false}, {'{', K::OpenBracket, true},
        {']', K::CloseBracket, false}, {'}', K::CloseBracket, true}, {'\\', K::Pipe, false},
        {'|', K::Pipe, true},          {';', K::Semicolon, false},   {':', K::Semicolon, true},
        {'\'', K::Quote, false},       {'"', K::Quote, true},        {'`', K::Backquote, false},
        {'~', K::Backquote, true},     {',', K::Comma, false},       {'<', K::Comma, true},
        {'.', K::Period, false},       {'>', K::Period, true},       {'/', K::Slash, false},
        {'?', K::Slash, true},
    };
    for (const auto& entry : Table) {
        if (entry.c == c) {
            // Qt's key codes for printable ASCII are the characters themselves.
            return KeyStroke{entry.usage, entry.shift, static_cast<int>(c), text};
        }
    }
    return std::nullopt;
}

std::optional<KeyStroke> NamedKey(std::string_view name) {
    namespace K = Settings::NativeKeyboard;
    struct Entry {
        std::string_view name;
        int usage;
        int qt_key;
    };
    static constexpr Entry Table[] = {
        {"enter", K::Return, Qt::Key_Return},  {"return", K::Return, Qt::Key_Return},
        {"backspace", K::Backspace, Qt::Key_Backspace}, {"tab", K::Tab, Qt::Key_Tab},
        {"escape", K::Escape, Qt::Key_Escape}, {"esc", K::Escape, Qt::Key_Escape},
        {"space", K::Space, Qt::Key_Space},    {"up", K::Up, Qt::Key_Up},
        {"down", K::Down, Qt::Key_Down},       {"left", K::Left, Qt::Key_Left},
        {"right", K::Right, Qt::Key_Right},    {"delete", K::Delete, Qt::Key_Delete},
        {"home", K::Home, Qt::Key_Home},       {"end", K::End, Qt::Key_End},
    };
    std::string lower(name);
    for (auto& c : lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    for (const auto& entry : Table) {
        if (entry.name == lower) {
            const QString text = entry.qt_key == Qt::Key_Space ? QStringLiteral(" ") : QString{};
            return KeyStroke{entry.usage, false, entry.qt_key, text};
        }
    }
    return std::nullopt;
}

/// Line `n` (1-based) of `path`, the value alone when it is key=value; nothing when unreadable.
std::optional<std::string> ReadLine(const std::string& path, int n) {
    std::ifstream file(path);
    std::string line;
    for (int i = 0; i < n && std::getline(file, line); ++i) {
        if (i == n - 1) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (const auto eq = line.find('='); eq != std::string::npos) {
                line.erase(0, eq + 1);
            }
            return line;
        }
    }
    return std::nullopt;
}

std::vector<std::string> Split(const std::string& line) {
    std::vector<std::string> words;
    std::istringstream stream(line);
    for (std::string word; stream >> word;) {
        words.push_back(word);
    }
    return words;
}

} // namespace

std::unique_ptr<HarborControl> HarborControl::CreateFromEnvironment(
    QObject* gui_context, InputCommon::InputSubsystem& input_subsystem, Hooks hooks) {
    const char* const path = std::getenv("HARBOR_CONTROL");
    if (path == nullptr || *path == '\0') {
        return nullptr;
    }
    return std::unique_ptr<HarborControl>(
        new HarborControl(path, gui_context, input_subsystem, std::move(hooks)));
}

HarborControl::HarborControl(std::string fifo_path_, QObject* gui_context_,
                             InputCommon::InputSubsystem& input_subsystem_, Hooks hooks_)
    : fifo_path{std::move(fifo_path_)}, gui_context{gui_context_},
      input_subsystem{input_subsystem_}, hooks{std::move(hooks_)} {
#ifdef _WIN32
    // The harness is for Linux and macOS (a FIFO); Windows has no named pipe at a plain path.
    LOG_ERROR(Frontend, "harbor-control: not available on Windows");
    return;
#else
    std::error_code ec;
    std::filesystem::remove(fifo_path, ec);
    if (mkfifo(fifo_path.c_str(), 0600) != 0) {
        LOG_ERROR(Frontend, "harbor-control: cannot create FIFO \"{}\"", fifo_path);
        return;
    }
    // Read-write keeps the FIFO open when a writer disconnects, so the reader never sees EOF.
    fifo_fd = open(fifo_path.c_str(), O_RDWR | O_NONBLOCK);
    if (fifo_fd < 0) {
        LOG_ERROR(Frontend, "harbor-control: cannot open FIFO \"{}\"", fifo_path);
        return;
    }
    LOG_INFO(Frontend, "harbor-control: listening on \"{}\"", fifo_path);
    reader = std::thread([this] { ReadLoop(); });
#endif
}

HarborControl::~HarborControl() {
    stop = true;
    if (reader.joinable()) {
        reader.join();
    }
#ifndef _WIN32
    if (fifo_fd >= 0) {
        close(fifo_fd);
        std::error_code ec;
        std::filesystem::remove(fifo_path, ec);
    }
#endif
}

void HarborControl::ReadLoop() {
#ifndef _WIN32
    Common::SetCurrentThreadName("HarborControl");

    std::string pending;
    char chunk[512];
    while (!stop) {
        pollfd pfd{.fd = fifo_fd, .events = POLLIN, .revents = 0};
        if (poll(&pfd, 1, 200) <= 0) {
            continue;
        }
        const ssize_t n = read(fifo_fd, chunk, sizeof(chunk));
        if (n <= 0) {
            continue;
        }
        pending.append(chunk, static_cast<std::size_t>(n));
        for (std::size_t eol; (eol = pending.find('\n')) != std::string::npos;) {
            const std::string line = pending.substr(0, eol);
            pending.erase(0, eol + 1);
            Execute(line);
        }
    }
#endif
}

void HarborControl::SetButton(int button, bool pressed) {
    auto* const gamepad = input_subsystem.GetVirtualGamepad();
    for (const auto port : PlayerPorts) {
        gamepad->SetButtonState(port, button, pressed);
    }
}

void HarborControl::PressFor(int button, int ms) {
    SetButton(button, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    SetButton(button, false);
}

QWidget* HarborControl::KeyboardDialog() {
    if (!hooks.keyboard_dialog) {
        return nullptr;
    }
    auto found = std::make_shared<std::promise<QWidget*>>();
    auto result = found->get_future();
    QMetaObject::invokeMethod(
        gui_context, [this, found] { found->set_value(hooks.keyboard_dialog()); },
        Qt::QueuedConnection);
    return result.wait_for(std::chrono::seconds(2)) == std::future_status::ready ? result.get()
                                                                                  : nullptr;
}

namespace {

void Strike(InputCommon::Keyboard& keyboard, QWidget* dialog, const KeyStroke& key) {
    if (dialog) {
        // Eden's Qt dialog (swkbd HLE): Qt key events to whatever has focus in it.
        QMetaObject::invokeMethod(
            dialog,
            [dialog, key] {
                QWidget* target = dialog->focusWidget() ? dialog->focusWidget() : dialog;
                const auto modifiers = key.shift ? Qt::ShiftModifier : Qt::NoModifier;
                QKeyEvent press(QEvent::KeyPress, key.qt_key, modifiers, key.text);
                QApplication::sendEvent(target, &press);
                QKeyEvent release(QEvent::KeyRelease, key.qt_key, modifiers, key.text);
                QApplication::sendEvent(target, &release);
            },
            Qt::BlockingQueuedConnection);
        return;
    }
    // The emulated USB keyboard: Shift down first, as a person types.
    const auto hold = std::chrono::milliseconds(KeyHoldMs.load());
    const auto gap = std::chrono::milliseconds(KeyGapMs.load());
    if (key.shift) {
        keyboard.SetKeyboardModifiers(1 << Settings::NativeKeyboard::LeftShift);
        std::this_thread::sleep_for(gap);
    }
    keyboard.PressKeyboardKey(key.usage);
    std::this_thread::sleep_for(hold);
    keyboard.ReleaseKeyboardKey(key.usage);
    if (key.shift) {
        std::this_thread::sleep_for(gap);
        keyboard.SetKeyboardModifiers(0);
    }
    // Released for a whole gap before anything else is pressed, so a repeated key is two presses.
    std::this_thread::sleep_for(gap);
}

} // namespace

bool HarborControl::TypeText(const std::string& text) {
    std::vector<KeyStroke> keys;
    for (const char c : text) {
        const auto key = StrokeFor(c);
        if (!key) {
            return false;
        }
        keys.push_back(*key);
    }
    QWidget* const dialog = KeyboardDialog();
    // hid only writes the keyboard into shared memory when it is enabled.
    Settings::values.keyboard_enabled = true;
    for (const auto& key : keys) {
        Strike(*input_subsystem.GetKeyboard(), dialog, key);
    }
    return true;
}

bool HarborControl::TypeKey(const std::string& name) {
    const auto key = NamedKey(name);
    if (!key) {
        return false;
    }
    QWidget* const dialog = KeyboardDialog();
    Settings::values.keyboard_enabled = true;
    Strike(*input_subsystem.GetKeyboard(), dialog, *key);
    return true;
}

void HarborControl::Execute(const std::string& line) {
    const auto words = Split(line);
    if (words.empty()) {
        return;
    }
    const std::string& cmd = words[0];
    if (cmd == "type-line") {
        // A secret: neither the file, its contents nor anything about it beyond the length.
        const auto text = words.size() == 3 ? ReadLine(words[1], std::atoi(words[2].c_str()))
                                            : std::nullopt;
        if (!text) {
            LOG_WARNING(Frontend, "harbor-control: type-line: usage: type-line <file> <n> (line "
                                  "not readable)");
            return;
        }
        LOG_INFO(Frontend, "harbor-control: type-line ({} chars)", text->size());
        if (!TypeText(*text)) {
            LOG_WARNING(Frontend, "harbor-control: type-line: a character has no key on the "
                                  "keyboard layout; nothing typed");
        }
        return;
    }
    LOG_INFO(Frontend, "harbor-control: {}", line);

    const auto button_at = [&](std::size_t i) -> std::optional<int> {
        if (i >= words.size()) {
            return std::nullopt;
        }
        const auto button = ParseButton(words[i]);
        if (!button) {
            return std::nullopt;
        }
        return static_cast<int>(*button);
    };
    const auto ms_at = [&](std::size_t i, int fallback) {
        return i < words.size() ? std::atoi(words[i].c_str()) : fallback;
    };

    if (cmd == "press" || cmd == "hold" || cmd == "release") {
        const auto button = button_at(1);
        if (!button) {
            LOG_WARNING(Frontend, "harbor-control: unknown button in \"{}\"", line);
        } else if (cmd == "press") {
            PressFor(*button, ms_at(2, DefaultPressMs));
        } else {
            SetButton(*button, cmd == "hold");
        }
    } else if (cmd == "stick") {
        if (words.size() < 4 || (words[1] != "l" && words[1] != "r")) {
            LOG_WARNING(Frontend, "harbor-control: usage: stick <l|r> <x> <y> [ms]");
            return;
        }
        const auto stick = words[1] == "l" ? InputCommon::VirtualGamepad::VirtualStick::Left
                                           : InputCommon::VirtualGamepad::VirtualStick::Right;
        const float x = std::strtof(words[2].c_str(), nullptr);
        const float y = std::strtof(words[3].c_str(), nullptr);
        auto* const gamepad = input_subsystem.GetVirtualGamepad();
        for (const auto port : PlayerPorts) {
            gamepad->SetStickPosition(port, stick, x, y);
        }
        if (words.size() > 4) {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms_at(4, 0)));
            for (const auto port : PlayerPorts) {
                gamepad->SetStickPosition(port, stick, 0.0f, 0.0f);
            }
        }
    } else if (cmd == "motion") {
        // motion <x> <y> <z> [ms] | motion turn <x|y|z> <degrees> [ms]: the controller turns at
        // x, y, z degrees per second about its own axes (or by `degrees` about one axis) for ms
        // (default 500), then is held still. The sensor is reported every 5 ms, as a controller
        // reports it, so hid integrates the orientation; the accelerometer reads gravity as it
        // falls in the turned controller (1 g along -z before any turn: lying flat).
        const auto usage = [] {
            LOG_WARNING(Frontend, "harbor-control: usage: motion <x> <y> <z> [ms] | "
                                  "motion turn <x|y|z> <degrees> [ms] (degrees per second)");
        };
        float rate[3]{};
        int ms = 500;
        if (words.size() >= 4 && words[1] == "turn") {
            const int axis = words[2] == "x" ? 0 : words[2] == "y" ? 1 : words[2] == "z" ? 2 : -1;
            ms = ms_at(4, ms);
            if (axis < 0 || ms <= 0) {
                usage();
                return;
            }
            rate[axis] = std::strtof(words[3].c_str(), nullptr) * 1000.0f / static_cast<float>(ms);
        } else if (words.size() >= 4) {
            for (int i = 0; i < 3; ++i) {
                rate[i] = std::strtof(words[i + 1].c_str(), nullptr);
            }
            ms = ms_at(4, ms);
        } else {
            usage();
            return;
        }
        auto* const gamepad = input_subsystem.GetVirtualGamepad();
        // The controller's pose, integrated from the same gyroscope samples hid gets, gives the
        // gravity a real accelerometer would read in it. A fixed reading would make hid's drift
        // correction pull the controller back to where it started.
        static Core::HID::MotionInput pose;
        // hid's gyroscope unit is turns per second.
        const auto report = [&](const float* turn, u64 delta_us) {
            pose.SetGyroscope({turn[0], turn[1], turn[2]});
            pose.UpdateOrientation(delta_us);
            const auto q = pose.GetQuaternion();
            // MotionInput::UpdateOrientation's gravity estimate, as the accelerometer it expects.
            const float q1 = q[3], q2 = q[0], q3 = q[1], q4 = q[2];
            const float gx = 2.0f * (q2 * q4 - q1 * q3);
            const float gy = 2.0f * (q1 * q2 + q3 * q4);
            const float gz = q1 * q1 - q2 * q2 - q3 * q3 + q4 * q4;
            for (const auto port : PlayerPorts) {
                gamepad->SetMotionState(port, delta_us, turn[0], turn[1], turn[2], -gx, gy, -gz);
            }
        };
        const float turns[3]{rate[0] / 360.0f, rate[1] / 360.0f, rate[2] / 360.0f};
        const float still[3]{};
        using Clock = std::chrono::steady_clock;
        const auto start = Clock::now();
        const auto end = start + std::chrono::milliseconds(ms);
        auto last = start;
        report(still, 5000);
        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            const auto now = std::min(Clock::now(), end);
            const auto delta =
                std::chrono::duration_cast<std::chrono::microseconds>(now - last).count();
            report(turns, static_cast<u64>(delta));
            last = now;
            if (now >= end) {
                break;
            }
        }
        report(still, 5000);
    } else if (cmd == "home") {
        PressFor(static_cast<int>(VirtualButton::ButtonHome), HomeShortMs);
    } else if (cmd == "home-long") {
        PressFor(static_cast<int>(VirtualButton::ButtonHome), HomeLongMs);
    } else if (cmd == "capture") {
        PressFor(static_cast<int>(VirtualButton::ButtonCapture), HomeShortMs);
    } else if (cmd == "power") {
        if (hooks.power) {
            hooks.power();
        }
    } else if (cmd == "auto-sleep") {
        // Harness runs neither dim nor auto-sleep unless a test turns it on (idle:sys).
        if (words.size() < 2 || (words[1] != "on" && words[1] != "off")) {
            LOG_WARNING(Frontend, "harbor-control: usage: auto-sleep on|off");
            return;
        }
        Service::OMM::IdlePolicy::SetHarnessSuppressed(words[1] == "off");
    } else if (cmd == "power-tap") {
        // The power button pressed briefly: the window's own F5 tap (HarborSystemKeys), so the
        // host side runs as it does for a person. It sleeps the console, or wakes it.
        QMetaObject::invokeMethod(
            gui_context,
            [] {
                for (QWidget* window : QApplication::topLevelWidgets()) {
                    if (!window->inherits("QMainWindow")) {
                        continue;
                    }
                    QKeyEvent press(QEvent::KeyPress, Qt::Key_F5, Qt::NoModifier);
                    QApplication::sendEvent(window, &press);
                    QKeyEvent release(QEvent::KeyRelease, Qt::Key_F5, Qt::NoModifier);
                    QApplication::sendEvent(window, &release);
                    return;
                }
                LOG_WARNING(Frontend, "harbor-control: power-tap: no main window");
            },
            Qt::BlockingQueuedConnection);
    } else if (cmd == "terminate") {
        if (hooks.terminate) {
            hooks.terminate();
        }
    } else if (cmd == "screenshot") {
        if (words.size() < 2 || !hooks.screenshot) {
            LOG_WARNING(Frontend, "harbor-control: usage: screenshot <png path>");
            return;
        }
        // Resolve now: the working directory is the run directory.
        std::error_code ec;
        const auto path = std::filesystem::absolute(words[1], ec);
        std::filesystem::create_directories(path.parent_path(), ec);
        auto done = std::make_shared<std::promise<bool>>();
        auto result = done->get_future();
        QMetaObject::invokeMethod(
            gui_context,
            [this, file = path.string(), done] {
                hooks.screenshot(file, [done](bool ok) { done->set_value(ok); });
            },
            Qt::QueuedConnection);
        // Block so a following command in the same script sees the finished file.
        if (result.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
            LOG_WARNING(Frontend, "harbor-control: screenshot timed out: the renderer only captures on a new frame and the guest is not presenting");
        } else {
            LOG_INFO(Frontend, "harbor-control: screenshot {}", result.get() ? "ok" : "failed");
        }
    } else if (cmd == "type") {
        // Not secret: everything after the command, spaces included.
        const auto start = line.find_first_not_of(" \t", line.find("type") + 4);
        const std::string text = start == std::string::npos ? std::string{} : line.substr(start);
        if (!TypeText(text)) {
            LOG_WARNING(Frontend, "harbor-control: type: a character has no key on the "
                                  "keyboard layout; nothing typed");
        }
    } else if (cmd == "layout") {
        // layout <us|uk>, or the set:sys KeyboardLayout number (1 = English US, 3 = English UK).
        const std::string name = words.size() > 1 ? words[1] : std::string{};
        if (name == "us" || name == "1") {
            CurrentLayout = KeyLayout::Us;
        } else if (name == "uk" || name == "3") {
            CurrentLayout = KeyLayout::Uk;
        } else {
            LOG_WARNING(Frontend, "harbor-control: layout: supported layouts are us (1) and uk (3); "
                                  "unchanged");
        }
    } else if (cmd == "key-timing") {
        const int hold = words.size() > 1 ? std::atoi(words[1].c_str()) : 0;
        const int gap = words.size() > 2 ? std::atoi(words[2].c_str()) : 0;
        if (hold < 8 || gap < 8) {
            LOG_WARNING(Frontend, "harbor-control: usage: key-timing <hold ms> <gap ms> (each >= 8)");
        } else {
            KeyHoldMs = hold;
            KeyGapMs = gap;
        }
    } else if (cmd == "key") {
        if (words.size() < 2 || !TypeKey(words[1])) {
            LOG_WARNING(Frontend, "harbor-control: usage: key <Enter|Backspace|Tab|Escape|Space|"
                                  "Up|Down|Left|Right|Delete|Home|End>");
        }
    } else if (cmd == "dock") {
        if (hooks.dock) {
            QMetaObject::invokeMethod(gui_context, [this] { hooks.dock(); },
                                      Qt::BlockingQueuedConnection);
        }
    } else if (cmd == "detach" || cmd == "attach") {
        if (hooks.joycons) {
            QMetaObject::invokeMethod(
                gui_context, [this, detached = cmd == "detach"] { hooks.joycons(detached); },
                Qt::BlockingQueuedConnection);
        }
    } else if (cmd == "threads") {
        // The dump is safe from any host thread; running it here keeps it working when the GUI
        // thread is the one that is stuck.
        if (hooks.threads) {
            hooks.threads();
        }
    } else if (cmd == "pad") {
        // The SDL test pad (harbor_test_pad.h): a physical controller as Eden's SDL driver sees it.
        const auto usage = [] {
            LOG_WARNING(Frontend, "harbor-control: usage: pad plug | pad unplug | pad tap <button> [ms] | "
                                  "pad down <button> | pad up <button> | pad axis <n> <value> [ms]");
        };
        if (words.size() < 2) {
            usage();
        } else if (words[1] == "plug") {
            HarborTestPad::Attach();
        } else if (words[1] == "unplug") {
            HarborTestPad::Detach();
        } else if ((words[1] == "tap" || words[1] == "down" || words[1] == "up") && words.size() >= 3) {
            const int button = HarborTestPad::ButtonIndex(words[2]);
            if (button < 0 || !HarborTestPad::SetButton(button, words[1] != "up")) {
                usage();
            } else if (words[1] == "tap") {
                std::this_thread::sleep_for(std::chrono::milliseconds(ms_at(3, DefaultPressMs)));
                HarborTestPad::SetButton(button, false);
            }
        } else if (words[1] == "axis" && words.size() >= 4) {
            const int axis = std::atoi(words[2].c_str());
            if (!HarborTestPad::SetAxis(axis, std::strtof(words[3].c_str(), nullptr))) {
                usage();
            } else if (words.size() > 4) {
                std::this_thread::sleep_for(std::chrono::milliseconds(ms_at(4, 0)));
                HarborTestPad::SetAxis(axis, 0.0f);
            }
        } else {
            usage();
        }
    } else if (cmd == "hostkey") {
        // A key on the host keyboard as the render window passes it on (the keyboard engine's port
        // 0: what controller mappings and the Harbor menu read), not the guest's USB keyboard.
        static const std::pair<const char*, int> names[] = {
            {"Enter", Qt::Key_Return}, {"Escape", Qt::Key_Escape}, {"Backspace", Qt::Key_Backspace},
            {"Space", Qt::Key_Space},  {"Up", Qt::Key_Up},         {"Down", Qt::Key_Down},
            {"Left", Qt::Key_Left},    {"Right", Qt::Key_Right},   {"Tab", Qt::Key_Tab},
        };
        int code = 0;
        if (words.size() >= 2) {
            for (const auto& [name, value] : names) {
                if (words[1] == name) {
                    code = value;
                }
            }
            if (code == 0 && words[1].size() == 1 && std::isalnum(static_cast<unsigned char>(words[1][0]))) {
                code = std::toupper(static_cast<unsigned char>(words[1][0]));
            }
        }
        if (code == 0) {
            LOG_WARNING(Frontend, "harbor-control: usage: hostkey <Enter|Escape|Backspace|Space|Up|Down|"
                                  "Left|Right|Tab|a letter or digit> [ms]");
            return;
        }
        auto* const keyboard = input_subsystem.GetKeyboard();
        keyboard->PressKey(code);
        std::this_thread::sleep_for(std::chrono::milliseconds(ms_at(2, DefaultPressMs)));
        keyboard->ReleaseKey(code);
    } else if (cmd == "menu") {
        // A Harbor menu entry by name (its dialogs then take `key` and `type`).
        if (words.size() < 2 || !hooks.menu) {
            LOG_WARNING(Frontend, "harbor-control: usage: menu <name>");
            return;
        }
        QMetaObject::invokeMethod(
            gui_context, [this, name = words[1]] { hooks.menu(name); }, Qt::QueuedConnection);
    } else if (cmd == "quit") {
        if (hooks.quit) {
            QMetaObject::invokeMethod(gui_context, [this] { hooks.quit(); }, Qt::QueuedConnection);
        }
    } else {
        LOG_WARNING(Frontend, "harbor-control: unknown command \"{}\"", cmd);
    }
}

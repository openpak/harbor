// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <algorithm>
#include <chrono>
#include <fstream>

#include <nlohmann/json.hpp>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/thread.h"
#include "core/core.h"
#include "core/hle/service/am/applet_manager.h"
#include "harbor_setup/menu.h"
#include "harbor_setup/menu_input.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/frontend/input_converter.h"
#include "hid_core/hid_core.h"
#include "input_common/main.h"

namespace HarborSetup {

namespace NativeAnalog = Settings::NativeAnalog;
namespace NativeButton = Settings::NativeButton;

const std::array<MappableButton, 18> MappableButtons{{
    {NativeButton::A, "A"},
    {NativeButton::B, "B"},
    {NativeButton::X, "X"},
    {NativeButton::Y, "Y"},
    {NativeButton::L, "L"},
    {NativeButton::R, "R"},
    {NativeButton::ZL, "ZL"},
    {NativeButton::ZR, "ZR"},
    {NativeButton::Plus, "Plus"},
    {NativeButton::Minus, "Minus"},
    {NativeButton::Home, "HOME"},
    {NativeButton::Screenshot, "Capture"},
    {NativeButton::DUp, "Up"},
    {NativeButton::DDown, "Down"},
    {NativeButton::DLeft, "Left"},
    {NativeButton::DRight, "Right"},
    {NativeButton::LStick, "Left stick press"},
    {NativeButton::RStick, "Right stick press"},
}};

namespace {

constexpr std::size_t HandheldIndex = 8;

/// Player 1's mappings also go to Handheld (Harbor keeps them identical).
std::vector<std::size_t> Indices(int player) {
    if (player == 0) {
        return {0, HandheldIndex};
    }
    return {static_cast<std::size_t>(player)};
}

bool IsControllerEngine(const std::string& engine) {
    return !engine.empty() && engine != "any" && engine != "mouse" && engine != "cemuhookudp" &&
           engine != "virtual_gamepad" && engine != "touch_from_button" && engine != "analog_from_button";
}

const char* ButtonNameText(Common::Input::ButtonNames name) {
    using N = Common::Input::ButtonNames;
    switch (name) {
    case N::ButtonLeft: return "Left";
    case N::ButtonRight: return "Right";
    case N::ButtonDown: return "Down";
    case N::ButtonUp: return "Up";
    case N::ButtonA: return "A";
    case N::ButtonB: return "B";
    case N::ButtonX: return "X";
    case N::ButtonY: return "Y";
    case N::ButtonPlus: return "Plus";
    case N::ButtonMinus: return "Minus";
    case N::ButtonHome: return "HOME";
    case N::ButtonCapture: return "Capture";
    case N::ButtonStickL: return "Left stick";
    case N::ButtonStickR: return "Right stick";
    case N::TriggerL: return "L";
    case N::TriggerZL: return "ZL";
    case N::TriggerSL: return "SL";
    case N::TriggerR: return "R";
    case N::TriggerZR: return "ZR";
    case N::TriggerSR: return "SR";
    case N::TriggerZ: return "Z";
    case N::ButtonStart: return "Start";
    case N::L1: return "L1";
    case N::L2: return "L2";
    case N::L3: return "L3";
    case N::R1: return "R1";
    case N::R2: return "R2";
    case N::R3: return "R3";
    case N::Circle: return "Circle";
    case N::Cross: return "Cross";
    case N::Square: return "Square";
    case N::Triangle: return "Triangle";
    case N::Share: return "Share";
    case N::Options: return "Options";
    case N::Home: return "Home";
    case N::Touch: return "Touch";
    case N::ButtonMouseWheel: return "Wheel";
    case N::ButtonBackward: return "Back";
    case N::ButtonForward: return "Forward";
    case N::ButtonTask: return "Task";
    case N::ButtonExtra: return "Extra";
    default: return nullptr;
    }
}

} // namespace

// ---- InputStore ----

InputStore::InputStore(std::filesystem::path path_) : path{std::move(path_)} {
    Load();
}

std::string InputStore::DeviceKey(const Common::ParamPackage& device) {
    const auto engine = device.Get("engine", "");
    if (engine == "keyboard") {
        return "keyboard";
    }
    const auto guid = device.Get("guid", "");
    return engine + ":" + (guid.empty() ? device.Get("display", "") : guid);
}

void InputStore::Load() {
    std::ifstream file(path);
    if (!file) {
        return;
    }
    try {
        const auto json = nlohmann::json::parse(file);
        power = json.value("power", std::string{});
        if (const auto it = json.find("profiles"); it != json.end() && it->is_object()) {
            for (const auto& [key, value] : it->items()) {
                Profile profile;
                const auto buttons = value.value("buttons", std::vector<std::string>{});
                const auto analogs = value.value("analogs", std::vector<std::string>{});
                for (std::size_t i = 0; i < profile.buttons.size() && i < buttons.size(); i++) {
                    profile.buttons[i] = buttons[i];
                }
                for (std::size_t i = 0; i < profile.analogs.size() && i < analogs.size(); i++) {
                    profile.analogs[i] = analogs[i];
                }
                profiles.emplace_back(key, std::move(profile));
            }
        }
    } catch (const std::exception& e) {
        LOG_WARNING(Frontend, "harbor: {} is unreadable ({}); starting empty", path.filename().string(),
                    e.what());
    }
}

bool InputStore::Save() const {
    std::scoped_lock lk{mutex};
    nlohmann::json json;
    json["power"] = power;
    auto& out = json["profiles"] = nlohmann::json::object();
    for (const auto& [key, profile] : profiles) {
        out[key] = {
            {"buttons", std::vector<std::string>(profile.buttons.begin(), profile.buttons.end())},
            {"analogs", std::vector<std::string>(profile.analogs.begin(), profile.analogs.end())},
        };
    }
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const auto temp = path.string() + ".new";
    {
        std::ofstream file(temp, std::ios::trunc);
        if (!file) {
            return false;
        }
        file << json.dump(2);
        if (!file) {
            return false;
        }
    }
    std::filesystem::rename(temp, path, ec);
    return !ec;
}

std::optional<InputStore::Profile> InputStore::Find(const std::string& key) const {
    std::scoped_lock lk{mutex};
    for (const auto& [k, profile] : profiles) {
        if (k == key) {
            return profile;
        }
    }
    return std::nullopt;
}

void InputStore::Put(const std::string& key, const Profile& profile) {
    std::scoped_lock lk{mutex};
    for (auto& [k, existing] : profiles) {
        if (k == key) {
            existing = profile;
            return;
        }
    }
    profiles.emplace_back(key, profile);
}

std::string InputStore::Power() const {
    std::scoped_lock lk{mutex};
    return power;
}

void InputStore::SetPower(const std::string& param) {
    std::scoped_lock lk{mutex};
    power = param;
}

std::string RetargetParam(const std::string& param, const Common::ParamPackage& device) {
    if (param.empty()) {
        return param;
    }
    Common::ParamPackage package{param};
    const auto engine = device.Get("engine", "");
    if (package.Get("engine", "") == "analog_from_button") {
        for (const char* direction : {"up", "down", "left", "right", "modifier"}) {
            if (package.Has(direction)) {
                package.Set(direction, RetargetParam(package.Get(direction, ""), device));
            }
        }
        return package.Serialize();
    }
    if (package.Get("engine", "") != engine) {
        return param;
    }
    if (device.Has("port")) {
        package.Set("port", device.Get("port", 0));
    }
    if (device.Has("guid")) {
        package.Set("guid", device.Get("guid", ""));
    }
    return package.Serialize();
}

// ---- PowerKey ----

PowerKey::PowerKey(Core::System& system_) : system{system_} {
    worker = std::thread([this] { Run(); });
}

PowerKey::~PowerKey() {
    device.reset();
    {
        std::scoped_lock lk{mutex};
        stopping = true;
    }
    cv.notify_all();
    worker.join();
}

void PowerKey::Bind(const std::string& param) {
    device.reset();
    if (param.empty()) {
        return;
    }
    device = Common::Input::CreateInputDevice(Common::ParamPackage{param});
    if (!device) {
        return;
    }
    device->SetCallback({.on_change = [this](const Common::Input::CallbackStatus& status) {
        OnChange(Core::HID::TransformToButton(status).value);
    }});
    LOG_INFO(Frontend, "harbor: the Power button is mapped");
}

void PowerKey::OnChange(bool now) {
    bool short_press = false;
    {
        std::scoped_lock lk{mutex};
        if (now == pressed) {
            return;
        }
        pressed = now;
        if (pressed) {
            held_fired = false;
            press_serial++;
        } else {
            short_press = !held_fired;
        }
    }
    cv.notify_all();
    if (short_press && system.IsPoweredOn()) {
        // As the console: a short press sleeps (or wakes); see am's PowerButtonShortPress.
        LOG_INFO(Frontend, "harbor: Power button, short press");
        system.GetAppletManager().PowerButtonShortPress();
    }
}

void PowerKey::Run() {
    std::unique_lock lk{mutex};
    while (!stopping) {
        cv.wait(lk, [this] { return stopping || (pressed && !held_fired); });
        if (stopping) {
            return;
        }
        const auto serial = press_serial;
        // Held for a second: HOME's power menu, as the console's power button.
        if (cv.wait_for(lk, std::chrono::milliseconds(1000),
                        [this, serial] { return stopping || !pressed || press_serial != serial; })) {
            continue;
        }
        held_fired = true;
        lk.unlock();
        if (system.IsPoweredOn()) {
            LOG_INFO(Frontend, "harbor: Power button, held");
            system.GetAppletManager().PowerButtonMiddlePress();
        }
        lk.lock();
    }
}

// ---- InputMapping ----

InputMapping::InputMapping(MenuHost& host_, InputStore& store_) : host{host_}, store{store_} {}

std::vector<InputDevice> InputMapping::Devices() const {
    std::vector<InputDevice> out;
    for (const auto& device : host.Input().GetInputDevices()) {
        const auto engine = device.Get("engine", "");
        if (!IsControllerEngine(engine)) {
            continue;
        }
        out.push_back({device, engine == "keyboard" ? std::string{"Keyboard"} : device.Get("display", engine)});
    }
    return out;
}

Common::ParamPackage InputMapping::ButtonParam(int player, int button) const {
    const auto* controller = host.System().HIDCore().GetEmulatedControllerByIndex(Indices(player)[0]);
    return controller ? controller->GetButtonParam(button) : Common::ParamPackage{};
}

Common::ParamPackage InputMapping::StickParam(int player, int stick) const {
    const auto* controller = host.System().HIDCore().GetEmulatedControllerByIndex(Indices(player)[0]);
    return controller ? controller->GetStickParam(stick) : Common::ParamPackage{};
}

bool InputMapping::Connected(int player) const {
    if (player == 0) {
        return true;
    }
    const auto* controller =
        host.System().HIDCore().GetEmulatedControllerByIndex(static_cast<std::size_t>(player));
    return controller && controller->IsConnected(true);
}

void InputMapping::SetConnected(int player, bool connected) {
    if (player == 0) {
        return;
    }
    auto* controller = host.System().HIDCore().GetEmulatedControllerByIndex(static_cast<std::size_t>(player));
    if (!controller) {
        return;
    }
    // A pad on its own is a Pro Controller. While the menu is open the controller is in
    // configuration mode: the change takes effect when the menu closes.
    controller->SetNpadStyleIndex(Core::HID::NpadStyleIndex::Fullkey);
    connected ? controller->Connect(true) : controller->Disconnect();
    for (const bool global : {false, true}) {
        auto& settings = Settings::values.players.GetValue(global)[static_cast<std::size_t>(player)];
        settings.connected = connected;
        settings.controller_type = Settings::ControllerType::ProController;
    }
    LOG_INFO(Frontend, "harbor: player {} {}", player + 1, connected ? "connected" : "disconnected");
    host.SaveConfig();
}

std::optional<InputDevice> InputMapping::CurrentDevice(int player, bool* connected) const {
    if (connected) {
        *connected = false;
    }
    for (const auto& mappable : MappableButtons) {
        auto param = ButtonParam(player, mappable.index);
        if (param.Get("engine", "") == "analog_from_button") {
            continue;
        }
        const auto engine = param.Get("engine", "");
        if (!IsControllerEngine(engine)) {
            continue;
        }
        for (const auto& device : Devices()) {
            const auto& d = device.params;
            if (d.Get("engine", "") != engine) {
                continue;
            }
            if (engine == "keyboard" ||
                (d.Get("guid", "") == param.Get("guid", "") && d.Get("port", 0) == param.Get("port", 0))) {
                if (connected) {
                    *connected = true;
                }
                return device;
            }
        }
        // Mapped to a controller that is not connected now.
        Common::ParamPackage absent;
        absent.Set("engine", engine);
        if (param.Has("guid")) {
            absent.Set("guid", param.Get("guid", ""));
        }
        absent.Set("port", param.Get("port", 0));
        return InputDevice{absent, "A controller that is not connected"};
    }
    return std::nullopt;
}

std::string InputMapping::DeviceName(int player) const {
    const auto device = CurrentDevice(player);
    return device ? device->name : std::string{"Not set"};
}

void InputMapping::Apply(int player, const InputStore::Profile& profile) {
    for (const auto index : Indices(player)) {
        auto* controller = host.System().HIDCore().GetEmulatedControllerByIndex(index);
        if (!controller) {
            continue;
        }
        for (int i = 0; i < NativeButton::NumButtons; i++) {
            controller->SetButtonParam(i, Common::ParamPackage{profile.buttons[i]});
        }
        for (int i = 0; i < NativeAnalog::NumAnalogs; i++) {
            controller->SetStickParam(i, Common::ParamPackage{profile.analogs[i]});
        }
    }
}

std::optional<InputStore::Profile> InputMapping::Defaults(const InputDevice& device) const {
    InputStore::Profile defaults;
    if (device.params.Get("engine", "") == "keyboard") {
        const auto keyboard = host.DefaultKeyboard();
        if (!keyboard) {
            return std::nullopt;
        }
        for (int i = 0; i < NativeButton::NumButtons; i++) {
            defaults.buttons[i] = InputCommon::GenerateKeyboardParam(keyboard->buttons[i]);
        }
        for (int i = 0; i < NativeAnalog::NumAnalogs; i++) {
            const auto& keys = keyboard->analogs[i];
            defaults.analogs[i] = InputCommon::GenerateAnalogParamFromKeys(
                keys[0], keys[1], keys[2], keys[3], keyboard->modifiers[i], 0.5f);
        }
        return defaults;
    }
    for (const auto& [index, param] : host.Input().GetButtonMappingForDevice(device.params)) {
        defaults.buttons[index] = param.Serialize();
    }
    for (const auto& [index, param] : host.Input().GetAnalogMappingForDevice(device.params)) {
        defaults.analogs[index] = param.Serialize();
    }
    return defaults;
}

void InputMapping::UseDevice(int player, const InputDevice& device) {
    if (const auto saved = store.Find(InputStore::DeviceKey(device.params))) {
        InputStore::Profile moved = *saved;
        for (auto& param : moved.buttons) {
            param = RetargetParam(param, device.params);
        }
        for (auto& param : moved.analogs) {
            param = RetargetParam(param, device.params);
        }
        Apply(player, moved);
        LOG_INFO(Frontend, "harbor: player {} uses a controller with its saved mapping", player + 1);
        Commit(player);
        return;
    }
    const auto defaults = Defaults(device);
    if (!defaults) {
        return;
    }
    Apply(player, *defaults);
    LOG_INFO(Frontend, "harbor: player {} uses a controller with its default mapping", player + 1);
    Commit(player);
}

void InputMapping::RestoreDefaults(int player) {
    const auto device = CurrentDevice(player);
    if (!device) {
        return;
    }
    const auto defaults = Defaults(*device);
    if (!defaults) {
        return;
    }
    Apply(player, *defaults);
    LOG_INFO(Frontend, "harbor: player {} mapping restored to the controller's defaults", player + 1);
    // Commit stores the defaults as the controller's saved mapping too.
    Commit(player);
}

std::string InputMapping::Describe(const Common::ParamPackage& param) const {
    const auto engine = param.Get("engine", "");
    if (engine.empty()) {
        return "Not set";
    }
    if (engine == "keyboard") {
        return host.KeyName(param.Get("code", 0));
    }
    if (engine == "analog_from_button") {
        std::string out;
        for (const char* direction : {"up", "left", "down", "right"}) {
            if (!out.empty()) {
                out += " ";
            }
            out += Describe(Common::ParamPackage{param.Get(direction, "")});
        }
        return out;
    }
    if (param.Has("axis_x")) {
        return "Stick (axes " + std::to_string(param.Get("axis_x", 0)) + " and " +
               std::to_string(param.Get("axis_y", 1)) + ")";
    }
    if (const char* name = ButtonNameText(host.Input().GetButtonName(param))) {
        return name;
    }
    if (param.Has("hat")) {
        return "D-pad " + param.Get("direction", "");
    }
    if (param.Has("axis")) {
        const bool negative = param.Get("threshold", 0.5f) < 0 || param.Get("invert", "+") == "-";
        return "Axis " + std::to_string(param.Get("axis", 0)) + (negative ? "-" : "+");
    }
    if (param.Has("button")) {
        return (engine == "mouse" ? "Mouse button " : "Button ") + std::to_string(param.Get("button", 0));
    }
    if (param.Has("motion")) {
        return "Motion";
    }
    return "Set";
}

std::string InputMapping::ButtonText(int player, int button) const {
    return Describe(ButtonParam(player, button));
}

std::string InputMapping::StickText(int player, int stick) const {
    return Describe(StickParam(player, stick));
}

void InputMapping::SetButton(int player, int button, const Common::ParamPackage& param) {
    for (const auto index : Indices(player)) {
        if (auto* controller = host.System().HIDCore().GetEmulatedControllerByIndex(index)) {
            controller->SetButtonParam(button, param);
        }
    }
    Commit(player);
}

void InputMapping::ClearButton(int player, int button) {
    SetButton(player, button, Common::ParamPackage{});
}

void InputMapping::SetStick(int player, int stick, const Common::ParamPackage& param) {
    for (const auto index : Indices(player)) {
        if (auto* controller = host.System().HIDCore().GetEmulatedControllerByIndex(index)) {
            controller->SetStickParam(stick, param);
        }
    }
    Commit(player);
}

std::optional<int> InputMapping::DeadZone(int player, int stick) const {
    const auto param = StickParam(player, stick);
    if (!param.Has("axis_x")) {
        return std::nullopt;
    }
    return static_cast<int>(param.Get("deadzone", 0.15f) * 100.0f + 0.5f);
}

void InputMapping::SetDeadZone(int player, int stick, int percent) {
    auto param = StickParam(player, stick);
    if (!param.Has("axis_x")) {
        return;
    }
    param.Set("deadzone", static_cast<float>(std::clamp(percent, 0, 90)) / 100.0f);
    SetStick(player, stick, param);
}

std::optional<StickInvert> InputMapping::Invert(int player, int stick) const {
    const auto param = StickParam(player, stick);
    if (!param.Has("axis_x")) {
        return std::nullopt;
    }
    const bool x = param.Get("invert_x", "+") == "-";
    const bool y = param.Get("invert_y", "+") == "-";
    return x && y ? StickInvert::Both : x ? StickInvert::X : y ? StickInvert::Y : StickInvert::None;
}

void InputMapping::SetInvert(int player, int stick, StickInvert invert) {
    auto param = StickParam(player, stick);
    if (!param.Has("axis_x")) {
        return;
    }
    const bool x = invert == StickInvert::X || invert == StickInvert::Both;
    const bool y = invert == StickInvert::Y || invert == StickInvert::Both;
    param.Set("invert_x", x ? "-" : "+");
    param.Set("invert_y", y ? "-" : "+");
    SetStick(player, stick, param);
}

std::string InputMapping::PowerText() const {
    return Describe(Common::ParamPackage{store.Power()});
}

void InputMapping::SetPower(const Common::ParamPackage& param) {
    store.SetPower(param.Serialize());
    store.Save();
    StartPowerKey(host.System());
}

void InputMapping::ClearPower() {
    SetPower(Common::ParamPackage{});
}

void InputMapping::Commit(int player) {
    for (const auto index : Indices(player)) {
        if (auto* controller = host.System().HIDCore().GetEmulatedControllerByIndex(index)) {
            // SaveCurrentConfig writes the controller's connection too; in configuration mode
            // that is the live one, so the saved connection stays what it was.
            auto& settings = Settings::values.players.GetValue()[index];
            const bool connected = settings.connected;
            const auto type = settings.controller_type;
            controller->SaveCurrentConfig();
            settings.connected = connected;
            settings.controller_type = type;
            // A console's controllers are console-wide: with a title's own settings loaded the
            // mapping lands in its custom copy, so it goes to the global one too (what is saved).
            auto& players = Settings::values.players;
            if (!players.UsingGlobal()) {
                auto& global = players.GetValue(true)[index];
                global.buttons = settings.buttons;
                global.analogs = settings.analogs;
                global.motions = settings.motions;
            }
        }
    }
    host.SaveConfig();
    if (const auto device = CurrentDevice(player)) {
        InputStore::Profile profile;
        for (int i = 0; i < NativeButton::NumButtons; i++) {
            profile.buttons[i] = ButtonParam(player, i).Serialize();
        }
        for (int i = 0; i < NativeAnalog::NumAnalogs; i++) {
            profile.analogs[i] = StickParam(player, i).Serialize();
        }
        store.Put(InputStore::DeviceKey(device->params), profile);
        store.Save();
    }
}

bool InputMapping::ApplySaved(int player) {
    bool connected = false;
    const auto device = CurrentDevice(player, &connected);
    if (!device || !connected || device->params.Get("engine", "") == "keyboard") {
        return false;
    }
    const auto saved = store.Find(InputStore::DeviceKey(device->params));
    if (!saved) {
        return false;
    }
    InputStore::Profile wanted = *saved;
    bool differs = false;
    for (int i = 0; i < NativeButton::NumButtons; i++) {
        wanted.buttons[i] = RetargetParam(wanted.buttons[i], device->params);
        differs |= Common::ParamPackage{wanted.buttons[i]}.Serialize() != ButtonParam(player, i).Serialize();
    }
    for (int i = 0; i < NativeAnalog::NumAnalogs; i++) {
        wanted.analogs[i] = RetargetParam(wanted.analogs[i], device->params);
        differs |= Common::ParamPackage{wanted.analogs[i]}.Serialize() != StickParam(player, i).Serialize();
    }
    if (!differs) {
        return false;
    }
    Apply(player, wanted);
    Commit(player);
    LOG_INFO(Frontend, "harbor: player {}: a known controller came back, its saved mapping is applied",
             player + 1);
    return true;
}

// ---- InputCapture ----

InputCapture::InputCapture(InputCommon::InputSubsystem& input_) : input{input_} {}

InputCapture::~InputCapture() {
    End();
}

void InputCapture::Begin(Kind kind_) {
    kind = kind_;
    stick_step = -1;
    directions = {};
    active = true;
    input.BeginMapping(kind == Kind::Stick ? InputCommon::Polling::InputType::Stick
                                           : InputCommon::Polling::InputType::Button);
}

std::optional<Common::ParamPackage> InputCapture::Poll() {
    if (!active) {
        return std::nullopt;
    }
    auto param = input.GetNextInput();
    if (!param.Has("engine")) {
        return std::nullopt;
    }
    if (kind == Kind::Button || param.Has("axis_x")) {
        End();
        return param;
    }
    // A stick from keys or buttons: four presses, up, down, left, right (Eden's analog from
    // buttons, as its keyboard default).
    stick_step = std::max(stick_step, 0);
    directions[static_cast<std::size_t>(stick_step)] = param.Serialize();
    if (++stick_step < 4) {
        // Mapping again for the next direction; the same button twice does not count.
        input.BeginMapping(InputCommon::Polling::InputType::Stick);
        return std::nullopt;
    }
    End();
    Common::ParamPackage stick{
        {"engine", "analog_from_button"},
        {"up", directions[0]},
        {"down", directions[1]},
        {"left", directions[2]},
        {"right", directions[3]},
        {"modifier_scale", "0.5"},
    };
    return stick;
}

void InputCapture::End() {
    if (active) {
        input.StopMapping();
    }
    active = false;
}

// ---- Session ----

InputStore& HarborInputStore() {
    static InputStore store{Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "harbor-input.json"};
    return store;
}

namespace {
std::thread watcher;
std::mutex watcher_mutex;
std::condition_variable watcher_cv;
bool watcher_stop = false;
} // namespace

void StartControllerWatcher(MenuHost& host) {
    if (watcher.joinable()) {
        return;
    }
    watcher = std::thread([&host] {
        Common::SetCurrentThreadName("HarborPads");
        InputMapping mapping{host, HarborInputStore()};
        // What each player was on at the last look: a new device, port or player is a connect.
        std::array<std::string, InputMapping::Players> seen;
        std::unique_lock lk{watcher_mutex};
        while (!watcher_cv.wait_for(lk, std::chrono::seconds(1), [] { return watcher_stop; })) {
            lk.unlock();
            for (int player = 0; player < InputMapping::Players; player++) {
                bool connected = false;
                const auto device = mapping.CurrentDevice(player, &connected);
                const std::string now = device && connected
                                            ? InputStore::DeviceKey(device->params) + "@" +
                                                  device->params.Get("port", "0")
                                            : std::string{};
                if (now == seen[player]) {
                    continue;
                }
                seen[player] = now;
                if (!now.empty()) {
                    mapping.ApplySaved(player);
                }
            }
            lk.lock();
        }
    });
}

void StopControllerWatcher() {
    {
        std::scoped_lock lk{watcher_mutex};
        watcher_stop = true;
    }
    watcher_cv.notify_all();
    if (watcher.joinable()) {
        watcher.join();
    }
}

void StartPowerKey(Core::System& system) {
    static std::unique_ptr<PowerKey> key;
    if (!key) {
        key = std::make_unique<PowerKey>(system);
    }
    key->Bind(HarborInputStore().Power());
}

} // namespace HarborSetup

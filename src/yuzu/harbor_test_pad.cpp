// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <algorithm>
#include <cstdlib>
#include <mutex>

#include <SDL3/SDL.h>

#include "common/logging.h"
#include "yuzu/harbor_test_pad.h"

namespace HarborTestPad {

namespace {
std::mutex mutex;
SDL_JoystickID id = 0;
SDL_Joystick* joystick = nullptr;
} // namespace

bool Attach() {
    std::scoped_lock lk{mutex};
    if (joystick) {
        return true;
    }
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.vendor_id = 0x1209; // pid.codes test range: not a pad Eden or SDL treats specially
    desc.product_id = 0x0001;
    desc.name = "Harbor test pad";
    id = SDL_AttachVirtualJoystick(&desc);
    if (id == 0) {
        LOG_WARNING(Frontend, "harbor-control: the test pad could not be plugged in: {}", SDL_GetError());
        return false;
    }
    joystick = SDL_OpenJoystick(id);
    if (!joystick) {
        LOG_WARNING(Frontend, "harbor-control: the test pad could not be opened: {}", SDL_GetError());
        SDL_DetachVirtualJoystick(id);
        id = 0;
        return false;
    }
    LOG_INFO(Frontend, "harbor-control: test pad plugged in");
    return true;
}

void Detach() {
    std::scoped_lock lk{mutex};
    if (!joystick) {
        return;
    }
    SDL_CloseJoystick(joystick);
    SDL_DetachVirtualJoystick(id);
    joystick = nullptr;
    id = 0;
    LOG_INFO(Frontend, "harbor-control: test pad unplugged");
}

bool SetButton(int button, bool down) {
    std::scoped_lock lk{mutex};
    if (!joystick || button < 0 || button >= SDL_GAMEPAD_BUTTON_COUNT) {
        return false;
    }
    SDL_SetJoystickVirtualButton(joystick, button, down);
    // The virtual driver reports the change at the next joystick update; ask for one now.
    SDL_UpdateJoysticks();
    return true;
}

bool SetAxis(int axis, float value) {
    std::scoped_lock lk{mutex};
    if (!joystick || axis < 0 || axis >= SDL_GAMEPAD_AXIS_COUNT) {
        return false;
    }
    const float clamped = std::clamp(value, -1.0f, 1.0f);
    SDL_SetJoystickVirtualAxis(joystick, axis, static_cast<Sint16>(clamped * SDL_JOYSTICK_AXIS_MAX));
    SDL_UpdateJoysticks();
    return true;
}

int ButtonIndex(const std::string& name) {
    const SDL_GamepadButton button = SDL_GetGamepadButtonFromString(name.c_str());
    if (button != SDL_GAMEPAD_BUTTON_INVALID) {
        return static_cast<int>(button);
    }
    char* end = nullptr;
    const long number = std::strtol(name.c_str(), &end, 10);
    return end != name.c_str() && *end == '\0' ? static_cast<int>(number) : -1;
}

} // namespace HarborTestPad

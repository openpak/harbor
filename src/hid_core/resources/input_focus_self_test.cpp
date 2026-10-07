// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Self-check of the per-aruid input focus: an aruid without the input reads neutral npad entries
// whose sampling numbers advance, and a button or finger already down when an aruid gets the
// input back stays hidden from it until released. Runs once when hid starts with
// HARBOR_HID_SELFTEST set, against the live ResourceManager with three throwaway aruids, and logs
// "hid self-test: PASS" or the first failed check.

#include <array>
#include <string>

#include "common/logging.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "hid_core/hid_util.h"
#include "hid_core/resource_manager.h"
#include "hid_core/resources/applet_resource.h"
#include "hid_core/resources/npad/npad.h"
#include "hid_core/resources/shared_memory_format.h"
#include "hid_core/resources/touch_screen/touch_screen_resource.h"

namespace Service::HID {

namespace {
// Far above any process id, so no real aruid can collide with them.
constexpr u64 AruidA = 0x7E00'0000'0000'0001;
constexpr u64 AruidB = 0x7E00'0000'0000'0002;
// Never disabled: must read the input exactly as stock Eden writes it.
constexpr u64 AruidC = 0x7E00'0000'0000'0003;

TouchScreenState MakeTouches(s64 sampling_number, std::initializer_list<u32> fingers) {
    TouchScreenState state{};
    state.sampling_number = sampling_number;
    for (const u32 finger : fingers) {
        auto& entry = state.states[static_cast<std::size_t>(state.entry_count++)];
        entry.finger = finger;
        entry.position = {.x = 100 + finger * 50, .y = 100};
        entry.diameter_x = 15;
        entry.diameter_y = 15;
    }
    return state;
}
} // namespace

bool ResourceManager::RunInputFocusSelfTest() {
    using Core::HID::NpadButton;

    auto& kernel = system.Kernel();
    const std::array<u64, 3> aruids{AruidA, AruidB, AruidC};
    std::string failure;
    int checks = 0;
    const auto check = [&](bool ok, const char* what) {
        ++checks;
        if (!ok && failure.empty()) {
            failure = what;
        }
    };
    const auto memory = [&](u64 aruid) -> SharedMemoryFormat* {
        SharedMemoryFormat* format{};
        applet_resource->GetSharedMemoryFormat(&format, aruid);
        return format;
    };

    // A connected controller whose right-hand buttons reach the npad (not a left Joy-Con alone).
    std::size_t npad_index = MaxSupportedNpadIdTypes;
    for (std::size_t i = 0; i < MaxSupportedNpadIdTypes; ++i) {
        const auto* controller = system.HIDCore().GetEmulatedControllerByIndex(i);
        const auto style = controller != nullptr ? controller->GetNpadStyleIndex()
                                                 : Core::HID::NpadStyleIndex::None;
        if (controller != nullptr && controller->IsConnected() &&
            style != Core::HID::NpadStyleIndex::None &&
            style != Core::HID::NpadStyleIndex::JoyconLeft) {
            npad_index = i;
            break;
        }
    }

    const auto saved_style = system.HIDCore().GetSupportedStyleTag();
    for (const u64 aruid : aruids) {
        RegisterAppletResourceUserId(aruid, true);
        CreateAppletResource(aruid);
        npad->SetSupportedNpadStyleSet(kernel, aruid, Core::HID::NpadStyleSet::All);
        npad->Activate(aruid);
    }
    check(memory(AruidA) != nullptr && memory(AruidB) != nullptr && memory(AruidC) != nullptr,
          "every test aruid has its shared memory");

    if (failure.empty() && npad_index < MaxSupportedNpadIdTypes) {
        const auto pad = [&](u64 aruid) {
            std::scoped_lock lock{shared_mutex};
            return memory(aruid)
                ->npad.npad_entry[npad_index]
                .internal_state.system_ext_lifo.ReadCurrentEntry()
                .state;
        };
        const auto step = [&](NpadButton buttons) {
            npad->SetSelfTestButtons(aruids, buttons);
            npad->OnUpdate(kernel, system.CoreTiming());
        };

        // The first update connects the controller to every test aruid.
        step(NpadButton::None);
        EnablePadInput(AruidB, false);
        step(NpadButton::None);
        const s64 b_sampling = pad(AruidB).sampling_number;

        step(NpadButton::X);
        check(pad(AruidA).npad_buttons.raw == NpadButton::X, "A (input) sees X");
        check(pad(AruidB).npad_buttons.raw == NpadButton::None, "B (no input) reads no buttons");
        check(pad(AruidB).sampling_number > b_sampling, "B's sampling number advances");
        check(pad(AruidC).npad_buttons.raw == NpadButton::X, "C (never disabled) sees X");

        // The focus moves from A to B while X is still down.
        EnablePadInput(AruidA, false);
        EnablePadInput(AruidB, true);
        step(NpadButton::X);
        check(pad(AruidB).npad_buttons.raw == NpadButton::None, "B does not see the held X");
        check(pad(AruidA).npad_buttons.raw == NpadButton::None, "A (lost input) reads no buttons");
        check(pad(AruidC).npad_buttons.raw == NpadButton::X, "C still sees X");

        step(NpadButton::X | NpadButton::Y);
        check(pad(AruidB).npad_buttons.raw == NpadButton::Y, "B sees the new Y but not X");

        step(NpadButton::None);
        check(pad(AruidB).npad_buttons.raw == NpadButton::None, "B reads no buttons after release");

        step(NpadButton::X);
        check(pad(AruidB).npad_buttons.raw == NpadButton::X, "B sees X pressed again");

        // SystemExt only in the memory of the aruid that asked for it (hid:sys 322).
        npad->SetNpadSystemExtStateEnabled(AruidB, true);
        step(NpadButton::None);
        {
            std::scoped_lock lock{shared_mutex};
            const auto& b_style = memory(AruidB)->npad.npad_entry[npad_index].internal_state.style_tag;
            const auto& a_style = memory(AruidA)->npad.npad_entry[npad_index].internal_state.style_tag;
            check(b_style.system_ext.Value() != 0, "B's style carries SystemExt");
            check(a_style.system_ext.Value() == 0, "A's style does not carry SystemExt");
        }
    } else if (npad_index >= MaxSupportedNpadIdTypes) {
        LOG_WARNING(Service_HID, "hid self-test: no connected controller, npad checks skipped");
    }

    if (failure.empty()) {
        const auto touch = [&](u64 aruid) {
            std::scoped_lock lock{shared_mutex};
            return memory(aruid)->touch_screen.touch_screen_lifo.ReadCurrentEntry().state;
        };
        s64 sampling = 1;
        const auto step = [&](std::initializer_list<u32> fingers) {
            touch_resource->SetSelfTestTouches(aruids, MakeTouches(sampling++, fingers));
            touch_resource->OnTouchUpdate(0);
        };

        EnableTouchScreen(AruidB, false);
        step({0});
        check(touch(AruidA).entry_count == 1, "A (touch) sees finger 0");
        check(touch(AruidB).entry_count == 0, "B (no touch) sees no finger");

        EnableTouchScreen(AruidA, false);
        EnableTouchScreen(AruidB, true);
        step({0});
        check(touch(AruidB).entry_count == 0, "B does not see the held finger 0");

        step({0, 1});
        check(touch(AruidB).entry_count == 1 && touch(AruidB).states[0].finger == 1,
              "B sees the new finger 1 but not finger 0");

        step({});
        step({0});
        check(touch(AruidB).entry_count == 1 && touch(AruidB).states[0].finger == 0,
              "B sees finger 0 touched again");
        check(touch(AruidC).entry_count == 1, "C (never disabled) sees finger 0");
    }

    npad->SetSelfTestButtons({}, NpadButton::None);
    touch_resource->SetSelfTestTouches({}, {});
    for (const u64 aruid : aruids) {
        UnregisterAppletResourceUserId(aruid);
    }
    system.HIDCore().SetSupportedStyleTag(saved_style);

    if (!failure.empty()) {
        LOG_ERROR(Service_HID, "hid self-test: FAIL at check {}: {}", checks, failure);
        return false;
    }
    LOG_INFO(Service_HID, "hid self-test: PASS ({} checks)", checks);
    return true;
}

} // namespace Service::HID

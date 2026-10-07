// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.utils

import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.YuzuApplication
import org.yuzu.yuzu_emu.features.input.NativeInput
import org.yuzu.yuzu_emu.features.input.YuzuInputDevice
import org.yuzu.yuzu_emu.features.input.YuzuInputOverlayDevice
import org.yuzu.yuzu_emu.features.input.YuzuJoyconPair
import org.yuzu.yuzu_emu.features.input.YuzuPhysicalDevice

object InputHandler {
    var androidControllers = mapOf<Int, YuzuInputDevice>()
    var registeredControllers = mutableListOf<ParamPackage>()

    private val controllerButtons = intArrayOf(
        KeyEvent.KEYCODE_BUTTON_A,
        KeyEvent.KEYCODE_BUTTON_B,
        KeyEvent.KEYCODE_BUTTON_X,
        KeyEvent.KEYCODE_BUTTON_Y,
        KeyEvent.KEYCODE_BUTTON_L1,
        KeyEvent.KEYCODE_BUTTON_R1,
        KeyEvent.KEYCODE_BUTTON_L2,
        KeyEvent.KEYCODE_BUTTON_R2,
        KeyEvent.KEYCODE_BUTTON_THUMBL,
        KeyEvent.KEYCODE_BUTTON_THUMBR,
        KeyEvent.KEYCODE_BUTTON_START,
        KeyEvent.KEYCODE_BUTTON_SELECT,
        KeyEvent.KEYCODE_DPAD_UP,
        KeyEvent.KEYCODE_DPAD_DOWN,
        KeyEvent.KEYCODE_DPAD_LEFT,
        KeyEvent.KEYCODE_DPAD_RIGHT
    )

    private val controllerAxes = intArrayOf(
        MotionEvent.AXIS_X,
        MotionEvent.AXIS_Y,
        MotionEvent.AXIS_Z,
        MotionEvent.AXIS_RX,
        MotionEvent.AXIS_RY,
        MotionEvent.AXIS_RZ,
        MotionEvent.AXIS_HAT_X,
        MotionEvent.AXIS_HAT_Y,
        MotionEvent.AXIS_LTRIGGER,
        MotionEvent.AXIS_RTRIGGER
    )

    // Currently, Android doesn't support Joy-Con D-pad buttons. We fall back to the scan code
    private const val LINUX_BUTTON_DPAD_UP = 0x220
    private const val LINUX_BUTTON_DPAD_DOWN = 0x221
    private const val LINUX_BUTTON_DPAD_LEFT = 0x222
    private const val LINUX_BUTTON_DPAD_RIGHT = 0x223

    fun isPhysicalGameController(device: InputDevice?): Boolean {
        device ?: return false

        if (device.isVirtual) {
            return false
        }

        val sources = device.sources
        val hasControllerSource =
            sources and InputDevice.SOURCE_GAMEPAD == InputDevice.SOURCE_GAMEPAD ||
                sources and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK
        if (!hasControllerSource) {
            return false
        }

        val hasControllerButtons = device.hasKeys(*controllerButtons).any { it }
        val hasControllerAxes = device.motionRanges.any { range ->
            controllerAxes.contains(range.axis)
        }
        return hasControllerButtons || hasControllerAxes
    }

    fun dispatchKeyEvent(event: KeyEvent): Boolean {
        val action = when (event.action) {
            KeyEvent.ACTION_DOWN -> NativeInput.ButtonState.PRESSED
            KeyEvent.ACTION_UP -> NativeInput.ButtonState.RELEASED
            else -> return false
        }

        var controllerData = androidControllers[event.device.controllerNumber]
        if (controllerData == null) {
            updateControllerData()
            controllerData = androidControllers[event.device.controllerNumber] ?: return false
        }

        val button = getButtonIdFromEvent(event)
        val pad = controllerData.getGUID() to controllerData.getPort()
        // What is held, so a pause or a host panel can let go of all of it.
        if (action == NativeInput.ButtonState.PRESSED) {
            heldButtons.getOrPut(pad) { mutableSetOf() }.add(button)
        } else {
            heldButtons[pad]?.remove(button)
        }
        NativeInput.onGamePadButtonEvent(pad.first, pad.second, button, action)
        return true
    }

    private val heldButtons = mutableMapOf<Pair<String, Int>, MutableSet<Int>>()
    private val movedAxes = mutableMapOf<Pair<String, Int>, MutableSet<Int>>()

    /**
     * Harbor: lets go of every physical button and stick, as if the pads were put down. Called when the
     * emulation pauses or a host panel takes the input, so nothing stays held in the guest.
     * Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base), PhysicalPadState.cs.
     */
    fun releaseAll() {
        heldButtons.forEach { (pad, buttons) ->
            buttons.forEach {
                NativeInput.onGamePadButtonEvent(pad.first, pad.second, it, NativeInput.ButtonState.RELEASED)
            }
        }
        movedAxes.forEach { (pad, axes) ->
            axes.forEach { NativeInput.onGamePadAxisEvent(pad.first, pad.second, it, 0f) }
        }
        heldButtons.clear()
        movedAxes.clear()
    }

    fun getButtonIdFromEvent(event: KeyEvent): Int {
        if (event.keyCode == 0) {
            return when (event.scanCode) {
                LINUX_BUTTON_DPAD_UP -> KeyEvent.KEYCODE_DPAD_UP
                LINUX_BUTTON_DPAD_DOWN -> KeyEvent.KEYCODE_DPAD_DOWN
                LINUX_BUTTON_DPAD_LEFT -> KeyEvent.KEYCODE_DPAD_LEFT
                LINUX_BUTTON_DPAD_RIGHT -> KeyEvent.KEYCODE_DPAD_RIGHT
                else -> return 0
            }
        }
        return event.keyCode
    }

    fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
        val controllerData =
            androidControllers[event.device.controllerNumber] ?: return false
        val pad = controllerData.getGUID() to controllerData.getPort()
        event.device.motionRanges.forEach {
            val value = event.getAxisValue(it.axis)
            if (value != 0f) {
                movedAxes.getOrPut(pad) { mutableSetOf() }.add(it.axis)
            }
            NativeInput.onGamePadAxisEvent(pad.first, pad.second, it.axis, value)
        }
        return true
    }

    // Nintendo's Joy-Con halves, which Android reports as two separate pads.
    private const val NINTENDO_VID = 0x057e
    private const val JOYCON_LEFT_PID = 0x2006
    private const val JOYCON_RIGHT_PID = 0x2007

    fun getDevices(): Map<Int, YuzuInputDevice> {
        val gameControllerDeviceIds = mutableMapOf<Int, YuzuInputDevice>()
        val deviceIds = InputDevice.getDeviceIds()
        var port = 0
        val inputSettings = NativeConfig.getInputSettings(true)
        val devices = deviceIds.toList().mapNotNull { InputDevice.getDevice(it) }
            .filter { isPhysicalGameController(it) }

        // Harbor: a left and a right Joy-Con together are one pad (both halves of the console's
        // controller), on one port; the first pair found is joined, other halves stay separate.
        // Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base), PhysicalPadState.cs.
        val left = devices.firstOrNull { it.vendorId == NINTENDO_VID && it.productId == JOYCON_LEFT_PID }
        val right = devices.firstOrNull { it.vendorId == NINTENDO_VID && it.productId == JOYCON_RIGHT_PID }
        val pairPort = if (left != null && right != null) port++ else -1
        if (left != null && right != null) {
            val pair = YuzuJoyconPair(left, right, pairPort, inputSettings[pairPort].useSystemVibrator)
            gameControllerDeviceIds[left.controllerNumber] = pair
            gameControllerDeviceIds[right.controllerNumber] = pair
        }

        devices.forEach { device ->
            if (gameControllerDeviceIds.contains(device.controllerNumber)) {
                return@forEach
            }
            gameControllerDeviceIds[device.controllerNumber] = YuzuPhysicalDevice(
                device,
                port,
                inputSettings[port.coerceAtMost(inputSettings.size - 1)].useSystemVibrator
            )
            port++
        }
        return gameControllerDeviceIds
    }

    /**
     * Harbor: the first connected pad drives player 1 (and the handheld controller) with no setup
     * step, whenever player 1 is not already bound to a pad that is connected now.
     */
    fun bindFirstControllerIfUnbound() {
        val connected = androidControllers.values.map { it.getGUID() to it.getPort() }.toSet()
        if (connected.isEmpty()) {
            return
        }
        val player = NativeConfig.getInputSettings(true)[0]
        val bound = player.buttons.any { serialized ->
            val param = ParamPackage(serialized)
            param.get("engine", "") == "android" &&
                (param.get("guid", "") to param.get("port", -1)) in connected
        }
        if (bound) {
            return
        }
        val first = registeredControllers.firstOrNull { it.get("port", -1) == 0 } ?: return
        val name = first.get("display", YuzuApplication.appContext.getString(R.string.unknown))
        Log.info("[Harbor] binding the first controller to player 1")
        NativeInput.updateMappingsWithDefault(0, first, name)
        NativeConfig.saveGlobalConfig()
    }

    fun updateControllerData() {
        androidControllers = getDevices()
        androidControllers.values.distinct().forEach {
            NativeInput.registerController(it)
        }

        // Register the input overlay on a dedicated port for all player 1 vibrations
        NativeInput.registerController(YuzuInputOverlayDevice(androidControllers.isEmpty(), 100))
        registeredControllers.clear()
        NativeInput.getInputDevices().forEach {
            registeredControllers.add(ParamPackage(it))
        }
        registeredControllers.sortBy { it.get("port", 0) }
    }

    fun InputDevice.getGUID(): String = String.format("%016x%016x", productId, vendorId)
}

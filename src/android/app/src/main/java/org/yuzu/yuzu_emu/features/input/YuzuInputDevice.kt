// SPDX-FileCopyrightText: 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.yuzu.yuzu_emu.features.input

import android.view.InputDevice
import androidx.annotation.Keep
import org.yuzu.yuzu_emu.YuzuApplication
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.utils.InputHandler.getGUID

@Keep
interface YuzuInputDevice {
    fun getName(): String

    fun getGUID(): String

    fun getPort(): Int

    fun getSupportsVibration(): Boolean

    fun vibrate(intensity: Float)

    fun getAxes(): Array<Int> = arrayOf()
    fun hasKeys(keys: IntArray): BooleanArray = BooleanArray(0)
}

class YuzuPhysicalDevice(
    private val device: InputDevice,
    private val port: Int,
    useSystemVibrator: Boolean
) : YuzuInputDevice {
    private val vibrator = if (useSystemVibrator) {
        YuzuVibrator.getSystemVibrator()
    } else {
        YuzuVibrator.getControllerVibrator(device)
    }

    override fun getName(): String {
        return device.name
    }

    override fun getGUID(): String {
        return device.getGUID()
    }

    override fun getPort(): Int {
        return port
    }

    override fun getSupportsVibration(): Boolean {
        return vibrator.supportsVibration()
    }

    override fun vibrate(intensity: Float) {
        vibrator.vibrate(intensity)
    }

    override fun getAxes(): Array<Int> = device.motionRanges.map { it.axis }.toTypedArray()
    override fun hasKeys(keys: IntArray): BooleanArray = device.hasKeys(*keys)
}

/**
 * Harbor: a left and a right Joy-Con as one pad. Both halves' events go to one guid and port (the left
 * half's), and the mapping sees the union of their buttons and axes: the left stick and D-pad from the
 * left half, the right stick and face buttons from the right.
 */
class YuzuJoyconPair(
    private val left: InputDevice,
    private val right: InputDevice,
    private val port: Int,
    useSystemVibrator: Boolean
) : YuzuInputDevice {
    private val vibrator = if (useSystemVibrator) {
        YuzuVibrator.getSystemVibrator()
    } else {
        YuzuVibrator.getControllerVibrator(right)
    }

    override fun getName(): String = "${left.name} + ${right.name}"

    override fun getGUID(): String = left.getGUID()

    override fun getPort(): Int = port

    override fun getSupportsVibration(): Boolean = vibrator.supportsVibration()

    override fun vibrate(intensity: Float) {
        vibrator.vibrate(intensity)
    }

    override fun getAxes(): Array<Int> =
        (left.motionRanges.map { it.axis } + right.motionRanges.map { it.axis }).distinct().toTypedArray()

    override fun hasKeys(keys: IntArray): BooleanArray {
        val l = left.hasKeys(*keys)
        val r = right.hasKeys(*keys)
        return BooleanArray(keys.size) { l[it] || r[it] }
    }
}

class YuzuInputOverlayDevice(
    private val vibration: Boolean,
    private val port: Int
) : YuzuInputDevice {
    private val vibrator = YuzuVibrator.getSystemVibrator()

    override fun getName(): String {
        return YuzuApplication.appContext.getString(R.string.input_overlay)
    }

    override fun getGUID(): String {
        return "00000000000000000000000000000000"
    }

    override fun getPort(): Int {
        return port
    }

    override fun getSupportsVibration(): Boolean {
        if (vibration) {
            return vibrator.supportsVibration()
        }
        return false
    }

    override fun vibrate(intensity: Float) {
        if (vibration) {
            vibrator.vibrate(intensity)
        }
    }
}

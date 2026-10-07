// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

package org.yuzu.yuzu_emu.harbor

import android.app.Dialog
import android.os.Handler
import android.os.Looper
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.Window
import kotlin.math.abs

/**
 * Controller input for Harbor's host panels (the Harbor menu and its pages), by Switch position like
 * the guest: the right face button (A) or Plus confirms, the bottom one (B) or Minus goes back. The
 * D-pad, the hat and the left stick move the focus once at once, then again after 350 ms and every
 * 110 ms while held; Android's own key repeat and stick-to-D-pad synthesis are replaced.
 * Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base), GamepadNavigation.cs and
 * ControllerPanel.cs.
 */
object HarborPanelInput {
    private const val REPEAT_DELAY_MS = 350L
    private const val REPEAT_INTERVAL_MS = 110L
    private const val STICK_START = 0.55f
    private const val STICK_HOLD = 0.4f
    private const val HAT_THRESHOLD = 0.5f

    /** Puts `dialog`'s window under this input. Call after show(). */
    fun attach(dialog: Dialog) {
        val window = dialog.window ?: return
        val current = window.callback ?: return
        if (current !is PanelCallback) {
            window.callback = PanelCallback(current)
        }
    }

    private fun isController(source: Int): Boolean =
        source and InputDevice.SOURCE_GAMEPAD == InputDevice.SOURCE_GAMEPAD ||
            source and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK ||
            source and InputDevice.SOURCE_DPAD == InputDevice.SOURCE_DPAD

    private class PanelCallback(private val delegate: Window.Callback) : Window.Callback by delegate {
        private val handler = Handler(Looper.getMainLooper())
        private var held = 0 // the D-pad keycode being repeated, 0 for none
        private var heldByStick = false
        private val repeat = object : Runnable {
            override fun run() {
                if (held != 0) {
                    tap(held)
                    handler.postDelayed(this, REPEAT_INTERVAL_MS)
                }
            }
        }

        override fun dispatchKeyEvent(event: KeyEvent): Boolean {
            val deviceSources = event.device?.sources ?: 0
            if (!isController(event.source) && !isController(deviceSources)) {
                return delegate.dispatchKeyEvent(event)
            }
            return when (event.keyCode) {
                KeyEvent.KEYCODE_BUTTON_B, KeyEvent.KEYCODE_BUTTON_START ->
                    delegate.dispatchKeyEvent(remap(event, KeyEvent.KEYCODE_DPAD_CENTER))
                KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_BUTTON_SELECT ->
                    delegate.dispatchKeyEvent(remap(event, KeyEvent.KEYCODE_BACK))
                KeyEvent.KEYCODE_DPAD_UP, KeyEvent.KEYCODE_DPAD_DOWN,
                KeyEvent.KEYCODE_DPAD_LEFT, KeyEvent.KEYCODE_DPAD_RIGHT -> {
                    when (event.action) {
                        KeyEvent.ACTION_DOWN -> if (event.repeatCount == 0) start(event.keyCode, false)
                        KeyEvent.ACTION_UP -> if (held == event.keyCode && !heldByStick) stop()
                    }
                    true
                }
                // Other pad buttons do nothing in a panel, and never reach the guest.
                else -> true
            }
        }

        override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
            if (!isController(event.source) || event.actionMasked != MotionEvent.ACTION_MOVE) {
                return delegate.dispatchGenericMotionEvent(event)
            }
            val hatX = event.getAxisValue(MotionEvent.AXIS_HAT_X)
            val hatY = event.getAxisValue(MotionEvent.AXIS_HAT_Y)
            val stickX = event.getAxisValue(MotionEvent.AXIS_X)
            val stickY = event.getAxisValue(MotionEvent.AXIS_Y)

            val hat = direction(hatX, hatY, HAT_THRESHOLD)
            // A held stick direction keeps going down to the lower threshold.
            val threshold = if (heldByStick && held != 0) STICK_HOLD else STICK_START
            val stick = direction(stickX, stickY, threshold)
            val wanted = if (hat != 0) hat else stick
            if (wanted != held) {
                stop()
                if (wanted != 0) {
                    start(wanted, hat == 0)
                }
            }
            return true
        }

        private fun direction(x: Float, y: Float, threshold: Float): Int {
            if (abs(x) < threshold && abs(y) < threshold) {
                return 0
            }
            return if (abs(x) > abs(y)) {
                if (x > 0) KeyEvent.KEYCODE_DPAD_RIGHT else KeyEvent.KEYCODE_DPAD_LEFT
            } else {
                if (y > 0) KeyEvent.KEYCODE_DPAD_DOWN else KeyEvent.KEYCODE_DPAD_UP
            }
        }

        private fun start(code: Int, byStick: Boolean) {
            stop()
            held = code
            heldByStick = byStick
            tap(code)
            handler.postDelayed(repeat, REPEAT_DELAY_MS)
        }

        private fun stop() {
            held = 0
            heldByStick = false
            handler.removeCallbacks(repeat)
        }

        private fun tap(code: Int) {
            delegate.dispatchKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, code))
            delegate.dispatchKeyEvent(KeyEvent(KeyEvent.ACTION_UP, code))
        }

        private fun remap(event: KeyEvent, code: Int): KeyEvent = KeyEvent(
            event.downTime,
            event.eventTime,
            event.action,
            code,
            event.repeatCount,
            event.metaState,
            event.deviceId,
            event.scanCode,
            event.flags,
            event.source
        )

        override fun onDetachedFromWindow() {
            stop()
            delegate.onDetachedFromWindow()
        }
    }
}

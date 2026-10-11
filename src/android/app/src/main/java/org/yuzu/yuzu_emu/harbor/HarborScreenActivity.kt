// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

package org.yuzu.yuzu_emu.harbor

import android.content.Context
import android.media.AudioManager
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.widget.FrameLayout
import androidx.activity.ComponentActivity
import androidx.activity.OnBackPressedCallback
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import kotlin.math.abs

/**
 * A screen of Harbor's own outside the console (the launcher's status, "Harbor stopped", "Harbor
 * cannot start", the driver that did not load): a Borealis page drawn by the Harbor menu
 * (harbor_setup/menu.h, MenuShowHostPage) into a 16:9 surface, the same page the desktop shows.
 * Touch, keys and controllers go to the page by the console's positions, as HarborPanelInput maps
 * them: the right face button (A) or Plus confirms, the bottom one (B), Minus or Back goes back.
 * What the page asks for arrives in [onHostAction] on the UI thread.
 */
abstract class HarborScreenActivity : ComponentActivity() {
    private var pageKind = KIND_STATUS
    private var pageText = ""
    private var resumed = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        volumeControlStream = AudioManager.STREAM_MUSIC
        setContentView(HarborScreenView.frame(this))
        WindowCompat.setDecorFitsSystemWindows(window, false)
        WindowInsetsControllerCompat(window, window.decorView).apply {
            hide(WindowInsetsCompat.Type.systemBars())
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
        // Back (the gesture or the key) is the page's own B.
        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() = tap(BUTTON_B)
        })
    }

    /** Shows a page (from any thread): one of the KIND_ constants and its text. */
    fun showPage(kind: Int, text: String) {
        runOnUiThread {
            pageKind = kind
            pageText = text
            if (resumed) {
                HarborNative.showHostPage(kind, text)
            }
        }
    }

    override fun onResume() {
        super.onResume()
        resumed = true
        current = this
        borealisUsed = true
        HarborNative.showHostPage(pageKind, pageText)
    }

    override fun onPause() {
        resumed = false
        if (current === this) {
            current = null
            HarborNative.endHostPage()
        }
        super.onPause()
    }

    /** What the page asked for: see MenuHost::HostPageAction. */
    abstract fun onHostAction(action: String, argument: String)

    // Keys and controllers, by the console's positions.
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        val button = when (event.keyCode) {
            KeyEvent.KEYCODE_BUTTON_B, KeyEvent.KEYCODE_BUTTON_START, KeyEvent.KEYCODE_ENTER,
            KeyEvent.KEYCODE_DPAD_CENTER, KeyEvent.KEYCODE_SPACE, KeyEvent.KEYCODE_NUMPAD_ENTER -> BUTTON_A
            KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_BUTTON_SELECT, KeyEvent.KEYCODE_ESCAPE,
            KeyEvent.KEYCODE_BACK, KeyEvent.KEYCODE_DEL -> BUTTON_B
            KeyEvent.KEYCODE_BUTTON_Y, KeyEvent.KEYCODE_X -> BUTTON_X
            KeyEvent.KEYCODE_BUTTON_X -> BUTTON_Y
            KeyEvent.KEYCODE_DPAD_UP -> BUTTON_UP
            KeyEvent.KEYCODE_DPAD_DOWN -> BUTTON_DOWN
            KeyEvent.KEYCODE_DPAD_LEFT -> BUTTON_LEFT
            KeyEvent.KEYCODE_DPAD_RIGHT -> BUTTON_RIGHT
            else -> return super.dispatchKeyEvent(event)
        }
        if (event.repeatCount == 0) {
            when (event.action) {
                KeyEvent.ACTION_DOWN -> HarborNative.hostButton(button, true)
                KeyEvent.ACTION_UP -> HarborNative.hostButton(button, false)
            }
        }
        return true
    }

    // The left stick and the hat as the D-pad (Borealis repeats a held direction itself).
    private val axisHeld = BooleanArray(4)

    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
        if (event.source and InputDevice.SOURCE_JOYSTICK != InputDevice.SOURCE_JOYSTICK ||
            event.action != MotionEvent.ACTION_MOVE
        ) {
            return super.dispatchGenericMotionEvent(event)
        }
        val x = event.getAxisValue(MotionEvent.AXIS_X).let { if (abs(it) > abs(hatX(event))) it else hatX(event) }
        val y = event.getAxisValue(MotionEvent.AXIS_Y).let { if (abs(it) > abs(hatY(event))) it else hatY(event) }
        setAxis(0, BUTTON_UP, y < -STICK)
        setAxis(1, BUTTON_DOWN, y > STICK)
        setAxis(2, BUTTON_LEFT, x < -STICK)
        setAxis(3, BUTTON_RIGHT, x > STICK)
        return true
    }

    private fun hatX(event: MotionEvent) = event.getAxisValue(MotionEvent.AXIS_HAT_X)
    private fun hatY(event: MotionEvent) = event.getAxisValue(MotionEvent.AXIS_HAT_Y)

    private fun setAxis(index: Int, button: Int, held: Boolean) {
        if (axisHeld[index] != held) {
            axisHeld[index] = held
            HarborNative.hostButton(button, held)
        }
    }

    private val handler = Handler(Looper.getMainLooper())

    /** A button pressed and released (Android's back, which has no separate release). */
    private fun tap(button: Int) {
        HarborNative.hostButton(button, true)
        handler.postDelayed({ HarborNative.hostButton(button, false) }, TAP_MS)
    }

    companion object {
        const val KIND_STATUS = 0
        const val KIND_STOPPED = 1
        const val KIND_CANNOT_START = 2
        const val KIND_DRIVER_FAILED = 3

        // HarborSetup::HostButton
        private const val BUTTON_A = 0
        private const val BUTTON_B = 1
        private const val BUTTON_X = 2
        private const val BUTTON_Y = 3
        private const val BUTTON_UP = 4
        private const val BUTTON_DOWN = 5
        private const val BUTTON_LEFT = 6
        private const val BUTTON_RIGHT = 7

        private const val STICK = 0.5f
        private const val TAP_MS = 80L

        /** Whether this process drew a page (Borealis then runs here; the setup needs a new process). */
        @Volatile
        var borealisUsed = false

        /** The screen whose page is up, for HarborMenuBridge's "hostPage" requests. */
        @Volatile
        var current: HarborScreenActivity? = null
    }
}

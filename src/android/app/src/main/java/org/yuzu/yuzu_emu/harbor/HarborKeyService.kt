// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

package org.yuzu.yuzu_emu.harbor

import android.accessibilityservice.AccessibilityService
import android.content.ComponentName
import android.content.Context
import android.provider.Settings
import android.view.KeyEvent
import android.view.accessibility.AccessibilityEvent
import org.yuzu.yuzu_emu.activities.EmulationActivity
import org.yuzu.yuzu_emu.utils.InputHandler
import org.yuzu.yuzu_emu.utils.Log

/**
 * Android acts on a controller's HOME key (and some phones on its Mode key) before any app sees it.
 * With this accessibility service on, those two keys from a physical controller go to the console
 * instead, and only while Harbor's console screen is in front. Every other key passes untouched.
 */
class HarborKeyService : AccessibilityService() {
    private val logged = mutableSetOf<Int>()

    override fun onKeyEvent(event: KeyEvent): Boolean {
        val code = event.keyCode
        if (code != KeyEvent.KEYCODE_HOME && code != KeyEvent.KEYCODE_BUTTON_MODE) return false
        val activity = EmulationActivity.foreground ?: return false
        if (!HarborConsole.active || !InputHandler.isPhysicalGameController(event.device)) return false
        if (logged.add(code)) {
            Log.info("[Harbor] key service: ${KeyEvent.keyCodeToString(code)} from a controller")
        }
        return activity.dispatchKeyEvent(event)
    }

    override fun onAccessibilityEvent(event: AccessibilityEvent?) {}

    override fun onInterrupt() {}

    companion object {
        fun enabled(context: Context): Boolean {
            val name = ComponentName(context, HarborKeyService::class.java).flattenToString()
            val list = Settings.Secure.getString(
                context.contentResolver,
                Settings.Secure.ENABLED_ACCESSIBILITY_SERVICES
            ) ?: return false
            return list.split(':').any { it.equals(name, ignoreCase = true) }
        }
    }
}

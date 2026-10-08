// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

package org.yuzu.yuzu_emu.harbor

import android.app.Activity
import android.content.Context
import android.content.res.Configuration
import android.graphics.Color
import android.view.ContextThemeWrapper
import org.yuzu.yuzu_emu.R

/**
 * The theme of Harbor's own screens (launcher, stopped screen, Harbor menu): the console's colour set
 * while it runs, its saved one while it is stopped, and the phone's before the console has booted
 * once. Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base), App.FollowConsoleTheme.
 */
object HarborTheme {
    val PAPER_LIGHT = Color.rgb(0xeb, 0xeb, 0xeb)
    val PAPER_DARK = Color.rgb(0x2d, 0x2d, 0x2d)
    val INK_LIGHT = Color.rgb(0x2d, 0x2d, 0x2d)
    val INK_DARK = Color.rgb(0xeb, 0xeb, 0xeb)

    fun dark(context: Context): Boolean {
        val live = HarborNative.consoleTheme()
        if (live >= 0) {
            return live == 1
        }
        val saved = HarborNative.savedConsoleTheme()
        if (saved >= 0) {
            return saved == 1
        }
        return context.resources.configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK ==
            Configuration.UI_MODE_NIGHT_YES
    }

    /** `activity` with the console's day or night resources, for the Harbor menu's dialogs. */
    fun dialogContext(activity: Activity): Context {
        val night = if (dark(activity)) Configuration.UI_MODE_NIGHT_YES else Configuration.UI_MODE_NIGHT_NO
        val override = Configuration().apply {
            uiMode = (activity.resources.configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK.inv()) or night
        }
        return ContextThemeWrapper(activity, R.style.Theme_Eden_Main).apply { applyOverrideConfiguration(override) }
    }

    /** The platform dialog theme for plain AlertDialogs (the driver refusal). */
    fun alertTheme(activity: Activity): Int =
        if (dark(activity)) {
            android.R.style.Theme_DeviceDefault_Dialog_Alert
        } else {
            android.R.style.Theme_DeviceDefault_Light_Dialog_Alert
        }
}

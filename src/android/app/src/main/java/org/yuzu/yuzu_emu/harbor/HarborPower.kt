// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

package org.yuzu.yuzu_emu.harbor

import org.yuzu.yuzu_emu.utils.Log

/**
 * The console's power button on the phone (the touch controls' power button, Volume Down): a short
 * press puts the console to sleep or wakes it (AppletManager::PowerButtonShortPress), a press held
 * for HOLD_MS is the power button's hold, which opens HOME's power menu
 * (AppletManager::PowerButtonMiddlePress, the desktop's F5 held).
 */
object HarborPower {
    /** The hold that opens the power menu, the same on every power input. */
    const val HOLD_MS = 1000L

    fun shortPress(source: String) {
        Log.info("[Harbor] power button pressed ($source)")
        HarborNative.powerShortPress()
    }

    fun hold(source: String) {
        Log.info("[Harbor] power button held ($source)")
        if (HarborSleep.sleeping) {
            // A sleeping console wakes first, as a held power button wakes a Switch.
            HarborNative.powerShortPress()
            return
        }
        HarborNative.pressPower()
    }
}

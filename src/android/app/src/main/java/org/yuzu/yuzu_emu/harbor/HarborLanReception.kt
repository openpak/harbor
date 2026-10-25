// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

package org.yuzu.yuzu_emu.harbor

import android.content.Context
import android.net.wifi.WifiManager
import org.yuzu.yuzu_emu.utils.Log

/** Keep Android's Wi-Fi filter from dropping local lobby broadcasts while playing. */
class HarborLanReception {
    private var lock: WifiManager.MulticastLock? = null

    fun acquire(context: Context) {
        if (lock != null) return
        try {
            val wifi = context.applicationContext.getSystemService(Context.WIFI_SERVICE) as? WifiManager
            lock = wifi?.createMulticastLock("harbor-local-play")?.apply {
                setReferenceCounted(false)
                acquire()
            }
        } catch (error: SecurityException) {
            Log.warning("[Harbor] Could not enable LAN discovery reception: ${error.message}")
        }
    }

    fun release() {
        lock?.let { if (it.isHeld) it.release() }
        lock = null
    }
}

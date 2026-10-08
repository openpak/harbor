// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

package org.yuzu.yuzu_emu.harbor

import android.graphics.Color
import android.os.Handler
import android.os.Looper
import android.view.View
import android.view.ViewGroup
import androidx.annotation.Keep
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.activities.EmulationActivity
import org.yuzu.yuzu_emu.utils.Log

/**
 * The console's sleep on the phone. am keeps the state (AppletManager::ToggleSleep, from a short
 * power press) and calls onSleepChanged; here the guest is paused, the screen goes black and the
 * phone may turn its display off. The power button, HOME, any controller button, Android back or
 * a tap on the black screen (the phone's stand-in for the console's buttons) wakes it.
 */
@Keep
object HarborSleep {
    @Volatile
    var sleeping = false
        private set

    private val main = Handler(Looper.getMainLooper())
    private var screen: View? = null

    /** From am, on any thread and with am's locks held: only queued. */
    @JvmStatic
    fun onSleepChanged(asleep: Boolean) {
        main.post { apply(asleep) }
    }

    /** A new or stopped session: awake, with no black screen left behind. */
    fun reset() {
        main.post {
            screen?.let { (it.parent as? ViewGroup)?.removeView(it) }
            screen = null
            sleeping = false
        }
    }

    /** Wakes a sleeping console (a button, back or a tap). */
    fun wake(source: String) {
        if (sleeping) {
            Log.info("[Harbor] wake from sleep ($source)")
            HarborNative.powerShortPress()
        }
    }

    private fun apply(asleep: Boolean) {
        if (asleep == sleeping) {
            return
        }
        sleeping = asleep
        val activity = EmulationActivity.foreground
        if (asleep) {
            Log.info("[Harbor] the console sleeps")
            NativeLibrary.pauseEmulation()
            HarborAudioFocus.release()
            val decor = activity?.window?.decorView as? ViewGroup ?: return
            screen = View(activity).apply {
                setBackgroundColor(Color.BLACK)
                isClickable = true
                setOnClickListener { wake("touch") }
            }
            decor.addView(screen, ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
            // The phone may turn its display off, as a sleeping console's is.
            activity.harborEmulationFragment()?.view?.keepScreenOn = false
        } else {
            Log.info("[Harbor] the console is awake")
            screen?.let { (it.parent as? ViewGroup)?.removeView(it) }
            screen = null
            // Away from Harbor, the emulation screen's own pause and resume take over.
            if (activity != null) {
                activity.harborEmulationFragment()?.view?.keepScreenOn = true
                NativeLibrary.unpauseEmulation()
                HarborAudioFocus.acquire()
            }
        }
    }
}

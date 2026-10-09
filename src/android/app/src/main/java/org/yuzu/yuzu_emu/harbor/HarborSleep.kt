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
 * The console's sleep on the phone. am runs the sleep sequence (HOME answers a short power press,
 * Sleep Mode or auto-sleep) and at its deepest point calls onSleepChanged
 * (AppletManager::OnSleepStateChanged); here the guest is paused, the screen goes black and the
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
    private var dimScreen: View? = null

    /** From am, on any thread and with am's locks held: only queued. */
    @JvmStatic
    fun onSleepChanged(asleep: Boolean) {
        main.post { apply(asleep) }
    }

    /**
     * From idle:sys (AppletManager::OnDimChanged), on any thread and with its lock held: the screen
     * dims after the dimming time and comes back on input. A translucent black layer that lets
     * touches through, so a touch both undims and reaches the console.
     */
    @JvmStatic
    fun onDimChanged(dimmed: Boolean) {
        main.post { applyDim(dimmed) }
    }

    /** A new or stopped session: awake, with no black or dim layer left behind. */
    fun reset() {
        main.post {
            screen?.let { (it.parent as? ViewGroup)?.removeView(it) }
            screen = null
            sleeping = false
            applyDim(false)
        }
    }

    private fun applyDim(dimmed: Boolean) {
        if (!dimmed) {
            dimScreen?.let { (it.parent as? ViewGroup)?.removeView(it) }
            dimScreen = null
            return
        }
        if (dimScreen != null) {
            return
        }
        val activity = EmulationActivity.foreground ?: return
        val decor = activity.window?.decorView as? ViewGroup ?: return
        dimScreen = View(activity).apply {
            // The console dims its LCD by about half (backlight down and luma -0.1).
            setBackgroundColor(Color.argb(128, 0, 0, 0))
            isClickable = false
            isFocusable = false
        }
        decor.addView(dimScreen, ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
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
            applyDim(false)
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

// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

package org.yuzu.yuzu_emu.harbor

import android.app.Activity
import android.content.Intent
import android.media.AudioManager
import android.os.Bundle
import android.view.Gravity
import android.widget.TextView
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import kotlin.concurrent.thread
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.activities.EmulationActivity
import org.yuzu.yuzu_emu.utils.Log

/**
 * Harbor's launcher (docs/harbor/eden-port/host.md section 3.1): a console that is ready boots its
 * HOME menu; one that is not opens the setup. Eden's own onboarding and game list are never shown.
 */
class HarborStartActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // Opened from the launcher while the console runs: back to it, no second boot.
        if (HarborConsole.active && (NativeLibrary.isRunning() || NativeLibrary.isPaused())) {
            Log.info("[Harbor] start: the console is running; returning to it")
            startActivity(
                Intent(this, EmulationActivity::class.java)
                    .addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT or Intent.FLAG_ACTIVITY_NO_ANIMATION)
            )
            finish()
            return
        }
        volumeControlStream = AudioManager.STREAM_MUSIC
        // The console's theme (its saved colour set, the phone's before a first boot), as the setup
        // and the stopped screen use it, so nothing flashes between screens.
        val dark = HarborTheme.dark(this)
        status = TextView(this).apply {
            setBackgroundColor(if (dark) HarborTheme.PAPER_DARK else HarborTheme.PAPER_LIGHT)
            setTextColor(if (dark) HarborTheme.INK_DARK else HarborTheme.INK_LIGHT)
            gravity = Gravity.CENTER
            textSize = 18f
            setText(R.string.harbor_starting)
        }
        setContentView(status)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        WindowInsetsControllerCompat(window, window.decorView).apply {
            hide(WindowInsetsCompat.Type.systemBars())
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }

        HarborNative.initialize()
        val afterSetup = intent.getBooleanExtra(EXTRA_AFTER_SETUP, false)
        // Crash reports: asked once, or last time's report offered, before the console boots.
        HarborCrashReports.onLaunch(this) { thread(name = "HarborStart") {
            val check = HarborNative.checkConsole()
            val ready = check.isEmpty()
            Log.info("[Harbor] start: ready=$ready afterSetup=$afterSetup")
            runOnUiThread {
                if (isFinishing) {
                    return@runOnUiThread
                }
                when {
                    ready -> HarborConsole.boot(this)
                    check == "setup" -> HarborConsole.openSetup(this)
                    else -> {
                        // The check itself failed: "Harbor cannot start" with its message.
                        startActivity(HarborStoppedActivity.cannotStart(this, check))
                        finish()
                    }
                }
            }
        } }
    }

    private lateinit var status: TextView

    /** A one-line state while the console gets ready (the boot connection to OpenPak). */
    fun showStatus(text: Int) {
        status.setText(text)
    }

    companion object {
        const val EXTRA_AFTER_SETUP = "harborAfterSetup"
    }
}

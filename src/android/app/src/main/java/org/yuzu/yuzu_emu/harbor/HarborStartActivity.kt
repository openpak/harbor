// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

package org.yuzu.yuzu_emu.harbor

import android.content.Intent
import android.os.Bundle
import kotlin.concurrent.thread
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.activities.EmulationActivity
import org.yuzu.yuzu_emu.utils.Log

/**
 * Harbor's launcher (docs/harbor/eden-port/host.md section 3.1): a console that is ready boots its
 * HOME menu; one that is not opens the setup. Eden's own onboarding and game list are never shown.
 */
class HarborStartActivity : HarborScreenActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // Opened from the launcher while the console runs: back to it, no second boot.
        if (HarborConsole.active && (NativeLibrary.isRunning() || NativeLibrary.isPaused())) {
            if (EmulationActivity.liveScreens == 0) {
                // Android closed the console's screen while the console kept running: a new screen
                // has no game to show ("No bootable game present!"), so start over in a fresh process.
                Log.info("[Harbor] start: the console runs without its screen; restarting")
                HarborConsole.restartProcess(this)
                finish()
                return
            }
            Log.info("[Harbor] start: the console is running; returning to it")
            startActivity(
                Intent(this, EmulationActivity::class.java)
                    .addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT or Intent.FLAG_ACTIVITY_NO_ANIMATION)
            )
            finish()
            return
        }
        // "Starting…" as a Borealis page (HarborScreenActivity), in the console's theme like the
        // setup and the stopped screen, so nothing flashes between screens.
        showPage(KIND_STATUS, getString(R.string.harbor_starting))

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

    /** A one-line state while the console gets ready (the boot connection to OpenPak). */
    fun showStatus(text: Int) {
        showPage(KIND_STATUS, getString(text))
    }

    /** The status page has nothing to choose; Back leaves Harbor while it gets ready. */
    override fun onHostAction(action: String, argument: String) {
        if (action == "back") {
            HarborConsole.exitHarbor(this)
        }
    }

    companion object {
        const val EXTRA_AFTER_SETUP = "harborAfterSetup"
    }
}

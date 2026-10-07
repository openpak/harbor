// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

package org.yuzu.yuzu_emu.harbor

import android.app.Activity
import android.content.Intent
import android.os.Bundle
import android.os.Process
import android.os.SystemClock
import java.io.File

/**
 * Runs in its own small process (":restart", see YuzuApplication): ends the main process, then starts
 * the setup (or, with EXTRA_CONSOLE, the console) in a new one. A foreground activity may start another; an alarm could not.
 */
class HarborRestartActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val pid = intent.getIntExtra(EXTRA_PID, -1)
        // The next main process carries this one's log forward (YuzuApplication), so the single
        // eden_log.txt shows the setup and the console boot together.
        runCatching { File(filesDir, RESTART_MARKER).writeText(pid.toString()) }
        if (pid > 0 && pid != Process.myPid()) {
            Process.killProcess(pid)
            // Started before the system has seen the old process die, the new activity can be
            // attached to the dying process record and cleared with it: wait until it is gone.
            val deadline = SystemClock.uptimeMillis() + KILL_WAIT_MS
            while (File("/proc/$pid").exists() && SystemClock.uptimeMillis() < deadline) {
                SystemClock.sleep(20)
            }
            SystemClock.sleep(SETTLE_MS)
        }
        val target = if (intent.getBooleanExtra(EXTRA_CONSOLE, false)) {
            HarborStartActivity::class.java
        } else {
            HarborSetupActivity::class.java
        }
        startActivity(Intent(this, target).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK))
        finish()
        Runtime.getRuntime().exit(0)
    }

    companion object {
        const val EXTRA_PID = "harborMainPid"
        /** Start the console (the launcher) rather than the setup. */
        const val EXTRA_CONSOLE = "harborConsole"
        /** In filesDir: the main process was restarted by this helper. */
        const val RESTART_MARKER = "harbor-restarted"
        private const val KILL_WAIT_MS = 3000L
        private const val SETTLE_MS = 250L
    }
}

// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

package org.yuzu.yuzu_emu.harbor

import android.app.Activity
import android.content.Intent
import android.os.Bundle
import android.os.Process

/**
 * Runs in its own small process (":restart", see YuzuApplication): ends the main process, then starts
 * the setup (or, with EXTRA_CONSOLE, the console) in a new one. A foreground activity may start another; an alarm could not.
 */
class HarborRestartActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val pid = intent.getIntExtra(EXTRA_PID, -1)
        if (pid > 0 && pid != Process.myPid()) {
            Process.killProcess(pid)
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
    }
}

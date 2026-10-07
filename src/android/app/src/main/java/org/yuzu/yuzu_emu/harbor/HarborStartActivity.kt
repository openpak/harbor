// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

package org.yuzu.yuzu_emu.harbor

import android.app.Activity
import android.content.res.Configuration
import android.graphics.Color
import android.os.Bundle
import android.view.View
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import kotlin.concurrent.thread
import org.yuzu.yuzu_emu.utils.Log

/**
 * Harbor's launcher (docs/harbor/eden-port/host.md section 3.1): a console that is ready boots its
 * HOME menu; one that is not opens the setup. Eden's own onboarding and game list are never shown.
 */
class HarborStartActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // The paper colour of the setup's current theme, so nothing flashes between screens.
        val dark = resources.configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK ==
            Configuration.UI_MODE_NIGHT_YES
        setContentView(View(this).apply { setBackgroundColor(if (dark) PAPER_DARK else PAPER_LIGHT) })
        WindowCompat.setDecorFitsSystemWindows(window, false)
        WindowInsetsControllerCompat(window, window.decorView).apply {
            hide(WindowInsetsCompat.Type.systemBars())
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }

        HarborNative.initialize()
        val afterSetup = intent.getBooleanExtra(EXTRA_AFTER_SETUP, false)
        thread(name = "HarborStart") {
            val ready = HarborNative.isConsoleReady()
            Log.info("[Harbor] start: ready=$ready afterSetup=$afterSetup")
            runOnUiThread {
                if (isFinishing) {
                    return@runOnUiThread
                }
                if (ready) {
                    HarborConsole.boot(this)
                } else {
                    HarborConsole.openSetup(this)
                }
            }
        }
    }

    companion object {
        const val EXTRA_AFTER_SETUP = "harborAfterSetup"
        private val PAPER_LIGHT = Color.rgb(0xeb, 0xeb, 0xeb)
        private val PAPER_DARK = Color.rgb(0x2d, 0x2d, 0x2d)
    }
}

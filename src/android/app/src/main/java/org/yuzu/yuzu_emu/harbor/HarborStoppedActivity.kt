// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

package org.yuzu.yuzu_emu.harbor

import android.content.Context
import android.content.Intent
import android.media.AudioManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.util.TypedValue
import android.view.Gravity
import android.view.KeyEvent
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.activity.ComponentActivity
import androidx.activity.OnBackPressedCallback
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import java.io.File
import kotlin.concurrent.thread
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.utils.GpuDriverHelper
import org.yuzu.yuzu_emu.utils.Log

/**
 * The console stopped on its own ("Harbor stopped", with the reason, Start again, Graphics driver on
 * Adreno and Set up again), or the launcher's readiness check failed ("Harbor cannot start", with the
 * message only). A stop the user asked for (the Harbor menu) never shows this screen.
 * Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base), App.axaml.cs ShowStopped and
 * ShowDrivers.
 */
class HarborStoppedActivity : ComponentActivity() {
    private lateinit var headingView: TextView
    private lateinit var message: TextView
    private lateinit var actions: LinearLayout
    private var onBack: () -> Unit = { finishAndRemoveTask() }

    /** A driver chosen here loads only in a fresh process. */
    private var driverChanged = false

    private val driverPicker = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) {
            installDriver(uri)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        // The console's saved colour set (the phone's before a first boot), buttons included.
        val dark = HarborTheme.dark(this)
        setTheme(
            if (dark) {
                android.R.style.Theme_DeviceDefault_NoActionBar_Fullscreen
            } else {
                android.R.style.Theme_DeviceDefault_Light_NoActionBar_Fullscreen
            }
        )
        super.onCreate(savedInstanceState)
        volumeControlStream = AudioManager.STREAM_MUSIC
        val ink = if (dark) HarborTheme.INK_DARK else HarborTheme.INK_LIGHT
        headingView = TextView(this).apply {
            setTextColor(ink)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 24f)
            typeface = android.graphics.Typeface.DEFAULT_BOLD
        }
        message = TextView(this).apply {
            setTextColor(ink)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 16f)
            setPadding(0, dp(16), 0, dp(16))
        }
        actions = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        val column = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(24), dp(24), dp(24), dp(24))
            addView(headingView)
            addView(message)
            addView(actions)
        }
        val scroll = ScrollView(this).apply {
            setBackgroundColor(if (dark) HarborTheme.PAPER_DARK else HarborTheme.PAPER_LIGHT)
            isFillViewport = true
            addView(
                FrameLayout(this@HarborStoppedActivity).apply {
                    addView(
                        column,
                        FrameLayout.LayoutParams(
                            minOf(dp(MAX_WIDTH_DP), resources.displayMetrics.widthPixels),
                            ViewGroup.LayoutParams.WRAP_CONTENT,
                            Gravity.CENTER
                        )
                    )
                }
            )
        }
        setContentView(scroll)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        WindowInsetsControllerCompat(window, window.decorView).apply {
            hide(WindowInsetsCompat.Type.systemBars())
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
        HarborPanelInput.attach(window)
        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() = onBack()
        })

        val reason = intent.getStringExtra(EXTRA_REASON).orEmpty()
        if (intent.getBooleanExtra(EXTRA_DRIVER_FAILED, false)) {
            HarborNative.initialize()
            Log.info("[Harbor] driver choice shown after a driver failure")
            showDriverFailed(reason, null)
        } else if (intent.getBooleanExtra(EXTRA_CANNOT_START, false)) {
            Log.info("[Harbor] cannot start screen shown")
            show(getString(R.string.harbor_cannot_start), reason, emptyList())
        } else {
            Log.info("[Harbor] stopped screen shown: $reason")
            showStopped(reason)
        }
    }

    // A pad's B (and Minus) arrive as a BACK key from HarborPanelInput, past the system's back dispatch.
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (event.keyCode == KeyEvent.KEYCODE_BACK) {
            if (event.action == KeyEvent.ACTION_UP && !event.isCanceled) {
                onBack()
            }
            return true
        }
        return super.dispatchKeyEvent(event)
    }

    private fun showStopped(reason: String) {
        val list = mutableListOf<Pair<String, () -> Unit>>(getString(R.string.harbor_start_again) to ::startAgain)
        if (GpuDriverHelper.isAdrenoGpu()) {
            list.add(getString(R.string.harbor_menu_driver) to { showDrivers(null) })
        }
        list.add(getString(R.string.harbor_menu_setup_again) to {
            HarborNative.stopOpenPak()
            HarborConsole.restartIntoSetup(this)
        })
        show(getString(R.string.harbor_stopped), reason, list)
        onBack = { finishAndRemoveTask() }
    }

    /**
     * The graphics driver page: the choice is saved at once and used from the console's next start;
     * installing a driver also chooses it.
     */
    private fun showDrivers(status: String?) {
        val flat = HarborSetupBridge.installedDrivers()
        val chosen = HarborSetupBridge.currentDriver()
        fun mark(id: String, label: String) =
            if (id == chosen) getString(R.string.harbor_driver_chosen, label) else label
        val list = mutableListOf<Pair<String, () -> Unit>>()
        list.add(mark("", getString(R.string.harbor_driver_system)) to { choose("") })
        for (i in 0 until flat.size / 3) {
            val id = flat[i * 3]
            val label = if (flat[i * 3 + 2].isEmpty()) flat[i * 3 + 1] else "${flat[i * 3 + 1]} (${flat[i * 3 + 2]})"
            list.add(mark(id, label) to { choose(id) })
        }
        list.add(getString(R.string.harbor_driver_install) to { driverPicker.launch(arrayOf("*/*")) })
        list.add(getString(R.string.harbor_start_again) to ::startAgain)
        list.add(getString(R.string.harbor_back) to { showStopped(intent.getStringExtra(EXTRA_REASON).orEmpty()) })
        show(getString(R.string.harbor_menu_driver), status ?: getString(R.string.harbor_driver_lead), list)
        onBack = { showStopped(intent.getStringExtra(EXTRA_REASON).orEmpty()) }
    }

    /**
     * The chosen driver did not load: never the phone's driver silently. The installed drivers that
     * fit this GPU, the recommended one downloaded (T30 on an Adreno 6xx), or the phone's own after
     * a warning; the choice restarts Harbor into it.
     */
    private fun showDriverFailed(problem: String, status: String?) {
        val current = HarborSetupBridge.currentDriver()
        val list = mutableListOf<Pair<String, () -> Unit>>()
        for ((path, meta) in GpuDriverHelper.getDrivers()) {
            val id = File(path).name
            val library = meta.libraryName ?: continue
            if (id == current || HarborNative.driverProblem(meta.name ?: "", library, meta.minApi).isNotEmpty()) {
                continue
            }
            val label = listOfNotNull(meta.name ?: File(path).nameWithoutExtension, meta.version)
                .filter { it.isNotBlank() }.joinToString(" ")
            list.add(label to { restartWithDriver(id, problem) })
        }
        if (HarborNative.hasRecommendedDriver()) {
            list.add(getString(R.string.harbor_driver_download_recommended) to { downloadRecommended(problem) })
        }
        list.add(getString(R.string.harbor_driver_use_phone) to { confirmPhoneDriver(problem) })
        list.add(getString(R.string.harbor_menu_setup_again) to {
            HarborNative.stopOpenPak()
            HarborConsole.restartIntoSetup(this)
        })
        list.add(getString(R.string.harbor_menu_exit) to {
            HarborNative.stopOpenPak()
            finishAndRemoveTask()
        })
        val text = (status?.let { it + "\n\n" } ?: "") + problem + "\n\n" + getString(R.string.harbor_driver_failed_choose)
        show(getString(R.string.harbor_driver_load_failed), text, list)
        onBack = { finishAndRemoveTask() }
    }

    private fun confirmPhoneDriver(problem: String) {
        show(
            getString(R.string.harbor_driver_use_phone),
            getString(R.string.harbor_driver_phone_warning),
            listOf(
                getString(R.string.harbor_driver_use_phone) to { restartWithDriver("", problem) },
                getString(R.string.harbor_back) to { showDriverFailed(problem, null) }
            )
        )
        onBack = { showDriverFailed(problem, null) }
    }

    private fun restartWithDriver(id: String, problem: String) {
        if (!HarborSetupBridge.selectDriver(id)) {
            showDriverFailed(problem, getString(R.string.harbor_driver_not_saved))
            return
        }
        Log.info("[Harbor] driver failure: restarting with ${if (id.isEmpty()) "the phone's driver" else id}")
        HarborConsole.restartProcess(this)
    }

    private fun downloadRecommended(problem: String) {
        show(getString(R.string.harbor_driver_load_failed), getString(R.string.harbor_driver_downloading), emptyList())
        onBack = {}
        thread(name = "HarborRecommendedDriver") {
            val result = try {
                HarborNative.installRecommendedDriver()
            } catch (e: Exception) {
                arrayOf("", "", e.message ?: e.javaClass.simpleName)
            }
            runOnUiThread {
                if (isFinishing) return@runOnUiThread
                if (result.size >= 3 && result[2].isEmpty() && result[0].isNotEmpty()) {
                    Log.info("[Harbor] driver failure: restarting with the recommended ${result[0]}")
                    HarborConsole.restartProcess(this)
                } else {
                    showDriverFailed(problem, result.getOrNull(2) ?: getString(R.string.harbor_driver_not_saved))
                }
            }
        }
    }

    private fun choose(id: String, saved: String? = null) {
        val message = if (id == HarborSetupBridge.currentDriver() && saved == null) {
            getString(R.string.harbor_driver_saved)
        } else if (HarborSetupBridge.selectDriver(id)) {
            driverChanged = true
            saved ?: getString(R.string.harbor_driver_saved)
        } else {
            getString(R.string.harbor_driver_not_saved)
        }
        showDrivers(message)
    }

    private fun installDriver(uri: Uri) {
        show(getString(R.string.harbor_menu_driver), getString(R.string.harbor_driver_installing), emptyList())
        onBack = {}
        thread(name = "HarborDriverInstall") {
            val result = try {
                val fields = HarborSetupBridge.inspectDriver(uri.toString())
                when {
                    fields[6].isNotEmpty() -> Result(null, fields[6])
                    fields[5].toIntOrNull()?.let { it > Build.VERSION.SDK_INT } == true -> {
                        HarborSetupBridge.discardDriver(fields[0])
                        Result(null, getString(R.string.harbor_driver_api, fields[1], fields[5], Build.VERSION.SDK_INT))
                    }
                    else -> {
                        val installed = HarborSetupBridge.commitDriver(fields[0])
                        if (installed[3].isNotEmpty()) Result(null, installed[3]) else Result(installed[0], installed[1])
                    }
                }
            } catch (e: Exception) {
                Result(null, getString(R.string.harbor_driver_install_failed, e.message ?: e.javaClass.simpleName))
            }
            runOnUiThread {
                if (isFinishing) return@runOnUiThread
                if (result.id != null) {
                    choose(result.id, getString(R.string.harbor_driver_installed, result.text))
                } else {
                    showDrivers(result.text)
                }
            }
        }
    }

    private data class Result(val id: String?, val text: String)

    private fun startAgain() {
        Log.info("[Harbor] start again from the stopped screen${if (driverChanged) " (new graphics driver: new process)" else ""}")
        if (driverChanged) {
            HarborConsole.restartProcess(this)
            return
        }
        startActivity(
            Intent(this, HarborStartActivity::class.java)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK)
                .addFlags(Intent.FLAG_ACTIVITY_NO_ANIMATION)
        )
        finish()
    }

    private fun show(heading: String, text: String, list: List<Pair<String, () -> Unit>>) {
        headingView.text = heading
        message.text = text
        message.visibility = if (text.isEmpty()) View.GONE else View.VISIBLE
        actions.removeAllViews()
        for ((label, click) in list) {
            actions.addView(
                Button(this).apply {
                    this.text = label
                    isAllCaps = false
                    minHeight = dp(52)
                    setOnClickListener { click() }
                },
                LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
                    .apply { bottomMargin = dp(8) }
            )
        }
        actions.getChildAt(0)?.requestFocus()
    }

    private fun dp(value: Int): Int = (value * resources.displayMetrics.density).toInt()

    companion object {
        private const val EXTRA_REASON = "harborStopReason"
        private const val EXTRA_CANNOT_START = "harborCannotStart"
        private const val EXTRA_DRIVER_FAILED = "harborDriverFailed"
        private const val MAX_WIDTH_DP = 560

        /** "Harbor stopped" with `reason`; the caller finishes its own screen. */
        fun stopped(context: Context, reason: String): Intent =
            Intent(context, HarborStoppedActivity::class.java)
                .putExtra(EXTRA_REASON, reason)
                .addFlags(Intent.FLAG_ACTIVITY_NO_ANIMATION)

        /** The chosen graphics driver did not load: the driver choice, with why. */
        fun driverFailed(context: Context, problem: String): Intent =
            Intent(context, HarborStoppedActivity::class.java)
                .putExtra(EXTRA_REASON, problem)
                .putExtra(EXTRA_DRIVER_FAILED, true)
                .addFlags(Intent.FLAG_ACTIVITY_NO_ANIMATION)

        /** "Harbor cannot start" with the readiness check's message. */
        fun cannotStart(context: Context, message: String): Intent =
            Intent(context, HarborStoppedActivity::class.java)
                .putExtra(EXTRA_REASON, message)
                .putExtra(EXTRA_CANNOT_START, true)
                .addFlags(Intent.FLAG_ACTIVITY_NO_ANIMATION)
    }
}

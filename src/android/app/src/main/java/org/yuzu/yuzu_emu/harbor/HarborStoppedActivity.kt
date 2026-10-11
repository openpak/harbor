// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

package org.yuzu.yuzu_emu.harbor

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import androidx.activity.result.contract.ActivityResultContracts
import java.io.File
import kotlin.concurrent.thread
import org.yuzu.yuzu_emu.utils.GpuDriverHelper
import org.yuzu.yuzu_emu.utils.Log

/**
 * The console stopped on its own ("Harbor stopped", with the reason: Start again, Graphics driver on
 * Adreno, Set up again, Exit), the launcher's readiness check failed ("Harbor cannot start"), or the
 * chosen graphics driver did not load (the installed drivers that fit, the recommended one
 * downloaded, the phone's own after a warning). Borealis pages of the Harbor menu
 * (harbor_setup/menu_view.cpp, HostBody), the same as the desktop's; this activity carries out what
 * they ask for. A stop the user asked for (the Harbor menu) never shows this screen.
 * Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base), App.axaml.cs ShowStopped and
 * ShowDrivers.
 */
class HarborStoppedActivity : HarborScreenActivity() {
    /** A driver chosen here loads only in a fresh process. */
    private var driverChanged = false

    private val driverPicker = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) {
            installDriver(uri)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        HarborNative.initialize()
        val reason = intent.getStringExtra(EXTRA_REASON).orEmpty()
        when {
            intent.getBooleanExtra(EXTRA_DRIVER_FAILED, false) -> {
                Log.info("[Harbor] driver choice shown after a driver failure")
                showPage(KIND_DRIVER_FAILED, reason)
            }
            intent.getBooleanExtra(EXTRA_CANNOT_START, false) -> {
                Log.info("[Harbor] cannot start screen shown")
                showPage(KIND_CANNOT_START, reason)
            }
            else -> {
                Log.info("[Harbor] stopped screen shown: $reason")
                showPage(KIND_STOPPED, reason)
            }
        }
    }

    override fun onHostAction(action: String, argument: String) {
        when (action) {
            "start-again" -> startAgain()
            // Ends this process's OpenPak session itself before the new process starts.
            "set-up-again" -> HarborConsole.restartIntoSetup(this)
            "exit", "back" -> HarborConsole.exitHarbor(this)
            "driver-changed" -> driverChanged = true
            "restart-with" -> restartWithDriver(argument)
            "install-file" -> driverPicker.launch(arrayOf("*/*"))
        }
    }

    /** After a driver failure: a driver is chosen and Harbor restarts into it. */
    private fun restartWithDriver(id: String) {
        if (!HarborSetupBridge.selectDriver(id)) {
            HarborNative.hostPageNotice("The choice could not be saved.", true)
            return
        }
        Log.info("[Harbor] driver failure: restarting with ${if (id.isEmpty()) "the phone's driver" else id}")
        HarborConsole.restartProcess(this)
    }

    /** A picked driver file: read, checked and installed as the setup does it, then chosen. */
    private fun installDriver(uri: Uri) {
        HarborNative.hostPageNotice("Installing the driver…", false)
        thread(name = "HarborDriverInstall") {
            val result = try {
                HarborNative.installDriverFile(uri.toString(), uri.lastPathSegment ?: "driver.zip")
            } catch (e: Exception) {
                arrayOf("", "", e.message ?: e.javaClass.simpleName)
            }
            runOnUiThread {
                if (result.size >= 3 && result[2].isEmpty() && result[0].isNotEmpty()) {
                    driverChanged = true
                    Log.info("[Harbor] stopped screen: driver file ${result[0]} installed and chosen")
                    HarborNative.hostPageNotice(
                        "${result[1]} is installed. It is chosen; the console uses it from its next start.",
                        false
                    )
                } else {
                    HarborNative.hostPageNotice(
                        "The driver could not be installed: ${result.getOrNull(2) ?: "unknown error"}",
                        true
                    )
                }
            }
        }
    }

    private fun startAgain() {
        Log.info("[Harbor] start again from the stopped screen${if (driverChanged) " (new graphics driver: new process)" else ""}")
        if (driverChanged) {
            HarborConsole.restartProcess(this)
            return
        }
        // The stopped console's session ends before the launcher signs in again.
        HarborConsole.endOpenPakSession()
        startActivity(
            Intent(this, HarborStartActivity::class.java)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK)
                .addFlags(Intent.FLAG_ACTIVITY_NO_ANIMATION)
        )
        finish()
    }

    companion object {
        private const val EXTRA_REASON = "harborStopReason"
        private const val EXTRA_CANNOT_START = "harborCannotStart"
        private const val EXTRA_DRIVER_FAILED = "harborDriverFailed"

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

        /**
         * After a driver failure: the installed drivers that fit this GPU other than the chosen one,
         * as lines of id and label (HarborMenuBridge "fittingDrivers").
         */
        fun fittingDrivers(): String {
            val current = HarborSetupBridge.currentDriver()
            val lines = mutableListOf<String>()
            for ((path, meta) in GpuDriverHelper.getDrivers()) {
                val id = File(path).name
                val library = meta.libraryName ?: continue
                if (id == current || HarborNative.driverProblem(meta.name ?: "", library, meta.minApi).isNotEmpty()) {
                    continue
                }
                val label = listOfNotNull(meta.name ?: File(path).nameWithoutExtension, meta.version)
                    .filter { it.isNotBlank() }.joinToString(" ")
                lines.add(id.replace('\n', ' '))
                lines.add(label.replace('\n', ' '))
            }
            return lines.joinToString("\n") + "\n"
        }
    }
}

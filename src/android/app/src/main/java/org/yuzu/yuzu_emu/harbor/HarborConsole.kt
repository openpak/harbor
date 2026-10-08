// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

package org.yuzu.yuzu_emu.harbor

import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.net.Uri
import android.widget.Toast
import android.os.Process
import androidx.drawerlayout.widget.DrawerLayout
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import kotlin.concurrent.thread
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.activities.EmulationActivity
import org.yuzu.yuzu_emu.features.settings.model.IntSetting
import org.yuzu.yuzu_emu.features.settings.model.Settings
import org.yuzu.yuzu_emu.model.AppletInfo
import org.yuzu.yuzu_emu.model.Game
import org.yuzu.yuzu_emu.model.MinimalDocumentFile
import org.yuzu.yuzu_emu.utils.FileUtil
import org.yuzu.yuzu_emu.utils.GameHelper
import org.yuzu.yuzu_emu.utils.GpuDriverHelper
import org.yuzu.yuzu_emu.features.settings.model.StringSetting
import java.io.File
import org.yuzu.yuzu_emu.utils.InputHandler
import org.yuzu.yuzu_emu.utils.LosslessScalingHelper
import org.yuzu.yuzu_emu.utils.Log
import org.yuzu.yuzu_emu.utils.NativeConfig

/**
 * Console mode (docs/harbor/eden-port/host.md section 3.1): the HOME menu boots straight away with the
 * games registered behind it, and the only host UI is the Harbor menu (Android back, or Plus and Minus
 * together on a controller).
 */
object HarborConsole {
    /** This process boots the console rather than a game from Eden's list. */
    @Volatile
    var active = false
        private set

    /** Registers the library, applies the console preset and starts the HOME menu in place of `from`. */
    fun boot(from: Activity) {
        active = true
        HarborNative.setConsoleMode(true)
        // What Eden's main screen sets up before any game: the multiplayer/network layer.
        NativeLibrary.initMultiplayer()
        thread(name = "HarborBoot") {
            if (HarborSetupBridge.ran) {
                // Keys or firmware changed in this process: the core reads its NAND again with them.
                NativeLibrary.initializeSystem(true)
            }
            HarborNative.applyConsolePreset()
            Log.info("[Harbor] console start: graphics driver ${ensureDriver()}")
            // The driver loads now, so a selected one that failed stops the boot here, not in HOME.
            val driverProblem = HarborNative.checkGpuDriver()
            if (driverProblem.isNotEmpty()) {
                Log.error("[Harbor] console not started: $driverProblem")
                from.runOnUiThread { showDriverRefusal(from, driverProblem) }
                return@thread
            }
            // The in-game drawer stays shut; the Harbor menu replaces it.
            IntSetting.LOCK_DRAWER.setInt(DrawerLayout.LOCK_MODE_LOCKED_CLOSED)
            IntSetting.RENDERER_SCREEN_LAYOUT.setInt(Settings.EmulationOrientation.SensorLandscape.int)
            NativeConfig.saveGlobalConfig()

            // What qlaunch and ns see as installed: every game, DLC and update under the setup's folders,
            // registered the way Eden's game list does it, without showing the list.
            val games = try {
                GameHelper.getGames().size
            } catch (e: Exception) {
                Log.warning("[Harbor] library scan failed: ${e.message}")
                -1
            }
            val addons = try {
                registerAddons()
            } catch (e: Exception) {
                Log.warning("[Harbor] add-on scan failed: ${e.message}")
                -1
            }
            // OpenPak before HOME, with its credentials in the Android Keystore.
            (from as? HarborStartActivity)?.let { start ->
                start.runOnUiThread { start.showStatus(R.string.harbor_connecting) }
            }
            HarborNative.connectOpenPak()
            // The Harbor menu and the mapped Power button, for this console session.
            HarborNative.initMenu()
            val qlaunch = NativeLibrary.getAppletLaunchPath(AppletInfo.QLaunch.entryId)
            Log.info("[Harbor] console boot: $games titles and $addons add-on files registered, HOME menu ${if (qlaunch.isEmpty()) "missing" else "present"}")
            from.runOnUiThread {
                if (qlaunch.isEmpty()) {
                    openSetup(from)
                    return@runOnUiThread
                }
                // RunEmulation resets the applet id when a session ends, so it is set on every boot.
                NativeLibrary.setCurrentAppletId(AppletInfo.QLaunch.appletId)
                val home = Game(title = from.getString(R.string.harbor_home_title), path = qlaunch)
                from.startActivity(
                    Intent(from, EmulationActivity::class.java)
                        .putExtra("game", home)
                        .addFlags(Intent.FLAG_ACTIVITY_NO_ANIMATION)
                )
                @Suppress("DEPRECATION")
                from.overridePendingTransition(0, 0)
                from.finish()
            }
        }
    }

    /**
     * The setup's DLC and update folders are SAF tree URIs, which core's file system cannot walk, so
     * their .nsp/.xci files are registered here, the way Eden's game list registers games (after
     * GameHelper.getGames, which clears the registrations first). Returns how many were registered.
     */
    private fun registerAddons(): Int {
        var count = 0
        fun walk(files: Array<MinimalDocumentFile>, depth: Int) {
            if (depth <= 0) {
                return
            }
            for (file in files) {
                if (file.isDirectory) {
                    walk(FileUtil.listFiles(file.uri), depth - 1)
                } else if (FileUtil.getExtension(file.uri).lowercase() in ADDON_EXTENSIONS) {
                    NativeLibrary.addFileToFilesystemProvider(file.uri.toString())
                    count++
                }
            }
        }
        for (folder in HarborNative.addonFolders().distinct()) {
            val uri = Uri.parse(folder)
            if (folder.isEmpty() || !FileUtil.isTreeUriValid(uri)) {
                Log.warning("[Harbor] an add-on folder is no longer readable; set it again in the setup")
                continue
            }
            walk(FileUtil.listFiles(uri), ADDON_SCAN_DEPTH)
        }
        return count
    }

    private val ADDON_EXTENSIONS = setOf("nsp", "xci")
    private const val ADDON_SCAN_DEPTH = 3

    /**
     * The chosen graphics driver unpacked where Eden loads it from (as Eden's game launch does), so
     * HOME runs with it. Returns its name for the log.
     */
    private fun ensureDriver(): String {
        // Only an Adreno GPU loads another driver: elsewhere a saved choice is ignored (G4.GPU.1).
        if (!GpuDriverHelper.isAdrenoGpu()) {
            if (GpuDriverHelper.installedCustomDriverData.name != null) {
                GpuDriverHelper.installDefaultDriver()
            }
            return "the phone's (not an Adreno GPU)"
        }
        val chosen = GpuDriverHelper.customDriverSettingData
        if (GpuDriverHelper.installedCustomDriverData != chosen) {
            val path = StringSetting.DRIVER_PATH.getString()
            if (chosen.name == null || !File(path).exists()) {
                GpuDriverHelper.installDefaultDriver()
            } else if (!GpuDriverHelper.installCustomDriver(File(path))) {
                Log.warning("[Harbor] the chosen graphics driver could not be unpacked")
            }
        }
        val installed = GpuDriverHelper.installedCustomDriverData
        return installed.name?.let { "$it ${installed.version ?: ""}".trim() } ?: "the phone's"
    }

    /**
     * The selected graphics driver did not load (EmulationSession::CheckGpuDriver): the console does
     * not boot on the phone's own driver instead. The setup chooses another; Exit closes Harbor.
     */
    fun showDriverRefusal(from: Activity, problem: String) {
        if (from.isFinishing) {
            return
        }
        val dialog = AlertDialog.Builder(from, HarborTheme.alertTheme(from))
            .setTitle(R.string.harbor_driver_failed_title)
            .setMessage(problem + "\n\n" + from.getString(R.string.harbor_driver_failed_hint))
            .setCancelable(false)
            .setPositiveButton(R.string.harbor_menu_setup_again) { _, _ ->
                HarborNative.stopOpenPak()
                restartIntoSetup(from)
            }
            .setNegativeButton(R.string.harbor_menu_exit) { _, _ ->
                HarborNative.stopOpenPak()
                from.finishAndRemoveTask()
            }
            .show()
        HarborPanelInput.attach(dialog)
    }

    /** Opens the setup wizard: directly the first time in a process, else in a new process. */
    fun openSetup(from: Activity) {
        if (HarborSetupBridge.ran) {
            restartIntoSetup(from)
            return
        }
        from.startActivity(
            Intent(from, HarborSetupActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NO_ANIMATION)
        )
        @Suppress("DEPRECATION")
        from.overridePendingTransition(0, 0)
        from.finish()
    }

    /**
     * The console stopped on its own (not through the Harbor menu). HOME's power menu: a restart boots
     * the console again through the launcher (library scan, OpenPak, HOME), turning it off closes
     * Harbor; anything else is the "Harbor stopped" screen.
     */
    fun onConsoleStopped(activity: EmulationActivity) {
        when (HarborNative.powerRequest()) {
            POWER_REBOOT -> {
                Log.info("[Harbor] the console restarts")
                restartConsole(activity)
            }
            POWER_OFF -> {
                Log.info("[Harbor] the console turned off")
                HarborNative.stopOpenPak()
                activity.finishAndRemoveTask()
            }
            else -> showStopped(activity, activity.getString(R.string.harbor_stop_root_exited))
        }
    }

    /** "Harbor stopped" with `reason` in place of `activity` (the emulation screen). */
    fun showStopped(activity: Activity, reason: String) {
        Log.info("[Harbor] the console stopped: $reason")
        HarborAudioFocus.release()
        activity.startActivity(HarborStoppedActivity.stopped(activity, reason))
        @Suppress("DEPRECATION")
        activity.overridePendingTransition(0, 0)
        activity.finish()
    }

    private const val POWER_OFF = 1
    private const val POWER_REBOOT = 2

    /** Boots the console again through the launcher: library scan, OpenPak, HOME. */
    fun restartConsole(activity: Activity) {
        activity.startActivity(
            Intent(activity, HarborStartActivity::class.java)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK)
                .addFlags(Intent.FLAG_ACTIVITY_NO_ANIMATION)
        )
        activity.finish()
    }

    /** A picked Lossless.dll: installed with the console stopped, as Eden requires, then HOME again. */
    fun installLosslessDll(activity: EmulationActivity, uri: Uri) {
        changeLosslessDll(activity) {
            val result = LosslessScalingHelper.install(uri)
            if (result == LosslessScalingHelper.RESULT_OK) {
                activity.getString(R.string.lossless_scaling_install_success)
            } else {
                activity.getString(R.string.lossless_scaling_install_failed) + ": " +
                    activity.resources.getStringArray(R.array.losslessDllResults)[result]
            }
        }
    }

    /** Lossless.dll cannot change while the renderer uses it: stop, change it off the UI thread, restart. */
    internal fun changeLosslessDll(activity: EmulationActivity, change: () -> String?) {
        activity.harborStop {
            val progress = MaterialAlertDialogBuilder(HarborTheme.dialogContext(activity))
                .setMessage(R.string.lossless_scaling_installing)
                .setCancelable(false)
                .show()
            thread(name = "HarborLossless") {
                val message = try {
                    change()
                } catch (e: Exception) {
                    Log.warning("[Harbor] Lossless.dll change failed: ${e.message}")
                    activity.getString(R.string.lossless_scaling_install_failed)
                }
                activity.runOnUiThread {
                    progress.dismiss()
                    message?.let { Toast.makeText(activity, it, Toast.LENGTH_LONG).show() }
                    restartConsole(activity)
                }
            }
        }
    }

    /**
     * The Harbor menu (Android back, Plus and Minus together, or the on-screen button): drawn by
     * Borealis onto the console's own screen (harbor_setup/menu.h); what it asks for comes back
     * through HarborMenuBridge. Physical input is released first so nothing stays held in the guest.
     */
    fun showMenu(activity: EmulationActivity, trigger: String) {
        if (activity.isFinishing) {
            Log.info("[Harbor] menu asked for by $trigger: the screen is closing")
            return
        }
        if (HarborNative.menuOpen()) {
            // Android back is the menu's own Back; Plus+Minus is handled by the menu itself.
            when (trigger) {
                "Android back" -> HarborNative.menuBack()
                "on-screen button" -> HarborNative.closeMenu()
            }
            return
        }
        Log.info("[Harbor] menu opened by $trigger")
        InputHandler.releaseAll()
        activity.harborResetChord()
        HarborNative.openMenu(MENU_PAGE_MENU)
    }

    private const val MENU_PAGE_MENU = 0

    /**
     * The setup finished: the console boots in a fresh process, as at any cold start. The wizard's
     * SDL and Borealis are still winding down in this one, and the core read the NAND before the
     * firmware and keys were installed. The helper process ends this one.
     */
    fun bootAfterSetup(from: Activity) {
        Log.info("[Harbor] the setup finished; starting the console in a new process")
        from.startActivity(
            Intent(from, HarborRestartActivity::class.java)
                .putExtra(HarborRestartActivity.EXTRA_PID, Process.myPid())
                .putExtra(HarborRestartActivity.EXTRA_CONSOLE, true)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_NO_ANIMATION)
        )
    }

    /** Harbor in a fresh process, straight back to the console (a new graphics driver loads then). */
    fun restartProcess(from: Activity) {
        Log.info("[Harbor] restarting the process")
        from.startActivity(
            Intent(from, HarborRestartActivity::class.java)
                .putExtra(HarborRestartActivity.EXTRA_PID, Process.myPid())
                .putExtra(HarborRestartActivity.EXTRA_CONSOLE, true)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        )
        from.finishAffinity()
        Runtime.getRuntime().exit(0)
    }

    /**
     * Starts the setup in a fresh process: Borealis and SDL can run the wizard once per process. A tiny
     * activity in its own process starts the setup after this process is gone.
     */
    fun restartIntoSetup(from: Activity) {
        Log.info("[Harbor] restarting into the setup")
        from.startActivity(
            Intent(from, HarborRestartActivity::class.java)
                .putExtra(HarborRestartActivity.EXTRA_PID, Process.myPid())
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        )
        from.finishAffinity()
        Runtime.getRuntime().exit(0)
    }

    /**
     * The Harbor menu's Show FPS, kept by Harbor itself: off unless chosen there, whatever Eden's
     * own default or saved setting for its performance overlay says.
     */
    var showFps: Boolean
        get() = preferences().getBoolean(PREF_SHOW_FPS, false)
        set(value) = preferences().edit().putBoolean(PREF_SHOW_FPS, value).apply()

    private fun preferences() = org.yuzu.yuzu_emu.YuzuApplication.appContext
        .getSharedPreferences("harbor", android.content.Context.MODE_PRIVATE)

    private const val PREF_SHOW_FPS = "show_fps"

    /** Harbor Settings: the device and its driver on a line under the FPS (off unless chosen). */
    var showDeviceLine: Boolean
        get() = preferences().getBoolean(PREF_DEVICE_LINE, false)
        set(value) = preferences().edit().putBoolean(PREF_DEVICE_LINE, value).apply()

    private const val PREF_DEVICE_LINE = "show_device_line"

    /** The touch controls hidden by the user's choice in the Harbor menu (not by a controller). */
    var padHidden: Boolean
        get() = preferences().getBoolean(PREF_PAD_HIDDEN, false)
        set(value) = preferences().edit().putBoolean(PREF_PAD_HIDDEN, value).apply()

    private const val PREF_PAD_HIDDEN = "pad_hidden"

    /** "Stopping the console…" while the session stops, then `next` (Restart, Set up again). */
    internal fun stopThen(activity: EmulationActivity, next: () -> Unit) {
        val notice = MaterialAlertDialogBuilder(HarborTheme.dialogContext(activity))
            .setMessage(R.string.harbor_stopping)
            .setCancelable(false)
            .show()
        activity.harborStop {
            if (notice.isShowing) notice.dismiss()
            next()
        }
    }

}

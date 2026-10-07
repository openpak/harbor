// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

package org.yuzu.yuzu_emu.harbor

import android.app.Activity
import android.content.Intent
import android.net.Uri
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.Toast
import android.os.Process
import androidx.drawerlayout.widget.DrawerLayout
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import kotlin.concurrent.thread
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.activities.EmulationActivity
import org.yuzu.yuzu_emu.dialogs.QuickSettings
import org.yuzu.yuzu_emu.features.settings.model.BooleanSetting
import org.yuzu.yuzu_emu.features.settings.model.IntSetting
import org.yuzu.yuzu_emu.features.settings.model.Settings
import org.yuzu.yuzu_emu.model.AppletInfo
import org.yuzu.yuzu_emu.model.Game
import org.yuzu.yuzu_emu.model.MinimalDocumentFile
import org.yuzu.yuzu_emu.utils.FileUtil
import org.yuzu.yuzu_emu.utils.GameHelper
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
     * Harbor; anything else leaves the emulation screen as Eden does.
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
            else -> activity.finish()
        }
    }

    private const val POWER_OFF = 1
    private const val POWER_REBOOT = 2

    /** Boots the console again through the launcher: library scan, OpenPak, HOME. */
    private fun restartConsole(activity: Activity) {
        activity.startActivity(
            Intent(activity, HarborStartActivity::class.java)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK)
                .addFlags(Intent.FLAG_ACTIVITY_NO_ANIMATION)
        )
        activity.finish()
    }

    /**
     * Frame generation (Lossless Scaling, Eden's LSFG): the user's own Lossless.dll, then the on/off and
     * mode settings Eden's quick settings had. Off by default; the settings are console-wide and apply to
     * whatever is presented, HOME or a title launched from it.
     */
    fun showFrameGeneration(activity: EmulationActivity) {
        if (!LosslessScalingHelper.isInstalled()) {
            activity.harborLosslessPicker.launch(arrayOf("*/*"))
            return
        }
        val fragment = activity.harborEmulationFragment() ?: return
        val container = LinearLayout(activity).apply { orientation = LinearLayout.VERTICAL }
        QuickSettings(fragment).addFrameGen(container)
        val dialog = MaterialAlertDialogBuilder(activity)
            .setTitle(R.string.frame_gen)
            .setView(ScrollView(activity).apply { addView(container) })
            .setPositiveButton(R.string.harbor_close, null)
            .setNeutralButton(R.string.lossless_scaling_replace) { _, _ ->
                activity.harborLosslessPicker.launch(arrayOf("*/*"))
            }
            .setNegativeButton(R.string.lossless_scaling_remove) { _, _ ->
                val confirm = MaterialAlertDialogBuilder(activity)
                    .setTitle(R.string.lossless_scaling_remove)
                    .setMessage(
                        activity.getString(R.string.lossless_scaling_remove_confirmation) + "\n\n" +
                            activity.getString(R.string.harbor_frame_gen_restart)
                    )
                    .setPositiveButton(R.string.lossless_scaling_remove) { _, _ ->
                        changeLosslessDll(activity) { LosslessScalingHelper.remove(); null }
                    }
                    .setNegativeButton(android.R.string.cancel, null)
                    .show()
                HarborPanelInput.attach(confirm)
            }
            .show()
        HarborPanelInput.attach(dialog)
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
    private fun changeLosslessDll(activity: EmulationActivity, change: () -> String?) {
        activity.harborStop {
            val progress = MaterialAlertDialogBuilder(activity)
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
     * The Harbor menu (Android back, or Plus and Minus together): Resume, Detach/Attach Joy-Con,
     * Restart, Graphics driver, Show/Hide FPS, Log threads, Frame generation (on GPUs that can run
     * it), Set up again, Exit. Physical input is released first so nothing stays held in the guest.
     */
    fun showMenu(activity: EmulationActivity) {
        if (activity.isFinishing || menuOpen) {
            return
        }
        menuOpen = true
        InputHandler.releaseAll()
        val fps = BooleanSetting.SHOW_PERFORMANCE_OVERLAY.getBoolean()
        val items = buildList<Pair<Int, () -> Unit>> {
            add(R.string.harbor_menu_resume to {})
            add(
                (if (HarborNative.joyconsDetached()) R.string.harbor_menu_attach_joycons else R.string.harbor_menu_detach_joycons) to {
                    HarborNative.setJoyconsDetached(!HarborNative.joyconsDetached())
                }
            )
            add(R.string.harbor_menu_restart to { activity.harborStop { restartConsole(activity) } })
            add(R.string.harbor_menu_driver to { showDriverPicker(activity) })
            add(
                (if (fps) R.string.harbor_menu_hide_fps else R.string.harbor_menu_show_fps) to {
                    activity.harborEmulationFragment()?.harborShowStats(!fps)
                }
            )
            add(R.string.harbor_menu_log_threads to { HarborNative.logGuestThreads() })
            if (LosslessScalingHelper.isSupportedByGpu()) {
                add(R.string.harbor_menu_frame_gen to { showFrameGeneration(activity) })
            }
            add(
                R.string.harbor_menu_setup_again to {
                    activity.harborStop {
                        HarborNative.stopOpenPak()
                        restartIntoSetup(activity)
                    }
                }
            )
            add(
                R.string.harbor_menu_exit to {
                    activity.harborStop {
                        HarborNative.stopOpenPak()
                        activity.finishAndRemoveTask()
                    }
                }
            )
        }
        val dialog = MaterialAlertDialogBuilder(activity)
            .setTitle(R.string.harbor_menu_title)
            .setItems(items.map { activity.getString(it.first) as CharSequence }.toTypedArray()) { _, which ->
                items[which].second()
            }
            .setOnDismissListener {
                menuOpen = false
                activity.harborResetChord()
            }
            .show()
        HarborPanelInput.attach(dialog)
    }

    /**
     * The graphics driver HOME and every title run with: the phone's own or one installed by the
     * setup. A driver loads when the process starts, so a change restarts Harbor into the console.
     */
    private fun showDriverPicker(activity: EmulationActivity) {
        val flat = HarborSetupBridge.installedDrivers()
        val ids = mutableListOf("")
        val names = mutableListOf<CharSequence>(activity.getString(R.string.harbor_driver_system))
        for (i in 0 until flat.size / 3) {
            ids.add(flat[i * 3])
            names.add(
                if (flat[i * 3 + 2].isEmpty()) flat[i * 3 + 1] else "${flat[i * 3 + 1]} (${flat[i * 3 + 2]})"
            )
        }
        val current = ids.indexOf(HarborSetupBridge.currentDriver()).coerceAtLeast(0)
        val dialog = MaterialAlertDialogBuilder(activity)
            .setTitle(R.string.harbor_menu_driver)
            .setSingleChoiceItems(names.toTypedArray(), current) { picker, which ->
                picker.dismiss()
                if (which == current || !HarborSetupBridge.selectDriver(ids[which])) {
                    return@setSingleChoiceItems
                }
                activity.harborStop { restartProcess(activity) }
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
        HarborPanelInput.attach(dialog)
    }

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
    private fun restartProcess(from: Activity) {
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

    @Volatile
    private var menuOpen = false
}

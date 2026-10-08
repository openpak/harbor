// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

package org.yuzu.yuzu_emu.harbor

import android.app.Activity
import android.content.Intent
import android.net.Uri
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.Toast
import android.os.Handler
import android.os.Looper
import android.os.Process
import androidx.drawerlayout.widget.DrawerLayout
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import kotlin.concurrent.thread
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.activities.EmulationActivity
import org.yuzu.yuzu_emu.dialogs.QuickSettings
import org.yuzu.yuzu_emu.features.input.NativeInput
import org.yuzu.yuzu_emu.features.input.model.NativeButton
import org.yuzu.yuzu_emu.features.input.model.NpadStyleIndex
import org.yuzu.yuzu_emu.features.settings.model.BooleanSetting
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
        HarborSleep.reset()
        HarborNative.setConsoleMode(true)
        // What Eden's main screen sets up before any game: the multiplayer/network layer.
        NativeLibrary.initMultiplayer()
        thread(name = "HarborBoot") {
            if (HarborSetupBridge.ran) {
                // Keys or firmware changed in this process: the core reads its NAND again with them.
                NativeLibrary.initializeSystem(true)
            }
            HarborNative.applyConsolePreset()
            // The driver loads now, so a selected one that failed stops the boot here, not in HOME,
            // and the console never starts on the phone's own driver in its place.
            val driverProblem = ensureDriver().ifEmpty { HarborNative.checkGpuDriver() }
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
            Log.info("[Harbor] console start: graphics driver the phone's (not an Adreno GPU)")
            return ""
        }
        val path = StringSetting.DRIVER_PATH.getString()
        // A chosen driver whose package is gone is a failure, not the phone's driver.
        if (path.isNotEmpty() && !File(path).exists()) {
            return "The chosen graphics driver (${File(path).name}) is no longer installed."
        }
        val chosen = GpuDriverHelper.customDriverSettingData
        if (GpuDriverHelper.installedCustomDriverData != chosen) {
            if (path.isEmpty() || chosen.name == null) {
                GpuDriverHelper.installDefaultDriver()
            } else if (!GpuDriverHelper.installCustomDriver(File(path))) {
                return "The chosen graphics driver (${chosen.name}) could not be unpacked."
            }
        }
        val installed = GpuDriverHelper.installedCustomDriverData
        Log.info("[Harbor] console start: graphics driver " +
            (installed.name?.let { "$it ${installed.version ?: ""}".trim() } ?: "the phone's"))
        return ""
    }

    /**
     * The selected graphics driver did not load (EmulationSession::CheckGpuDriver, or its package
     * is gone or broken): the console does not boot on the phone's own driver instead. The driver
     * choice opens at once (HarborStoppedActivity): another installed driver that fits, the
     * recommended one downloaded, or the phone's own after a warning, then a restart into it.
     */
    fun showDriverRefusal(from: Activity, problem: String) {
        if (from.isFinishing) {
            return
        }
        Log.error("[Harbor] the graphics driver could not be loaded: $problem")
        from.startActivity(HarborStoppedActivity.driverFailed(from, problem))
        @Suppress("DEPRECATION")
        from.overridePendingTransition(0, 0)
        from.finish()
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
        HarborSleep.reset()
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
        val dialog = MaterialAlertDialogBuilder(HarborTheme.dialogContext(activity))
            .setTitle(R.string.harbor_menu_frame_gen)
            .setView(ScrollView(activity).apply { addView(container) })
            .setPositiveButton(R.string.harbor_close, null)
            .setNeutralButton(R.string.lossless_scaling_replace) { _, _ ->
                activity.harborLosslessPicker.launch(arrayOf("*/*"))
            }
            .setNegativeButton(R.string.lossless_scaling_remove) { _, _ ->
                val confirm = MaterialAlertDialogBuilder(HarborTheme.dialogContext(activity))
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
     * The Harbor menu (Android back, or Plus and Minus together): Resume, Detach/Attach Joy-Con,
     * Restart, Graphics driver, Show/Hide FPS, Log threads, Frame generation (on GPUs that can run
     * it), Set up again, Exit. Physical input is released first so nothing stays held in the guest.
     */
    fun showMenu(activity: EmulationActivity, trigger: String) {
        // A menu left behind by an earlier emulation screen (destroyed without a dismiss) does not
        // count: only one showing on this screen does.
        val open = menuDialog?.takeIf { it.isShowing && it.ownerActivity === activity }
        if (activity.isFinishing || open != null) {
            Log.info("[Harbor] menu asked for by $trigger: ${if (open != null) "already open" else "the screen is closing"}")
            return
        }
        Log.info("[Harbor] menu opened by $trigger")
        InputHandler.releaseAll()
        val fps = BooleanSetting.SHOW_PERFORMANCE_OVERLAY.getBoolean()
        val items = buildList<Pair<Int, () -> Unit>> {
            add(R.string.harbor_menu_resume to {})
            add(R.string.harbor_menu_home to { pressHome(HOME_SHORT_MS) })
            add(R.string.harbor_menu_home_hold to { pressHome(HOME_HOLD_MS) })
            add(R.string.harbor_menu_power to { pressPower() })
            add(
                (if (HarborNative.joyconsDetached()) R.string.harbor_menu_attach_joycons else R.string.harbor_menu_detach_joycons) to {
                    HarborNative.setJoyconsDetached(!HarborNative.joyconsDetached())
                }
            )
            if (!HarborKeyService.enabled(activity)) {
                add(R.string.harbor_menu_controller_home to {
                    Toast.makeText(activity, R.string.harbor_key_service_hint, Toast.LENGTH_LONG).show()
                    activity.startActivity(Intent(android.provider.Settings.ACTION_ACCESSIBILITY_SETTINGS))
                })
            }
            add(R.string.harbor_menu_restart to { stopThen(activity) { restartConsole(activity) } })
            // Only an Adreno GPU can load another driver (G4.GPU.1).
            if (GpuDriverHelper.isAdrenoGpu()) {
                add(R.string.harbor_menu_driver to { showDriverPicker(activity) })
            }
            add(
                (if (fps) R.string.harbor_menu_hide_fps else R.string.harbor_menu_show_fps) to {
                    activity.harborEmulationFragment()?.harborShowStats(!fps)
                }
            )
            add(R.string.harbor_menu_touch_controls to { showTouchControls(activity) })
            add(R.string.harbor_menu_log_threads to { HarborNative.logGuestThreads() })
            if (LosslessScalingHelper.isSupportedByGpu()) {
                add(R.string.harbor_menu_frame_gen to { showFrameGeneration(activity) })
            }
            add(R.string.harbor_menu_backup to { askBackupPassphrase(activity) })
            add(R.string.harbor_menu_restore to { confirmRestore(activity) })
            add(R.string.harbor_menu_new_console to { confirmNewConsole(activity) })
            add(
                R.string.harbor_menu_setup_again to {
                    stopThen(activity) {
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
        val dialog = MaterialAlertDialogBuilder(HarborTheme.dialogContext(activity))
            .setTitle(R.string.harbor_menu_title)
            .setItems(items.map { activity.getString(it.first) as CharSequence }.toTypedArray()) { _, which ->
                items[which].second()
            }
            .setOnDismissListener {
                menuDialog = null
                activity.harborResetChord()
            }
            .show()
        dialog.setOwnerActivity(activity)
        menuDialog = dialog
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
        val dialog = MaterialAlertDialogBuilder(HarborTheme.dialogContext(activity))
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

    private var menuDialog: android.app.Dialog? = null

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

    /** The touch controls hidden by the user's choice in the Harbor menu (not by a controller). */
    var padHidden: Boolean
        get() = preferences().getBoolean(PREF_PAD_HIDDEN, false)
        set(value) = preferences().edit().putBoolean(PREF_PAD_HIDDEN, value).apply()

    private const val PREF_PAD_HIDDEN = "pad_hidden"

    /** The Harbor menu's Touch controls page: show or hide, edit the layout, size and opacity, reset. */
    private fun showTouchControls(activity: EmulationActivity) {
        val fragment = activity.harborEmulationFragment() ?: return
        val hidden = padHidden
        val items = listOf<Pair<Int, () -> Unit>>(
            (if (hidden) R.string.harbor_pad_show else R.string.harbor_pad_hide) to {
                fragment.harborSetControlsHidden(!hidden)
            },
            R.string.harbor_pad_edit to { fragment.harborEditControls() },
            R.string.harbor_pad_adjust to { fragment.harborAdjustControls() },
            R.string.harbor_pad_reset to { fragment.harborResetControls() }
        )
        val dialog = MaterialAlertDialogBuilder(HarborTheme.dialogContext(activity))
            .setTitle(R.string.harbor_menu_touch_controls)
            .setItems(items.map { activity.getString(it.first) as CharSequence }.toTypedArray()) { _, which ->
                items[which].second()
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
        HarborPanelInput.attach(dialog)
    }

    /**
     * The HOME button from the Harbor menu, held for `holdMs` on the controller the touch overlay
     * drives (Handheld while the Joy-Cons are attached, Player 1 when detached). am times it like a
     * real press: under 400 ms is a short press, past the long-press time quick settings.
     */
    private fun pressHome(holdMs: Long) {
        val port = if (NativeInput.getStyleIndex(0) == NpadStyleIndex.Handheld) 8 else 0
        Log.info("[Harbor] HOME from the menu, held $holdMs ms")
        val handler = Handler(Looper.getMainLooper())
        // After the menu has closed, so the press lands in the guest.
        handler.postDelayed({
            NativeInput.onOverlayButtonEvent(port, NativeButton.Home, NativeInput.ButtonState.PRESSED)
            handler.postDelayed({
                NativeInput.onOverlayButtonEvent(port, NativeButton.Home, NativeInput.ButtonState.RELEASED)
            }, holdMs)
        }, HOME_AFTER_MENU_MS)
    }

    /**
     * The console's power button from the Harbor menu, as the Ryujinx Harbor's F5: held, so HOME's
     * overlay opens its power menu (Sleep Mode, Restart, Power Off). Sent after the menu has closed.
     */
    private fun pressPower() {
        Log.info("[Harbor] power button from the menu")
        Handler(Looper.getMainLooper()).postDelayed({ HarborNative.pressPower() }, HOME_AFTER_MENU_MS)
    }

    /** "Stopping the console…" while the session stops, then `next` (Restart, Set up again). */
    private fun stopThen(activity: EmulationActivity, next: () -> Unit) {
        val notice = MaterialAlertDialogBuilder(HarborTheme.dialogContext(activity))
            .setMessage(R.string.harbor_stopping)
            .setCancelable(false)
            .show()
        activity.harborStop {
            if (notice.isShowing) notice.dismiss()
            next()
        }
    }

    // Console backups: the NAND zipped with its device key sealed by a passphrase the user sets.
    // The passphrase lives only in this field between the question and the file picker's answer.
    private var backupPassphrase: String? = null
    private const val MIN_PASSPHRASE = 8

    private fun askPassphrase(activity: EmulationActivity, message: Int, answer: (String?) -> Unit) {
        val field = android.widget.EditText(activity).apply {
            inputType = android.text.InputType.TYPE_CLASS_TEXT or
                android.text.InputType.TYPE_TEXT_VARIATION_PASSWORD
        }
        val dialog = MaterialAlertDialogBuilder(activity)
            .setMessage(message)
            .setView(field)
            .setPositiveButton(android.R.string.ok) { _, _ -> answer(field.text.toString()) }
            .setNegativeButton(android.R.string.cancel) { _, _ -> answer(null) }
            .setCancelable(false)
            .show()
        HarborPanelInput.attach(dialog)
    }

    private fun tell(activity: Activity, message: String, then: () -> Unit = {}) {
        MaterialAlertDialogBuilder(activity)
            .setMessage(message)
            .setPositiveButton(android.R.string.ok, null)
            .setOnDismissListener { then() }
            .show()
    }

    private fun askBackupPassphrase(activity: EmulationActivity) {
        askPassphrase(activity, R.string.harbor_backup_passphrase) { first ->
            if (first == null) return@askPassphrase
            if (first.length < MIN_PASSPHRASE) {
                Toast.makeText(activity, R.string.harbor_backup_passphrase_short, Toast.LENGTH_LONG).show()
                return@askPassphrase
            }
            askPassphrase(activity, R.string.harbor_backup_passphrase_again) { second ->
                if (second == null) return@askPassphrase
                if (second != first) {
                    Toast.makeText(activity, R.string.harbor_backup_passphrase_differs, Toast.LENGTH_LONG).show()
                    return@askPassphrase
                }
                backupPassphrase = first
                val stamp = java.text.SimpleDateFormat("yyyyMMdd-HHmm", java.util.Locale.ROOT)
                    .format(java.util.Date())
                activity.harborBackupTarget.launch("harbor-console-$stamp.zip")
            }
        }
    }

    /** The picked file for a backup: console stopped, key sealed, NAND zipped, sealed key removed. */
    fun backUpTo(activity: EmulationActivity, uri: Uri?) {
        val passphrase = backupPassphrase
        backupPassphrase = null
        if (uri == null || passphrase == null) return
        stopThen(activity) {
            val progress = MaterialAlertDialogBuilder(activity)
                .setMessage(R.string.harbor_backup_working)
                .setCancelable(false)
                .show()
            thread(name = "HarborBackup") {
                val ok = try {
                    val nand = File(HarborNative.nandDirectory())
                    HarborNative.sealDeviceKey(passphrase) &&
                        FileUtil.zipFromInternalStorage(
                            nand,
                            nand.absolutePath,
                            java.io.BufferedOutputStream(activity.contentResolver.openOutputStream(uri)),
                            compression = false
                        ) == org.yuzu.yuzu_emu.model.TaskState.Completed
                } catch (e: Exception) {
                    Log.warning("[Harbor] console backup failed: ${e.message}")
                    false
                } finally {
                    HarborNative.removeSealedDeviceKey()
                }
                Log.info("[Harbor] console backup ${if (ok) "written" else "failed"}")
                activity.runOnUiThread {
                    progress.dismiss()
                    tell(activity, activity.getString(if (ok) R.string.harbor_backup_done else R.string.harbor_backup_failed)) {
                        restartConsole(activity)
                    }
                }
            }
        }
    }

    private fun confirmRestore(activity: EmulationActivity) {
        val dialog = MaterialAlertDialogBuilder(activity)
            .setTitle(R.string.harbor_menu_restore)
            .setMessage(R.string.harbor_restore_confirm)
            .setPositiveButton(android.R.string.ok) { _, _ ->
                activity.harborRestoreSource.launch(arrayOf("application/zip"))
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
        HarborPanelInput.attach(dialog)
    }

    /** A picked backup: console stopped, the NAND replaced, then its device key's passphrase. */
    fun restoreFrom(activity: EmulationActivity, uri: Uri) {
        stopThen(activity) {
            val progress = MaterialAlertDialogBuilder(activity)
                .setMessage(R.string.harbor_restore_working)
                .setCancelable(false)
                .show()
            thread(name = "HarborRestore") {
                val failure = try {
                    replaceNand(uri)
                } catch (e: Exception) {
                    Log.warning("[Harbor] console restore failed: ${e.message}")
                    R.string.harbor_restore_failed
                }
                activity.runOnUiThread {
                    progress.dismiss()
                    if (failure != null) {
                        tell(activity, activity.getString(failure)) { restartConsole(activity) }
                    } else {
                        unlockRestoredKey(activity)
                    }
                }
            }
        }
    }

    /** The backup into nand.restore, checked, then swapped in for the NAND. Null when it worked. */
    private fun replaceNand(uri: Uri): Int? {
        val nand = File(HarborNative.nandDirectory())
        val incoming = File(nand.path + ".restore")
        val previous = File(nand.path + ".previous")
        incoming.deleteRecursively()
        incoming.mkdirs()
        FileUtil.unzipToInternalStorage(uri.toString(), incoming)
        if (!File(incoming, "calibration/prodinfo.bin").isFile) {
            incoming.deleteRecursively()
            return R.string.harbor_restore_not_backup
        }
        previous.deleteRecursively()
        if (!nand.renameTo(previous)) {
            incoming.deleteRecursively()
            return R.string.harbor_restore_failed
        }
        if (!incoming.renameTo(nand)) {
            previous.renameTo(nand)
            return R.string.harbor_restore_failed
        }
        previous.deleteRecursively()
        HarborNative.reloadCalibration()
        Log.info("[Harbor] console restored from a backup")
        return null
    }

    private fun unlockRestoredKey(activity: EmulationActivity) {
        if (!HarborNative.deviceKeyRestorePending()) {
            HarborNative.removeSealedDeviceKey()
            tell(activity, activity.getString(R.string.harbor_restore_done)) { restartConsole(activity) }
            return
        }
        askPassphrase(activity, R.string.harbor_restore_passphrase) { passphrase ->
            if (passphrase == null) {
                tell(activity, activity.getString(R.string.harbor_restore_locked)) { restartConsole(activity) }
                return@askPassphrase
            }
            thread(name = "HarborRestoreKey") {
                val result = HarborNative.restoreDeviceKey(passphrase)
                activity.runOnUiThread {
                    when (result) {
                        0 -> tell(activity, activity.getString(R.string.harbor_restore_done)) { restartConsole(activity) }
                        1 -> {
                            Toast.makeText(activity, R.string.harbor_restore_wrong, Toast.LENGTH_LONG).show()
                            unlockRestoredKey(activity)
                        }
                        else -> tell(activity, activity.getString(R.string.harbor_restore_locked)) { restartConsole(activity) }
                    }
                }
            }
        }
    }

    private fun confirmNewConsole(activity: EmulationActivity) {
        val dialog = MaterialAlertDialogBuilder(activity)
            .setTitle(R.string.harbor_menu_new_console)
            .setMessage(R.string.harbor_new_console_confirm)
            .setPositiveButton(R.string.harbor_menu_new_console) { _, _ ->
                stopThen(activity) {
                    thread(name = "HarborNewConsole") {
                        val made = HarborNative.makeNewConsole()
                        activity.runOnUiThread {
                            tell(
                                activity,
                                activity.getString(if (made) R.string.harbor_new_console_done else R.string.harbor_new_console_failed)
                            ) { restartConsole(activity) }
                        }
                    }
                }
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
        HarborPanelInput.attach(dialog)
    }

    private const val HOME_SHORT_MS = 100L
    // Past am's 800 ms default and any long-press time the HOME menu sets for itself.
    private const val HOME_HOLD_MS = 1500L
    private const val HOME_AFTER_MENU_MS = 200L
}

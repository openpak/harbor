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
import org.yuzu.yuzu_emu.features.settings.model.IntSetting
import org.yuzu.yuzu_emu.features.settings.model.Settings
import org.yuzu.yuzu_emu.model.AppletInfo
import org.yuzu.yuzu_emu.model.Game
import org.yuzu.yuzu_emu.utils.GameHelper
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
            // OpenPak before HOME, with its credentials in the Android Keystore.
            (from as? HarborStartActivity)?.let { start ->
                start.runOnUiThread { start.showStatus(R.string.harbor_connecting) }
            }
            HarborNative.connectOpenPak()
            val qlaunch = NativeLibrary.getAppletLaunchPath(AppletInfo.QLaunch.entryId)
            Log.info("[Harbor] console boot: $games titles registered, HOME menu ${if (qlaunch.isEmpty()) "missing" else "present"}")
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
        MaterialAlertDialogBuilder(activity)
            .setTitle(R.string.frame_gen)
            .setView(ScrollView(activity).apply { addView(container) })
            .setPositiveButton(R.string.harbor_close, null)
            .setNeutralButton(R.string.lossless_scaling_replace) { _, _ ->
                activity.harborLosslessPicker.launch(arrayOf("*/*"))
            }
            .setNegativeButton(R.string.lossless_scaling_remove) { _, _ ->
                MaterialAlertDialogBuilder(activity)
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
            }
            .show()
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

    /** The minimal Harbor menu: Resume, Set up again, Exit, Detach/Attach Joy-Con. */
    fun showMenu(activity: EmulationActivity) {
        if (activity.isFinishing || menuOpen) {
            return
        }
        menuOpen = true
        MaterialAlertDialogBuilder(activity)
            .setTitle(R.string.harbor_menu_title)
            .setItems(
                arrayOf<CharSequence>(
                    activity.getString(R.string.harbor_menu_resume),
                    activity.getString(R.string.harbor_menu_setup_again),
                    activity.getString(R.string.harbor_menu_exit),
                    activity.getString(
                        if (HarborNative.joyconsDetached()) {
                            R.string.harbor_menu_attach_joycons
                        } else {
                            R.string.harbor_menu_detach_joycons
                        }
                    ),
                    activity.getString(R.string.harbor_menu_frame_gen)
                ).let { items ->
                    // Only on GPUs that can run the frame generation shaders.
                    if (LosslessScalingHelper.isSupportedByGpu()) items else items.dropLast(1).toTypedArray()
                }
            ) { _, which ->
                when (which) {
                    3 -> HarborNative.setJoyconsDetached(!HarborNative.joyconsDetached())
                    4 -> showFrameGeneration(activity)
                    1 -> activity.harborStop {
                        HarborNative.stopOpenPak()
                        restartIntoSetup(activity)
                    }
                    2 -> activity.harborStop {
                        HarborNative.stopOpenPak()
                        activity.finishAndRemoveTask()
                    }
                }
            }
            .setOnDismissListener { menuOpen = false }
            .show()
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

// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

package org.yuzu.yuzu_emu.harbor

import android.content.Intent
import android.content.res.Configuration
import android.os.Handler
import android.os.Looper
import android.widget.Toast
import androidx.annotation.Keep
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.YuzuApplication
import org.yuzu.yuzu_emu.activities.EmulationActivity
import org.yuzu.yuzu_emu.features.settings.model.IntSetting
import org.yuzu.yuzu_emu.utils.GpuDriverHelper
import org.yuzu.yuzu_emu.utils.LosslessScalingHelper
import org.yuzu.yuzu_emu.utils.Log
import org.yuzu.yuzu_emu.utils.NativeConfig

/**
 * What the Harbor menu (Borealis, jni/harbor_menu_android.cpp) asks of the app, by name. Called on
 * the menu's own thread: reads answer at once, anything that touches the screen runs on the UI
 * thread. The actions are the same calls the Harbor menu made before it moved onto the console's
 * screen.
 */
@Keep
object HarborMenuBridge {
    private val main = Handler(Looper.getMainLooper())

    private fun activity(): EmulationActivity? = EmulationActivity.foreground

    private fun onUi(work: (EmulationActivity) -> Unit) {
        main.post {
            val activity = activity() ?: return@post
            if (!activity.isFinishing) work(activity)
        }
    }

    /** Runs `work` on the UI thread and waits for its answer (at most a second). */
    private fun <T> fromUi(fallback: T, work: (EmulationActivity) -> T): T {
        var result = fallback
        val done = CountDownLatch(1)
        main.post {
            try {
                activity()?.let { result = work(it) }
            } finally {
                done.countDown()
            }
        }
        done.await(1, TimeUnit.SECONDS)
        return result
    }

    private fun flag(value: Boolean) = if (value) "1" else "0"

    private const val POWER_CONTROL = "button_power"

    @JvmStatic
    fun call(what: String, argument: String): String {
        val on = argument == "1"
        return when (what) {
            "platformDark" -> flag(
                YuzuApplication.appContext.resources.configuration.uiMode and
                    Configuration.UI_MODE_NIGHT_MASK == Configuration.UI_MODE_NIGHT_YES
            )
            "saveConfig" -> {
                NativeConfig.saveGlobalConfig()
                ""
            }

            // The console.
            "restart" -> {
                onUi { activity ->
                    HarborConsole.stopThen(activity) { HarborConsole.restartConsole(activity) }
                }
                ""
            }
            "setupAgain" -> {
                onUi { activity ->
                    HarborConsole.stopThen(activity) {
                        HarborNative.stopOpenPak()
                        HarborConsole.restartIntoSetup(activity)
                    }
                }
                ""
            }
            "exit" -> {
                onUi { activity ->
                    activity.harborStop {
                        HarborNative.stopOpenPak()
                        activity.finishAndRemoveTask()
                    }
                }
                ""
            }

            // Display & Performance.
            "showFps" -> flag(HarborConsole.showFps)
            "setShowFps" -> {
                // Saved at once, so the menu reads it back; the screen follows on the UI thread.
                HarborConsole.showFps = on
                onUi { it.harborEmulationFragment()?.harborShowStats(on) }
                ""
            }
            "showDeviceLine" -> flag(HarborConsole.showDeviceLine)
            "setShowDeviceLine" -> {
                HarborConsole.showDeviceLine = on
                onUi { it.harborEmulationFragment()?.harborShowStats(HarborConsole.showFps) }
                ""
            }
            "frameGenSupported" -> flag(LosslessScalingHelper.isSupportedByGpu())
            "chooseLosslessDll" -> {
                onUi { it.harborLosslessPicker.launch(arrayOf("*/*")) }
                ""
            }
            "removeLosslessDll" -> {
                onUi { activity ->
                    HarborConsole.changeLosslessDll(activity) {
                        LosslessScalingHelper.remove()
                        null
                    }
                }
                ""
            }
            // Only an Adreno GPU loads another driver (G4.GPU.1).
            "hasDriverChoice" -> flag(GpuDriverHelper.isAdrenoGpu())
            "drivers" -> {
                val lines = mutableListOf(
                    "",
                    YuzuApplication.appContext.getString(R.string.harbor_driver_system),
                    ""
                )
                lines.addAll(HarborSetupBridge.installedDrivers().map { it.replace('\n', ' ') })
                lines.joinToString("\n") + "\n"
            }
            "currentDriver" -> HarborSetupBridge.currentDriver()
            "chooseDriver" -> {
                // A driver loads when the process starts: Harbor restarts into the console.
                if (HarborSetupBridge.selectDriver(argument)) {
                    onUi { activity -> activity.harborStop { HarborConsole.restartProcess(activity) } }
                }
                ""
            }

            // Controllers and Input.
            "homeKeyEnabled" -> flag(HarborKeyService.enabled(YuzuApplication.appContext))
            "openHomeKeySettings" -> {
                onUi { activity ->
                    Toast.makeText(activity, R.string.harbor_key_service_hint, Toast.LENGTH_LONG).show()
                    activity.startActivity(Intent(android.provider.Settings.ACTION_ACCESSIBILITY_SETTINGS))
                }
                ""
            }

            // Touch Controls.
            "touchHidden" -> flag(HarborConsole.padHidden)
            "setTouchHidden" -> {
                HarborConsole.padHidden = on
                onUi { it.harborEmulationFragment()?.harborSetControlsHidden(on) }
                ""
            }
            "powerControlShown" -> flag(
                NativeConfig.getOverlayControlData().firstOrNull { it.id == POWER_CONTROL }?.enabled != false
            )
            "setPowerControlShown" -> {
                val data = NativeConfig.getOverlayControlData()
                val control = data.firstOrNull { it.id == POWER_CONTROL }
                if (control == null) {
                    Log.warning("[Harbor] the touch controls have no power button in this build")
                } else {
                    control.enabled = on
                    NativeConfig.setOverlayControlData(data)
                    NativeConfig.saveGlobalConfig()
                    onUi { it.harborEmulationFragment()?.harborRefreshControls() }
                }
                ""
            }
            "touchOpacity" -> IntSetting.OVERLAY_OPACITY.getInt().toString()
            "setTouchOpacity" -> {
                argument.toIntOrNull()?.let { value ->
                    IntSetting.OVERLAY_OPACITY.setInt(value)
                    onUi { it.harborEmulationFragment()?.harborSetControlOpacity(value) }
                }
                ""
            }
            "touchScale" -> IntSetting.OVERLAY_SCALE.getInt().toString()
            "setTouchScale" -> {
                argument.toIntOrNull()?.let { value ->
                    IntSetting.OVERLAY_SCALE.setInt(value)
                    onUi { it.harborEmulationFragment()?.harborSetControlScale(value) }
                }
                ""
            }
            "editTouchLayout" -> {
                onUi { it.harborEmulationFragment()?.harborEditControls() }
                ""
            }
            "resetTouchLayout" -> fromUi("") {
                it.harborEmulationFragment()?.harborResetControls()
                ""
            }

            // Harbor: the console's backup, restore and new identity (the console stops for each).
            "consoleAction" -> {
                onUi { activity ->
                    when (argument) {
                        "backup" -> HarborConsole.askBackupPassphrase(activity)
                        "restore" -> HarborConsole.confirmRestore(activity)
                        "new-console" -> HarborConsole.confirmNewConsole(activity)
                    }
                }
                ""
            }

            else -> {
                Log.warning("[Harbor] the menu asked for an unknown action: $what")
                ""
            }
        }
    }
}

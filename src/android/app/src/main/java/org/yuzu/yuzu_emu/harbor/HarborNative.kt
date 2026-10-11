// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

package org.yuzu.yuzu_emu.harbor

import androidx.annotation.Keep
import org.yuzu.yuzu_emu.NativeLibrary

/** Harbor's native calls (jni/harbor_setup_android.cpp). */
@Keep
object HarborNative {
    init {
        // Loads libyuzu-android and sets up SDL's Java side, as every Eden entry point does.
        NativeLibrary.toString()
    }

    /** Looks up the Java side the native wizard calls into. Call on a Java thread before the wizard runs. */
    external fun initialize()

    /**
     * Keys usable, firmware with the HOME menu installed, and harbor-setup.json completed: "" when
     * the console can boot, "setup" when the setup must run first, else why the check failed.
     */
    external fun checkConsole(): String

    /** A picker the native wizard opened has answered; a null uri means it was cancelled. */
    external fun onPicked(requestId: Int, uri: String?, name: String?)

    /** A picker the native wizard asked for could not open: the wizard shows `message`. */
    external fun onPickFailed(requestId: Int, message: String?)

    /** Bytes received so far; false asks the download to stop. */
    external fun downloadProgress(handle: Long, received: Long): Boolean

    /** Android back while the wizard shows: the wizard's own Back. */
    external fun onBackPressed()

    /** Whether this process boots the console (HOME menu) rather than a game from Eden's list. */
    external fun setConsoleMode(active: Boolean)

    /** Every applet from the firmware, handheld; saved, since the emulation screen reloads the config. */
    external fun applyConsolePreset()

    /** The setup's DLC and update folders (SAF tree URIs), whose files the console boot registers. */
    external fun addonFolders(): Array<String>

    /** Connects to OpenPak before HOME boots (blocking: up to ~10 s on a first connect). Not on the UI thread. */
    external fun connectOpenPak()

    /** Stops OpenPak's heartbeat and push on the way out. */
    external fun stopOpenPak()

    /** What HOME's power menu asked for when the console stopped: 0 nothing, 1 turn off, 2 restart. */
    external fun powerRequest(): Int

    /** Why the host stopped the console (e.g. the GPU was lost), cleared once read; "" if it did not. */
    external fun hostStopReason(): String

    /** The Joy-Cons off (true) or back on the console's rails. */
    external fun setJoyconsDetached(detached: Boolean)

    /**
     * Loads the console's graphics driver and checks it is the one the setup chose: "" when it is,
     * else why the console must not boot. Not on the UI thread.
     */
    external fun checkGpuDriver(): String

    /** Whether the Joy-Cons are off the rails. */
    external fun joyconsDetached(): Boolean

    /** Every guest thread to the log (System::LogGuestThreads), for hangs. */
    external fun logGuestThreads()

    /** The console's power button held: HOME's overlay opens its power menu. */
    external fun pressPower()

    /** Whether a recommended driver exists for this GPU (T30 on an Adreno 6xx). */
    external fun hasRecommendedDriver(): Boolean

    /** Why an installed driver does not fit this GPU, or "". */
    external fun driverProblem(name: String, library: String, minApi: Int): String

    /** Downloads, installs and selects the recommended driver: [id, name, error]. Not on the UI thread. */
    external fun installRecommendedDriver(): Array<String>

    /** Whether the setup's GitHub driver catalogue is offered on this phone. */
    external fun canBrowseDrivers(): Boolean

    /** What the catalogue is, or why it is not offered here. */
    external fun driverDownloadLead(): String

    /** Fetches the catalogue: [notice, then title, detail, file, recommended per package]. Blocking. */
    external fun driverCatalogue(): Array<String>

    /** Downloads, installs and chooses package `index` of the last catalogue: [id, name, error, cancelled]. Blocking. */
    external fun downloadDriver(index: Int): Array<String>

    /** The words for the running catalogue fetch or download. */
    external fun driverProgress(): String

    /** Cancels the running catalogue fetch or download (not an install). */
    external fun cancelDriverWork()

    /** The console's power button pressed briefly: sleep, or wake when asleep (HarborSleep). */
    external fun powerShortPress()

    /** The console's output gain for Android's audio focus: 1 full, 0.25 ducked, 0 muted. */
    external fun setHostGain(gain: Float)

    /** The console's theme: 1 Basic Black, 0 Basic White, -1 while no console runs. */
    external fun consoleTheme(): Int

    /** The console's saved theme: 1 Basic Black, 0 Basic White, -1 before its first boot. */
    external fun savedConsoleTheme(): Int
    /** The console's NAND directory, which a console backup holds. */
    external fun nandDirectory(): String

    /** The device key sealed with `passphrase` into the NAND for a backup about to be taken. */
    external fun sealDeviceKey(passphrase: String): Boolean

    /** The sealed device key out of the live NAND again. */
    external fun removeSealedDeviceKey()

    /** Read the calibration again (the NAND was replaced). */
    external fun reloadCalibration()

    /** A restored NAND whose device key waits for its backup's passphrase. */
    external fun deviceKeyRestorePending(): Boolean

    /** 0 keys usable, 1 not known yet (no password store), 2 not on this phone, 3 a backup's key waits. */
    external fun calibrationKeys(): Int

    /** 0 restored, 1 wrong passphrase, 2 no sealed key in the NAND, 3 the key could not be kept. */
    external fun restoreDeviceKey(passphrase: String): Int

    /** A fresh calibration and device key; this console's OpenPak credentials dropped. */
    external fun makeNewConsole(): Boolean

    /** Once a console boots: the Harbor menu's host and the mapped Power button (harbor_menu_android.cpp). */
    external fun initMenu()

    /**
     * On the UI thread when a controller connects, changes or goes, and as the menu opens: the
     * device list the menu shows, and a known pad's saved mapping applied again.
     */
    external fun checkControllers()

    /** Opens the Harbor menu on the console's screen: 0 the menu, 1 Settings, 2 frame generation, 3 controllers. */
    external fun openMenu(page: Int)

    /** Closes it: the console's picture and its controllers come back. */
    external fun closeMenu()

    /** Whether the Harbor menu is open. */
    external fun menuOpen(): Boolean

    /** Android back while the menu is open: the menu's own Back (on its top level it closes). */
    external fun menuBack()

    /** The console's picture on the emulation surface (left, top, right, bottom), empty while none runs. */
    external fun pictureRect(): IntArray
}

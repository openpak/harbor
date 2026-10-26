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

    /** Text from the Android keyboard; null when cancelled. */
    external fun onTextEntered(text: String?)

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

    /** Device theme for the firmware's first-time setup; completed consoles keep their own theme. */
    external fun setInitialConsoleDarkTheme(dark: Boolean)

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

    /** Why an installed driver does not fit this GPU, or "". */
    external fun driverProblem(name: String, library: String, minApi: Int): String

    /**
     * Harbor's own pages outside the console (HarborScreenActivity): a Borealis page of the Harbor
     * menu, kind 0 status, 1 stopped, 2 cannot start, 3 driver failed, drawn into the surface given
     * by hostSurface.
     */
    external fun showHostPage(kind: Int, text: String)

    external fun endHostPage()

    /** How many pictures of its own the console has put on the screen in this process. */
    external fun guestFrames(): Long

    /** The page's surface (a Surface), or null when it went. */
    external fun hostSurface(surface: Any?)

    /** A button by the console's positions: 0 A, 1 B, 2 X, 3 Y, 4 up, 5 down, 6 left, 7 right. */
    external fun hostButton(button: Int, down: Boolean)

    /** A finger on the page, `x` and `y` from 0 to 1. */
    external fun hostTouch(id: Int, x: Float, y: Float, down: Boolean)

    /** A status line on the console's screen while Harbor stops it ("Stopping the console…"). */
    external fun menuStatus(text: String)

    /** A short notice from Harbor itself, a line on the console's screen (MenuHost::HostNotice). */
    external fun hostNotice(text: String)

    /** A status line for the open host page (a driver file installed through the picker). */
    external fun hostPageNotice(text: String, bad: Boolean)

    /** A picked driver package read, checked, installed and chosen: [id, name, error]. Blocking. */
    external fun installDriverFile(location: String, name: String): Array<String>

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

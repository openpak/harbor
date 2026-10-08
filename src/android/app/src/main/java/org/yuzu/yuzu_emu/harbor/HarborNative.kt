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

    /** The console's output gain for Android's audio focus: 1 full, 0.25 ducked, 0 muted. */
    external fun setHostGain(gain: Float)

    /** The console's theme: 1 Basic Black, 0 Basic White, -1 while no console runs. */
    external fun consoleTheme(): Int

    /** The console's saved theme: 1 Basic Black, 0 Basic White, -1 before its first boot. */
    external fun savedConsoleTheme(): Int

    /** The console's picture on the emulation surface (left, top, right, bottom), empty while none runs. */
    external fun pictureRect(): IntArray
}

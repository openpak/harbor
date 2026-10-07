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

    /** Keys usable, firmware with the HOME menu installed, and harbor-setup.json completed. */
    external fun isConsoleReady(): Boolean

    /** A picker the native wizard opened has answered; a null uri means it was cancelled. */
    external fun onPicked(requestId: Int, uri: String?, name: String?)

    /** Bytes received so far; false asks the download to stop. */
    external fun downloadProgress(handle: Long, received: Long): Boolean

    /** Android back while the wizard shows: the wizard's own Back. */
    external fun onBackPressed()

    /** Whether this process boots the console (HOME menu) rather than a game from Eden's list. */
    external fun setConsoleMode(active: Boolean)

    /** Every applet from the firmware, handheld; saved, since the emulation screen reloads the config. */
    external fun applyConsolePreset()
}

// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

package org.yuzu.yuzu_emu.harbor

import android.app.Activity
import android.content.Intent
import androidx.annotation.Keep
import java.io.File
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.utils.Log

/**
 * Crash reports (common/harbor_crash.h): off until the user agrees; then a native crash or a guest
 * fatal error leaves a scrubbed report on the phone. The question and the offer of last time's
 * report (view it, share it, delete it) are a Harbor menu page drawn in Borealis once the console's
 * picture is up (harbor_setup/menu.h, MenuOfferCrashReport), as on the desktop. Nothing is ever sent
 * by Harbor itself. Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).
 */
@Keep
object HarborCrashReports {
    init {
        // Loads libyuzu-android, as every Eden entry point does.
        NativeLibrary.toString()
    }

    /** Installs the fatal-signal handler (with consent) and finishes a report a crash left. */
    external fun install()

    /** 0 not asked yet, 1 reports kept, 2 not kept. */
    external fun consent(): Int

    external fun setConsent(accepted: Boolean)

    /** The newest report not offered yet (path, name, summary, when), marked offered; empty if none. */
    external fun takeOffer(): Array<String>

    /** The newest kept report (path, name, summary, when); empty if none. */
    external fun latest(): Array<String>

    external fun read(path: String): String

    external fun delete(path: String)

    /**
     * At launch, before the console boots: logs why the last processes ended and installs the
     * handler. The question or the offer comes later, on the console's screen.
     */
    fun onLaunch(activity: Activity, next: () -> Unit) {
        // Why the last processes ended (a native crash's tombstone decoded), into eden_log.txt.
        HarborExitReasons.logRecent(activity)
        install()
        next()
    }

    /** The report as text to an app the user picks (the Harbor menu's Share). */
    fun share(activity: Activity, path: String, name: String) {
        // Only a report in Harbor's own reports directory.
        if (!File(path).name.startsWith(REPORT_PREFIX)) {
            return
        }
        var text = read(path)
        if (text.isEmpty()) {
            return
        }
        // A share goes through a binder transaction: the head of a very large report only.
        if (text.length > SHARE_MAX_CHARS) {
            text = text.substring(0, SHARE_MAX_CHARS) + "\n[truncated]\n"
        }
        val send = Intent(Intent.ACTION_SEND)
            .setType("text/plain")
            .putExtra(Intent.EXTRA_SUBJECT, name)
            .putExtra(Intent.EXTRA_TEXT, text)
        try {
            activity.startActivity(Intent.createChooser(send, activity.getString(R.string.harbor_crash_share)))
            Log.info("[Harbor] crash report $name shared by the user")
        } catch (e: Exception) {
            Log.warning("[Harbor] the share sheet did not open: ${e.message}")
        }
    }

    private const val REPORT_PREFIX = "harbor-crash-"
    private const val SHARE_MAX_CHARS = 400_000
}

// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

package org.yuzu.yuzu_emu.harbor

import android.app.Activity
import android.app.AlertDialog
import android.app.Application
import android.content.Intent
import android.graphics.Typeface
import android.os.Bundle
import android.widget.ScrollView
import android.widget.TextView
import androidx.annotation.Keep
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.utils.Log

/**
 * Crash reports (common/harbor_crash.h): off until the user agrees; then a native crash or a guest
 * fatal error leaves a scrubbed report on the phone, and the next launch offers it once: view it,
 * share it (the user picks where), or delete it. Nothing is ever sent by Harbor itself.
 * Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).
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
     * At launch, before the console boots: asks once whether to keep reports, or offers the report
     * the last crash left. `next` runs when the user is done (at once when there is nothing to ask).
     */
    fun onLaunch(activity: Activity, next: () -> Unit) {
        // Why the last processes ended (a native crash's tombstone decoded), into eden_log.txt.
        HarborExitReasons.logRecent(activity)
        install()
        when {
            consent() == CONSENT_UNASKED -> askConsent(activity, next)
            else -> {
                val offer = takeOffer()
                if (offer.size == 4) {
                    Log.info("[Harbor] offering crash report ${offer[1]}")
                    offerReport(activity, offer, next)
                } else {
                    next()
                }
            }
        }
    }

    private fun askConsent(activity: Activity, next: () -> Unit) {
        val dialog = AlertDialog.Builder(activity, android.R.style.Theme_DeviceDefault_Dialog_Alert)
            .setTitle(R.string.harbor_crash_consent_title)
            .setMessage(R.string.harbor_crash_consent_message)
            .setPositiveButton(R.string.harbor_crash_consent_keep) { _, _ ->
                setConsent(true)
                Log.info("[Harbor] crash reports kept from now on")
            }
            .setNegativeButton(R.string.harbor_crash_consent_decline) { _, _ ->
                setConsent(false)
                Log.info("[Harbor] crash reports not kept")
            }
            // Dismissed without an answer: asked again next time; until then nothing is kept.
            .setOnDismissListener { next() }
            .show()
        HarborPanelInput.attach(dialog)
    }

    private fun offerReport(activity: Activity, report: Array<String>, next: () -> Unit) {
        var shared = false
        val dialog = AlertDialog.Builder(activity, android.R.style.Theme_DeviceDefault_Dialog_Alert)
            .setTitle(R.string.harbor_crash_offer_title)
            .setMessage(activity.getString(R.string.harbor_crash_offer_message, report[2], report[3]))
            .setPositiveButton(R.string.harbor_crash_view) { _, _ -> }
            .setNeutralButton(R.string.harbor_crash_share) { _, _ ->
                shared = share(activity, report, next)
            }
            .setNegativeButton(R.string.harbor_crash_delete) { _, _ ->
                delete(report[0])
                Log.info("[Harbor] crash report ${report[1]} deleted")
            }
            .setOnDismissListener { if (!shared) next() }
            .create()
        dialog.setOnShowListener {
            // View keeps the offer open behind the report's text.
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener {
                showText(activity, report)
            }
        }
        dialog.show()
        HarborPanelInput.attach(dialog)
    }

    /** The report's text, selectable, in a scrolling dialog. */
    private fun showText(activity: Activity, report: Array<String>) {
        val text = TextView(activity).apply {
            text = read(report[0])
            typeface = Typeface.MONOSPACE
            textSize = 11f
            setTextIsSelectable(true)
            val pad = (12 * resources.displayMetrics.density).toInt()
            setPadding(pad, pad, pad, pad)
        }
        val dialog = AlertDialog.Builder(activity, android.R.style.Theme_DeviceDefault_Dialog_Alert)
            .setTitle(report[1])
            .setView(ScrollView(activity).apply { addView(text) })
            .setPositiveButton(R.string.harbor_close, null)
            .show()
        HarborPanelInput.attach(dialog)
    }

    /**
     * The report as text to an app the user picks. The launch goes on once Harbor is back in front.
     * Returns whether the share sheet opened.
     */
    private fun share(activity: Activity, report: Array<String>, next: () -> Unit): Boolean {
        var text = read(report[0])
        if (text.isEmpty()) {
            return false
        }
        // A share goes through a binder transaction: the head of a very large report only.
        if (text.length > SHARE_MAX_CHARS) {
            text = text.substring(0, SHARE_MAX_CHARS) + "\n[truncated]\n"
        }
        val send = Intent(Intent.ACTION_SEND)
            .setType("text/plain")
            .putExtra(Intent.EXTRA_SUBJECT, report[1])
            .putExtra(Intent.EXTRA_TEXT, text)
        return try {
            activity.startActivity(Intent.createChooser(send, activity.getString(R.string.harbor_crash_share)))
            Log.info("[Harbor] crash report ${report[1]} shared by the user")
            // Paused by the share sheet, resumed when the user comes back: then the launch goes on.
            activity.application.registerActivityLifecycleCallbacks(object : SimpleLifecycle() {
                private var paused = false
                override fun onActivityPaused(a: Activity) {
                    if (a === activity) paused = true
                }
                override fun onActivityResumed(a: Activity) {
                    if (a === activity && paused) {
                        activity.application.unregisterActivityLifecycleCallbacks(this)
                        next()
                    }
                }
            })
            true
        } catch (e: Exception) {
            Log.warning("[Harbor] the share sheet did not open: ${e.message}")
            false
        }
    }

    private open class SimpleLifecycle : Application.ActivityLifecycleCallbacks {
        override fun onActivityCreated(a: Activity, state: Bundle?) {}
        override fun onActivityStarted(a: Activity) {}
        override fun onActivityResumed(a: Activity) {}
        override fun onActivityPaused(a: Activity) {}
        override fun onActivityStopped(a: Activity) {}
        override fun onActivitySaveInstanceState(a: Activity, state: Bundle) {}
        override fun onActivityDestroyed(a: Activity) {}
    }

    private const val CONSENT_UNASKED = 0
    private const val SHARE_MAX_CHARS = 400_000
}

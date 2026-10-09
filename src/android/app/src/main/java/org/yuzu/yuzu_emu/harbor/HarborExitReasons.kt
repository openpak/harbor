// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

package org.yuzu.yuzu_emu.harbor

import android.app.ActivityManager
import android.app.ApplicationExitInfo
import android.content.Context
import android.os.Build
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.TimeZone
import org.yuzu.yuzu_emu.utils.DirectoryInitialization
import org.yuzu.yuzu_emu.utils.Log

/**
 * Why Harbor's earlier processes ended (Android 11+), written into eden_log.txt at the next start,
 * so a crash on a phone with no logcat at hand can still be read from the log alone. A native
 * crash's tombstone is kept beside the crash reports (crash-reports/tombstone-<time>.pb) and its
 * signal, fault address, abort message, crashing thread's backtrace and the last log lines are
 * decoded into the log. Everything stays on the device.
 */
object HarborExitReasons {
    fun logRecent(context: Context) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) {
            return
        }
        try {
            val manager = context.getSystemService(ActivityManager::class.java) ?: return
            val prefs = context.getSharedPreferences("harbor", Context.MODE_PRIVATE)
            val lastSeen = prefs.getLong(PREF_LAST_EXIT, 0L)
            val exits = manager.getHistoricalProcessExitReasons(context.packageName, 0, 5)
                .filter { it.timestamp > lastSeen }
                .sortedBy { it.timestamp }
            for (exit in exits) {
                logExit(exit)
            }
            exits.maxOfOrNull { it.timestamp }?.let { prefs.edit().putLong(PREF_LAST_EXIT, it).apply() }
        } catch (e: Exception) {
            Log.warning("harbor: last exit: could not be read: ${e.javaClass.simpleName} ${e.message}")
        }
    }

    private fun logExit(exit: ApplicationExitInfo) {
        val status = when (exit.reason) {
            ApplicationExitInfo.REASON_CRASH_NATIVE, ApplicationExitInfo.REASON_SIGNALED ->
                "signal ${exit.status}"
            else -> "${exit.status}"
        }
        Log.info(
            "harbor: last exit: reason=${reasonName(exit.reason)} status=$status " +
                "desc=${exit.description ?: "-"} pss/rss=${exit.pss}/${exit.rss} kB " +
                "process=${exit.processName} at ${utc(exit.timestamp)}"
        )
        val trace = try {
            exit.traceInputStream?.use { it.readBytes() }
        } catch (e: Exception) {
            Log.warning("harbor: last exit: trace not readable: ${e.message}")
            null
        } ?: return
        when (exit.reason) {
            ApplicationExitInfo.REASON_CRASH_NATIVE -> logTombstone(trace, exit.timestamp)
            else -> logText("trace", String(trace), MAX_TEXT_LINES)
        }
    }

    private fun logTombstone(bytes: ByteArray, timestamp: Long) {
        saveTombstone(bytes, timestamp)
        // Android 11 gives the text tombstone; Android 12+ the protobuf one.
        if (bytes.isNotEmpty() && bytes[0] == '*'.code.toByte()) {
            logText("tombstone", String(bytes), MAX_TEXT_LINES)
            return
        }
        try {
            for (line in TombstoneSummary.decode(bytes)) {
                Log.info("harbor: tombstone: $line")
            }
        } catch (e: Exception) {
            Log.warning("harbor: tombstone: could not be decoded: ${e.javaClass.simpleName} ${e.message}")
        }
    }

    private fun saveTombstone(bytes: ByteArray, timestamp: Long) {
        val root = DirectoryInitialization.userDirectory ?: return
        try {
            val dir = File(root, "crash-reports").apply { mkdirs() }
            val name = "tombstone-${SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).apply {
                timeZone = TimeZone.getTimeZone("UTC")
            }.format(Date(timestamp))}.pb"
            File(dir, name).writeBytes(bytes)
            Log.info("harbor: tombstone saved as crash-reports/$name (${bytes.size} bytes)")
            // At most five kept.
            dir.listFiles { f -> f.name.startsWith("tombstone-") }
                ?.sortedByDescending { it.name }?.drop(5)?.forEach { it.delete() }
        } catch (e: Exception) {
            Log.warning("harbor: tombstone not saved: ${e.message}")
        }
    }

    private fun logText(what: String, text: String, maxLines: Int) {
        val lines = text.lines()
        for (line in lines.take(maxLines)) {
            Log.info("harbor: $what: $line")
        }
        if (lines.size > maxLines) {
            Log.info("harbor: $what: (${lines.size - maxLines} more lines)")
        }
    }

    private fun utc(millis: Long): String =
        SimpleDateFormat("yyyy-MM-dd HH:mm:ss 'UTC'", Locale.US)
            .apply { timeZone = TimeZone.getTimeZone("UTC") }.format(Date(millis))

    private fun reasonName(reason: Int): String = when (reason) {
        ApplicationExitInfo.REASON_EXIT_SELF -> "EXIT_SELF"
        ApplicationExitInfo.REASON_SIGNALED -> "SIGNALED"
        ApplicationExitInfo.REASON_LOW_MEMORY -> "LOW_MEMORY"
        ApplicationExitInfo.REASON_CRASH -> "CRASH"
        ApplicationExitInfo.REASON_CRASH_NATIVE -> "CRASH_NATIVE"
        ApplicationExitInfo.REASON_ANR -> "ANR"
        ApplicationExitInfo.REASON_INITIALIZATION_FAILURE -> "INITIALIZATION_FAILURE"
        ApplicationExitInfo.REASON_PERMISSION_CHANGE -> "PERMISSION_CHANGE"
        ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE -> "EXCESSIVE_RESOURCE_USAGE"
        ApplicationExitInfo.REASON_USER_REQUESTED -> "USER_REQUESTED"
        ApplicationExitInfo.REASON_USER_STOPPED -> "USER_STOPPED"
        ApplicationExitInfo.REASON_DEPENDENCY_DIED -> "DEPENDENCY_DIED"
        ApplicationExitInfo.REASON_OTHER -> "OTHER"
        else -> "UNKNOWN($reason)"
    }

    private const val PREF_LAST_EXIT = "last_exit_timestamp"
    private const val MAX_TEXT_LINES = 200
}

/**
 * The parts of an AOSP tombstone (system/core/debuggerd/proto/tombstone.proto) worth a log line:
 * the signal, the abort message, the causes, the crashing thread's registers and backtrace, and the
 * last lines of the process's own log. A minimal protobuf reader, no dependency.
 */
object TombstoneSummary {
    fun decode(bytes: ByteArray): List<String> {
        val out = mutableListOf<String>()
        val top = Message(bytes, 0, bytes.size)
        val tid = top.varint(6)
        out.add(
            "pid ${top.varint(5)} tid $tid uid ${top.varint(7)} cmdline ${top.strings(9).joinToString(" ")} " +
                "build ${top.string(2)} uptime ${top.varint(20)}s"
        )
        top.message(10)?.let { signal ->
            val fault = if (signal.varint(8) != 0L) " fault address 0x${signal.varint(9).toULong().toString(16)}" else ""
            out.add(
                "signal ${signal.varint(1)} (${signal.string(2)}), code ${signal.varint(3).toInt()} " +
                    "(${signal.string(4)})$fault"
            )
        }
        top.string(14).takeIf { it.isNotEmpty() }?.let { out.add("abort message: $it") }
        for (cause in top.messages(15)) {
            cause.string(1).takeIf { it.isNotEmpty() }?.let { out.add("cause: $it") }
        }
        // threads: map<uint32, Thread> = 16, entries {key = 1, value = 2}.
        val crashing = top.messages(16).firstOrNull { it.varint(1) == tid }?.message(2)
        if (crashing != null) {
            out.add("crashing thread ${crashing.varint(1)} \"${crashing.string(2)}\"")
            val wanted = setOf("pc", "lr", "sp", "x0", "x1", "x8", "x29", "x30")
            val registers = crashing.messages(3)
                .map { it.string(1) to it.varint(2) }
                .filter { it.first in wanted }
                .joinToString(" ") { "${it.first}=0x${it.second.toULong().toString(16)}" }
            if (registers.isNotEmpty()) {
                out.add("registers: $registers")
            }
            for (note in crashing.strings(7)) {
                out.add("backtrace note: $note")
            }
            for ((index, frame) in crashing.messages(4).withIndex()) {
                val function = frame.string(4)
                val symbol = if (function.isEmpty()) "" else " ($function+${frame.varint(5)})"
                val buildId = frame.string(8).takeIf { it.isNotEmpty() }?.let { " (BuildId: $it)" } ?: ""
                out.add(
                    "  #%02d pc %016x  %s%s%s".format(
                        index, frame.varint(1), frame.string(6).ifEmpty { "<unknown>" }, symbol, buildId
                    )
                )
            }
        } else {
            out.add("crashing thread $tid not in the tombstone")
        }
        // The process's last log lines (log_buffers = 18: name = 1, logs = 2 {tag = 5, message = 6}).
        val logs = top.messages(18).flatMap { buffer ->
            buffer.messages(2).map { "${buffer.string(1)} ${it.string(1)} ${it.varint(3)} ${it.string(5)}: ${it.string(6).trimEnd()}" }
        }
        for (line in logs.takeLast(LOG_LINES)) {
            out.add("log: $line")
        }
        return out
    }

    private const val LOG_LINES = 60

    /** One protobuf message: fields read on demand, the last value winning for a scalar. */
    private class Message(private val data: ByteArray, private val start: Int, private val end: Int) {
        private class Field(val number: Int, val wire: Int, val value: Long, val from: Int, val to: Int)

        private val fields: List<Field> by lazy { parse() }

        private fun parse(): List<Field> {
            val list = mutableListOf<Field>()
            var at = start
            fun varintAt(): Long {
                var result = 0L
                var shift = 0
                while (at < end) {
                    val b = data[at++].toInt() and 0xff
                    result = result or ((b and 0x7f).toLong() shl shift)
                    if (b and 0x80 == 0) return result
                    shift += 7
                    if (shift >= 64) break
                }
                return result
            }
            while (at < end) {
                val key = varintAt()
                val number = (key ushr 3).toInt()
                when (val wire = (key and 7).toInt()) {
                    0 -> list.add(Field(number, wire, varintAt(), 0, 0))
                    1 -> {
                        var v = 0L
                        for (i in 0 until 8) v = v or ((data[at + i].toLong() and 0xff) shl (8 * i))
                        at += 8
                        list.add(Field(number, wire, v, 0, 0))
                    }
                    2 -> {
                        val length = varintAt().toInt()
                        if (length < 0 || at + length > end) return list
                        list.add(Field(number, wire, 0, at, at + length))
                        at += length
                    }
                    5 -> {
                        var v = 0L
                        for (i in 0 until 4) v = v or ((data[at + i].toLong() and 0xff) shl (8 * i))
                        at += 4
                        list.add(Field(number, wire, v, 0, 0))
                    }
                    else -> return list // groups are not used by tombstone.proto
                }
            }
            return list
        }

        fun varint(number: Int): Long = fields.lastOrNull { it.number == number && it.wire != 2 }?.value ?: 0L

        fun string(number: Int): String =
            fields.lastOrNull { it.number == number && it.wire == 2 }
                ?.let { String(data, it.from, it.to - it.from, Charsets.UTF_8) } ?: ""

        fun strings(number: Int): List<String> =
            fields.filter { it.number == number && it.wire == 2 }
                .map { String(data, it.from, it.to - it.from, Charsets.UTF_8) }

        fun message(number: Int): Message? =
            fields.lastOrNull { it.number == number && it.wire == 2 }?.let { Message(data, it.from, it.to) }

        fun messages(number: Int): List<Message> =
            fields.filter { it.number == number && it.wire == 2 }.map { Message(data, it.from, it.to) }
    }
}

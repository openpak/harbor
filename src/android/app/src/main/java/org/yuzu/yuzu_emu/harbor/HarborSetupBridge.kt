// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

package org.yuzu.yuzu_emu.harbor

import android.net.Uri
import android.os.SystemClock
import android.provider.OpenableColumns
import androidx.annotation.Keep
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.io.InputStream
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest
import java.util.zip.ZipEntry
import java.util.zip.ZipException
import java.util.zip.ZipFile
import java.util.zip.ZipOutputStream
import javax.net.ssl.HttpsURLConnection
import org.yuzu.yuzu_emu.YuzuApplication
import org.yuzu.yuzu_emu.features.settings.model.StringSetting
import org.yuzu.yuzu_emu.utils.GpuDriverHelper
import org.yuzu.yuzu_emu.utils.GpuDriverMetadata
import org.yuzu.yuzu_emu.utils.Log
import org.yuzu.yuzu_emu.utils.NativeConfig

/**
 * What the native setup wizard asks of Android: pickers (the Storage Access Framework), the network,
 * and Eden's graphics driver store. Called from native threads; nothing here touches views directly.
 */
@Keep
object HarborSetupBridge {
    @Volatile
    var activity: HarborSetupActivity? = null

    /** Whether the wizard already ran in this process (SDL allows one run; a second needs a new process). */
    @Volatile
    var ran = false

    private const val USER_AGENT = "Harbor"

    // Driver packages (G4.GPU.10) and downloads (G4.GPU.5), as the Ryujinx Harbor limits them.
    private const val MAX_META_BYTES = 64L * 1024
    private const val MAX_DRIVER_FILES = 128
    private const val MAX_DRIVER_FILES_BYTES = 256L * 1024 * 1024
    private const val ZIP_SLACK_BYTES = 1L * 1024 * 1024
    private const val DOWNLOAD_DEADLINE_MS = 3 * 60 * 1000L

    @JvmStatic
    fun pickFile(requestId: Int, title: String, extensions: Array<String>) {
        val host = activity
        if (host == null) {
            HarborNative.onPicked(requestId, null, null)
            return
        }
        host.runOnUiThread { host.pickFile(requestId) }
    }

    @JvmStatic
    fun pickFolder(requestId: Int, title: String) {
        val host = activity
        if (host == null) {
            HarborNative.onPicked(requestId, null, null)
            return
        }
        host.runOnUiThread { host.pickFolder(requestId) }
    }

    @JvmStatic
    fun editText(title: String, hint: String, initial: String, maxLength: Int, secret: Boolean) {
        val host = activity
        if (host == null) {
            HarborNative.onTextEntered(null)
            return
        }
        host.runOnUiThread { host.editText(title, hint, initial, maxLength, secret) }
    }

    @JvmStatic
    fun cancelText() {
        activity?.let { host -> host.runOnUiThread { host.cancelText() } }
    }

    /** ["ok", body] or ["error", why]. */
    @JvmStatic
    fun httpGet(url: String, maxBytes: Long): Array<String> = try {
        val connection = connect(url)
        connection.setRequestProperty("Accept", "application/vnd.github+json")
        val code = connection.responseCode
        when {
            code == HttpURLConnection.HTTP_FORBIDDEN || code == 429 ->
                arrayOf("error", "GitHub's request limit was reached. Try again later.")
            code !in 200..299 -> arrayOf("error", "GitHub answered $code.")
            else -> connection.inputStream.use { input ->
                arrayOf("ok", String(readBounded(input, maxBytes), Charsets.UTF_8))
            }
        }
    } catch (e: Exception) {
        arrayOf("error", e.message ?: e.javaClass.simpleName)
    }

    /** ["ok", path, sha256 hex, size] or ["error", why]. Progress goes to HarborNative.downloadProgress. */
    @JvmStatic
    fun download(url: String, maxBytes: Long, handle: Long): Array<String> {
        val directory = File(YuzuApplication.appContext.cacheDir, "harbor-download").apply { mkdirs() }
        // Named after the release asset: the store keeps one package per file name.
        val asset = Uri.decode(url.substringBefore('?').substringAfterLast('/'))
        val file = File(directory, File(asset).name.takeIf { it.endsWith(".zip", ignoreCase = true) } ?: "driver.zip")
        directory.listFiles()?.forEach { it.delete() }
        return try {
            val connection = connect(url)
            val code = connection.responseCode
            if (code !in 200..299) {
                return arrayOf("error", "GitHub answered $code.")
            }
            val length = connection.contentLengthLong
            if (length > maxBytes) {
                return arrayOf("error", "The download is larger than the release says.")
            }
            val digest = MessageDigest.getInstance("SHA-256")
            // The whole download, not just each read, has three minutes.
            val deadline = SystemClock.elapsedRealtime() + DOWNLOAD_DEADLINE_MS
            var total = 0L
            connection.inputStream.use { input ->
                FileOutputStream(file).use { output ->
                    val buffer = ByteArray(64 * 1024)
                    while (true) {
                        val read = input.read(buffer)
                        if (read < 0) {
                            break
                        }
                        total += read
                        if (total > maxBytes) {
                            return arrayOf("error", "The download is larger than the release says.")
                        }
                        digest.update(buffer, 0, read)
                        output.write(buffer, 0, read)
                        if (!HarborNative.downloadProgress(handle, total)) {
                            return arrayOf("error", "Cancelled.")
                        }
                        if (SystemClock.elapsedRealtime() > deadline) {
                            file.delete()
                            return arrayOf("error", "The download took longer than three minutes. Try again.")
                        }
                    }
                }
            }
            val hex = digest.digest().joinToString("") { "%02x".format(it) }
            arrayOf("ok", file.path, hex, total.toString())
        } catch (e: Exception) {
            file.delete()
            arrayOf("error", e.message ?: e.javaClass.simpleName)
        }
    }

    /** The drivers in Eden's store, flattened as id, name, detail. */
    @JvmStatic
    fun installedDrivers(): Array<String> = GpuDriverHelper.getDrivers().flatMap { (path, meta) ->
        listOf(
            File(path).name,
            meta.name ?: File(path).nameWithoutExtension,
            listOfNotNull(meta.version, meta.vendor).filter { it.isNotBlank() }.joinToString(" · ")
        )
    }.toTypedArray()

    /** The store id of the driver Eden starts with, or "" for the phone's own. */
    @JvmStatic
    fun currentDriver(): String {
        val path = StringSetting.DRIVER_PATH.getString()
        return if (path.isEmpty() || !File(path).exists()) "" else File(path).name
    }

    /**
     * Copies a driver package aside, checks it and reads it with Eden's own parser (so Eden can load
     * what Harbor accepts): [staged, name, version, vendor, libraryName, minApi, error]. Only the files
     * beside meta.json are kept, flat, in the staged copy, so a package in a subfolder installs and
     * nothing in the zip chooses where a file lands.
     * Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base), GpuDrivers.Install.
     */
    @JvmStatic
    fun inspectDriver(location: String): Array<String> {
        val context = YuzuApplication.appContext
        val name = displayName(location).takeIf { it.endsWith(".zip", ignoreCase = true) } ?: "driver.zip"
        val directory = File(context.cacheDir, "harbor-driver").apply {
            deleteRecursively()
            mkdirs()
        }
        val source = File(directory, "source.zip")
        val staged = File(directory, File(name).name)
        return try {
            openInput(location).use { input ->
                FileOutputStream(source).use { output ->
                    if (copyBounded(input, output, MAX_DRIVER_FILES_BYTES + ZIP_SLACK_BYTES) < 0) {
                        return failure("The driver package is too large to install.")
                    }
                }
            }
            val problem = repackDriver(source, staged, name)
            source.delete()
            if (problem != null) {
                staged.delete()
                Log.warning("[Harbor] driver package refused: $problem")
                return failure(problem)
            }
            val meta = GpuDriverHelper.getMetadataFromZip(staged)
            if (meta.name == null || meta.libraryName == null) {
                staged.delete()
                return failure("$name has no complete meta.json, so it is not a driver package Harbor can load. Choose an AdrenoTools driver .zip.")
            }
            arrayOf(
                staged.path, meta.name!!, meta.version ?: "", meta.vendor ?: "", meta.libraryName!!,
                meta.minApi.toString(), ""
            )
        } catch (e: IOException) {
            source.delete()
            staged.delete()
            failure("The driver could not be read: ${e.message}")
        }
    }

    /** Writes the files beside `source`'s meta.json to `target`, flat; returns why not, or null. */
    private fun repackDriver(source: File, target: File, name: String): String? {
        val zip = try {
            ZipFile(source)
        } catch (e: ZipException) {
            return "$name is not a .zip file."
        }
        zip.use {
            val entries = zip.entries().toList().filter { !it.isDirectory }
            val meta = entries.firstOrNull { it.name == "meta.json" || it.name.endsWith("/meta.json") }
                ?: return "$name has no meta.json, so it is not a driver package. Choose an AdrenoTools driver .zip."
            if (meta.size > MAX_META_BYTES) {
                return "The driver package metadata is too large to install."
            }
            val folder = meta.name.removeSuffix("meta.json")
            val files = entries.filter {
                it.name.startsWith(folder) && it.name.length > folder.length &&
                    !it.name.substring(folder.length).contains('/')
            }
            if (files.size > MAX_DRIVER_FILES || files.sumOf { it.size.coerceAtLeast(0) } > MAX_DRIVER_FILES_BYTES) {
                return "The driver package is too large to install."
            }
            val parsed = zip.getInputStream(meta).use { GpuDriverMetadata(it, meta.size) }
            val library = parsed.libraryName ?: return "The meta.json in $name names no driver library."
            if (library.contains('/') || library.contains('\\') || library == "." || library == ".." ||
                files.none { it.name.substring(folder.length) == library }
            ) {
                return "$name does not contain $library, the library its meta.json names."
            }
            // Counted as written: a zip's declared sizes can lie.
            var budget = MAX_DRIVER_FILES_BYTES
            ZipOutputStream(FileOutputStream(target)).use { output ->
                for (file in files) {
                    output.putNextEntry(ZipEntry(file.name.substring(folder.length)))
                    val written = zip.getInputStream(file).use { copyBounded(it, output, budget) }
                    if (written < 0) {
                        return "The driver package is too large to install."
                    }
                    budget -= written
                    output.closeEntry()
                }
            }
        }
        return null
    }

    /** Copies at most `limit` bytes; returns how many, or -1 when there was more. */
    private fun copyBounded(input: InputStream, output: java.io.OutputStream, limit: Long): Long {
        val buffer = ByteArray(64 * 1024)
        var total = 0L
        while (true) {
            val read = input.read(buffer)
            if (read < 0) {
                return total
            }
            total += read
            if (total > limit) {
                return -1
            }
            output.write(buffer, 0, read)
        }
    }

    /** Moves a staged package into Eden's store: [id, name, detail, error]. */
    @JvmStatic
    fun commitDriver(staged: String): Array<String> {
        val source = File(staged)
        val target = File(GpuDriverHelper.driverStoragePath, source.name)
        return try {
            File(GpuDriverHelper.driverStoragePath).mkdirs()
            source.copyTo(target, overwrite = true)
            source.delete()
            val meta = GpuDriverHelper.getMetadataFromZip(target)
            Log.info("[Harbor] driver installed: ${target.name}")
            arrayOf(
                target.name, meta.name ?: target.nameWithoutExtension,
                listOfNotNull(meta.version, meta.vendor).filter { it.isNotBlank() }.joinToString(" · "), ""
            )
        } catch (e: IOException) {
            arrayOf("", "", "", "The driver could not be installed: ${e.message}")
        }
    }

    @JvmStatic
    fun discardDriver(staged: String) {
        File(staged).delete()
    }

    /** Chooses the driver Eden starts with ("" = the phone's); EmulationFragment installs it at launch. */
    @JvmStatic
    fun selectDriver(id: String): Boolean {
        val path = if (id.isEmpty()) "" else GpuDriverHelper.driverStoragePath + id
        if (path.isNotEmpty() && !File(path).exists()) {
            return false
        }
        StringSetting.DRIVER_PATH.setString(path)
        NativeConfig.saveGlobalConfig()
        // Unpacked now, so the console's process (a fresh one) loads it from its first start.
        val installed = if (path.isEmpty()) {
            GpuDriverHelper.installDefaultDriver()
            true
        } else {
            GpuDriverHelper.installCustomDriver(File(path))
        }
        Log.info("[Harbor] graphics driver: ${if (id.isEmpty()) "the phone's" else id}" +
            if (installed) "" else " (could not be unpacked)")
        return installed
    }

    /** Deletes an installed driver from Eden's store; the chosen one stays. */
    @JvmStatic
    fun removeDriver(id: String): Boolean {
        val file = File(GpuDriverHelper.driverStoragePath, File(id).name)
        if (file.path == StringSetting.DRIVER_PATH.getString() || !file.isFile) {
            return false
        }
        val removed = file.delete()
        Log.info("[Harbor] driver ${file.name} ${if (removed) "removed" else "could not be removed"}")
        return removed
    }

    /** "Start Harbor" was pressed and the record saved: the console starts once the wizard returns. */
    @JvmStatic
    fun finishSetup(completed: Boolean) {
        val host = activity ?: return
        host.runOnUiThread { host.onSetupFinished(completed) }
    }

    private fun failure(message: String) = arrayOf("", "", "", "", "", "0", message)

    private fun connect(url: String): HttpsURLConnection {
        val connection = URL(url).openConnection() as? HttpsURLConnection
            ?: throw IOException("Only https downloads are allowed.")
        connection.connectTimeout = 20_000
        connection.readTimeout = 60_000
        connection.instanceFollowRedirects = true
        connection.setRequestProperty("User-Agent", USER_AGENT)
        return connection
    }

    private fun openInput(location: String): InputStream {
        return if (location.startsWith("content://")) {
            YuzuApplication.appContext.contentResolver.openInputStream(Uri.parse(location))
                ?: throw IOException("The file could not be opened.")
        } else {
            File(location).inputStream()
        }
    }

    private fun displayName(location: String): String {
        if (!location.startsWith("content://")) {
            return File(location).name
        }
        return try {
            YuzuApplication.appContext.contentResolver.query(
                Uri.parse(location), arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null
            )?.use { cursor ->
                if (cursor.moveToFirst()) cursor.getString(0) else null
            } ?: "driver.zip"
        } catch (e: Exception) {
            "driver.zip"
        }
    }

    private fun readBounded(input: InputStream, limit: Long): ByteArray {
        val output = java.io.ByteArrayOutputStream()
        val buffer = ByteArray(64 * 1024)
        while (true) {
            val read = input.read(buffer)
            if (read < 0) {
                break
            }
            if (output.size() + read > limit) {
                throw IOException("GitHub's answer is larger than expected.")
            }
            output.write(buffer, 0, read)
        }
        return output.toByteArray()
    }
}

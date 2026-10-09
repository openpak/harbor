// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

package org.yuzu.yuzu_emu.harbor

import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import androidx.annotation.Keep
import java.io.File
import java.security.KeyStore
import java.security.MessageDigest
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec
import org.yuzu.yuzu_emu.YuzuApplication
import org.yuzu.yuzu_emu.utils.Log

/**
 * OpenPak's password store on Android (src/frontend_common/secret_store_android.cpp calls it). Each secret is
 * sealed with AES-256-GCM under one key the Android Keystore generates and never lets out; the app
 * holds only a handle. The ciphertext is a file in the app's no-backup directory, named by a hash of
 * the secret's key, with the key itself as associated data, so a file copied onto another key's name
 * does not open. Nothing is ever written in plain text.
 * Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base), KeystoreSecretBackend.cs.
 */
@Keep
object HarborSecrets {
    private const val ALIAS = "org.openpak.harbor.secrets"
    private const val TRANSFORMATION = "AES/GCM/NoPadding"
    private const val TAG_BITS = 128

    private val directory: File
        get() = File(YuzuApplication.appContext.noBackupFilesDir, "openpak-secrets")

    @Volatile
    private var key: SecretKey? = null

    /** Whether the Keystore gives a working key; false leaves OpenPak's secrets in memory only. */
    @JvmStatic
    @Synchronized
    fun available(): Boolean {
        if (key != null) {
            return true
        }
        return try {
            val store = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
            val existing = store.getKey(ALIAS, null) as? SecretKey
            key = existing ?: KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore")
                .apply {
                    // No user-authentication binding: the secrets must open at boot without a prompt.
                    init(
                        KeyGenParameterSpec.Builder(
                            ALIAS,
                            KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT
                        )
                            .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                            .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                            .setKeySize(256)
                            .build()
                    )
                }.generateKey()
            directory.mkdirs()
            // The native side then checks it with a store/load/erase round trip
            // (frontend_common/secret_store.cpp): some vendor keymasters hand out keys that fail.
            true
        } catch (e: Exception) {
            Log.warning("[Harbor] OpenPak: the Android Keystore is unavailable: ${e.message}")
            key = null
            false
        }
    }

    /**
     * The secret, or null when there is none. A secret that exists but cannot be opened (no
     * Keystore key, a failed read, a changed byte) throws instead: the native side must tell that
     * apart from "none", or it would replace a linked device account with a new one.
     */
    @JvmStatic
    @Synchronized
    fun load(name: String): String? {
        val file = fileOf(name)
        if (!file.exists()) {
            return null
        }
        val secretKey = key ?: throw IllegalStateException("the Keystore key is unavailable")
        return try {
            val blob = file.readBytes()
            val ivLength = blob[0].toInt()
            val cipher = Cipher.getInstance(TRANSFORMATION)
            cipher.init(Cipher.DECRYPT_MODE, secretKey, GCMParameterSpec(TAG_BITS, blob, 1, ivLength))
            cipher.updateAAD(name.toByteArray(Charsets.UTF_8))
            // A wrong key, a changed byte or another key's file throws: no partial plain text.
            val plain = cipher.doFinal(blob, 1 + ivLength, blob.size - 1 - ivLength)
            String(plain, Charsets.UTF_8).also { plain.fill(0) }
        } catch (e: Exception) {
            Log.warning("[Harbor] OpenPak: a secret could not be opened")
            throw e
        }
    }

    @JvmStatic
    @Synchronized
    fun store(name: String, secret: String): Boolean {
        val secretKey = key ?: return false
        val plain = secret.toByteArray(Charsets.UTF_8)
        return try {
            // The Keystore picks a fresh random IV for every encryption.
            val cipher = Cipher.getInstance(TRANSFORMATION)
            cipher.init(Cipher.ENCRYPT_MODE, secretKey)
            cipher.updateAAD(name.toByteArray(Charsets.UTF_8))
            val sealed = cipher.doFinal(plain)
            val iv = cipher.iv
            val blob = ByteArray(1 + iv.size + sealed.size)
            blob[0] = iv.size.toByte()
            iv.copyInto(blob, 1)
            sealed.copyInto(blob, 1 + iv.size)
            directory.mkdirs()
            val file = fileOf(name)
            val temporary = File(file.path + ".tmp")
            temporary.writeBytes(blob)
            temporary.renameTo(file)
        } catch (e: Exception) {
            Log.warning("[Harbor] OpenPak: a secret could not be kept")
            false
        } finally {
            plain.fill(0)
        }
    }

    @JvmStatic
    @Synchronized
    fun erase(name: String) {
        fileOf(name).delete()
    }

    private fun fileOf(name: String): File {
        val hash = MessageDigest.getInstance("SHA-256").digest(name.toByteArray(Charsets.UTF_8))
        return File(directory, hash.joinToString("") { "%02x".format(it) } + ".bin")
    }
}

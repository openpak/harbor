// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

package org.yuzu.yuzu_emu.harbor

import android.annotation.SuppressLint
import android.content.Intent
import android.media.AudioManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.OpenableColumns
import android.view.WindowManager
import android.window.OnBackInvokedDispatcher
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.documentfile.provider.DocumentFile
import org.libsdl.app.BorealisHandler
import org.libsdl.app.PlatformUtils
import org.libsdl.app.SDL
import org.libsdl.app.SDLActivity
import org.yuzu.yuzu_emu.utils.Log

/**
 * Harbor's setup wizard. SDL runs SDL_main from libyuzu-android (jni/harbor_setup_android.cpp) on its
 * own thread: the Borealis wizard, drawn with GLES on the SDL surface, using the same SDL Eden's audio
 * uses. When the wizard returns, SDL finishes this activity; on "Start Harbor" the console is started
 * first, so no other screen shows in between.
 *
 * SDL allows one run per process (it keeps static state and exits the process when an SDLActivity is
 * created again), so setting up again restarts the process (HarborConsole.openSetup).
 */
class HarborSetupActivity : SDLActivity() {
    private var pendingPick = 0
    private var finishedSetup = false

    override fun getLibraries(): Array<String> = arrayOf("yuzu-android")

    override fun onCreate(savedInstanceState: Bundle?) {
        HarborNative.initialize()
        HarborSetupBridge.activity = this
        HarborSetupBridge.ran = true
        super.onCreate(savedInstanceState)
        PlatformUtils.borealisHandler = BorealisHandler()
        window.attributes = window.attributes.apply {
            layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
        }
        hideSystemBars()
        // The volume keys set the media volume (the setup's step sounds), as on the console screen.
        volumeControlStream = AudioManager.STREAM_MUSIC
        // Android 13 and later deliver back here (the manifest opts into predictive back), not to
        // onBackPressed: the wizard's Back, which on Welcome leaves the app.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            onBackInvokedDispatcher.registerOnBackInvokedCallback(OnBackInvokedDispatcher.PRIORITY_DEFAULT) {
                HarborNative.onBackPressed()
            }
        }
        Log.info("[Harbor] setup activity created")
    }

    override fun onResume() {
        super.onResume()
        hideSystemBars()
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) {
            // Again after a picker or the permission screen: the setup has no system bars.
            hideSystemBars()
        }
    }

    @SuppressLint("MissingSuperCall")
    @Deprecated("Android back is the wizard's own Back")
    override fun onBackPressed() {
        HarborNative.onBackPressed()
    }

    fun pickFile(requestId: Int) {
        pendingPick = requestId
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT)
            .addCategory(Intent.CATEGORY_OPENABLE)
            .setType("*/*")
        startPicker(intent, PICK_FILE)
    }

    fun pickFolder(requestId: Int) {
        pendingPick = requestId
        startPicker(Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), PICK_FOLDER)
    }

    @Suppress("DEPRECATION")
    private fun startPicker(intent: Intent, requestCode: Int) {
        try {
            startActivityForResult(intent, requestCode)
        } catch (e: Exception) {
            Log.warning("[Harbor] picker could not open: ${e.javaClass.simpleName}")
            // The wizard says so and keeps the earlier choice.
            HarborNative.onPickFailed(pendingPick, e.message ?: e.javaClass.simpleName)
            pendingPick = 0
        }
    }

    @Deprecated("SDLActivity still uses onActivityResult")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        if (requestCode != PICK_FILE && requestCode != PICK_FOLDER) {
            @Suppress("DEPRECATION")
            super.onActivityResult(requestCode, resultCode, data)
            return
        }
        val requestId = pendingPick
        pendingPick = 0
        val uri = data?.data
        if (resultCode != RESULT_OK || uri == null) {
            HarborNative.onPicked(requestId, null, null)
            return
        }
        if (requestCode == PICK_FOLDER) {
            // Eden reads game, DLC and update folders by this tree URI at every start.
            try {
                contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
            } catch (e: SecurityException) {
                Log.warning("[Harbor] folder permission not kept: ${e.message}")
            }
            val name = DocumentFile.fromTreeUri(this, uri)?.name
            HarborNative.onPicked(requestId, uri.toString(), name)
        } else {
            HarborNative.onPicked(requestId, uri.toString(), displayName(uri))
        }
    }

    /**
     * Called by the bridge when "Start Harbor" saved the record: the console starts in a new process
     * (HarborConsole.bootAfterSetup), which ends this one.
     */
    fun onSetupFinished(completed: Boolean) {
        if (!completed || finishedSetup) {
            return
        }
        finishedSetup = true
        HarborConsole.bootAfterSetup(this)
        @Suppress("DEPRECATION")
        overridePendingTransition(0, 0)
    }

    override fun onDestroy() {
        super.onDestroy()
        HarborSetupBridge.activity = null
        // As before the wizard ran: Eden's own SDL use (audio) never had an SDL activity as context.
        SDL.setContext(null)
        Log.info("[Harbor] setup activity destroyed")
    }

    private fun hideSystemBars() {
        WindowCompat.setDecorFitsSystemWindows(window, false)
        WindowInsetsControllerCompat(window, window.decorView).apply {
            hide(WindowInsetsCompat.Type.systemBars())
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
    }

    private fun displayName(uri: Uri): String? = try {
        contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use {
            if (it.moveToFirst()) it.getString(0) else null
        }
    } catch (e: Exception) {
        null
    }

    companion object {
        private const val PICK_FILE = 0x4801
        private const val PICK_FOLDER = 0x4802
    }
}

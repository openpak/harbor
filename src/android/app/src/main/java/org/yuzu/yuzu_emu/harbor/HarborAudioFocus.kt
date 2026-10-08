// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

package org.yuzu.yuzu_emu.harbor

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioManager
import org.yuzu.yuzu_emu.YuzuApplication
import org.yuzu.yuzu_emu.utils.Log

/**
 * Android's audio focus for the console's sound: held while the console is on screen, given back
 * while it is paused. Another app taking focus mutes the console (a call, a voice note, music
 * started elsewhere), a notification that asks to duck lowers it to 25 %, and focus coming back
 * restores it. A permanent loss is asked for again at the next resume. The console keeps running
 * either way; only its output gain changes (HarborNative.setHostGain).
 * Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base), HarborAudioFocus.cs.
 */
object HarborAudioFocus : AudioManager.OnAudioFocusChangeListener {
    private const val DUCKED_GAIN = 0.25f

    private val manager: AudioManager? by lazy {
        YuzuApplication.appContext.getSystemService(Context.AUDIO_SERVICE) as? AudioManager
    }

    // Ducking is ours to do: the system cannot duck an AAudio low-latency stream.
    private val request: AudioFocusRequest by lazy {
        AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_GAME)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                    .build()
            )
            .setAcceptsDelayedFocusGain(true)
            .setWillPauseWhenDucked(true)
            .setOnAudioFocusChangeListener(this)
            .build()
    }

    private var held = false

    @Synchronized
    fun acquire() {
        val audio = manager ?: return
        if (held) {
            return
        }
        held = true
        val result = audio.requestAudioFocus(request)
        // Delayed: something (a call) holds it now; the listener hears when it comes.
        HarborNative.setHostGain(if (result == AudioManager.AUDIOFOCUS_REQUEST_GRANTED) 1f else 0f)
        Log.info("[Harbor] audio focus ${resultName(result)}")
    }

    @Synchronized
    fun release() {
        val audio = manager ?: return
        if (!held) {
            return
        }
        held = false
        audio.abandonAudioFocusRequest(request)
    }

    override fun onAudioFocusChange(focusChange: Int) {
        val gain = when (focusChange) {
            AudioManager.AUDIOFOCUS_GAIN,
            AudioManager.AUDIOFOCUS_GAIN_TRANSIENT,
            AudioManager.AUDIOFOCUS_GAIN_TRANSIENT_MAY_DUCK,
            AudioManager.AUDIOFOCUS_GAIN_TRANSIENT_EXCLUSIVE -> 1f
            AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK -> DUCKED_GAIN
            else -> 0f
        }
        Log.info("[Harbor] audio focus changed: $focusChange, gain $gain")
        HarborNative.setHostGain(gain)
        // A permanent loss does not come back by itself: the next resume of the console asks again.
        if (focusChange == AudioManager.AUDIOFOCUS_LOSS) {
            release()
        }
    }

    private fun resultName(result: Int): String = when (result) {
        AudioManager.AUDIOFOCUS_REQUEST_GRANTED -> "granted"
        AudioManager.AUDIOFOCUS_REQUEST_DELAYED -> "delayed"
        else -> "failed"
    }
}

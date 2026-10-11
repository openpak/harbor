// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

package org.yuzu.yuzu_emu.harbor

import android.content.Context
import android.view.Gravity
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.widget.FrameLayout

/**
 * The surface a Harbor host page (harbor_setup/menu.h, MenuShowHostPage) is drawn into, kept at
 * 16:9; touches go to the page. Used by HarborScreenActivity's screens and, while the console
 * starts or stops, laid over the console's own screen.
 */
class HarborScreenView(context: Context) : SurfaceView(context), SurfaceHolder.Callback {
    init {
        holder.addCallback(this)
        isFocusable = true
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val width = MeasureSpec.getSize(widthMeasureSpec)
        val height = MeasureSpec.getSize(heightMeasureSpec)
        val fitted = if (width * 9 > height * 16) {
            Pair(height * 16 / 9, height)
        } else {
            Pair(width, width * 9 / 16)
        }
        setMeasuredDimension(fitted.first, fitted.second)
    }

    // One surface takes the pages at a time: the newest view's. A view that goes lets go of the
    // surface only when it is its own (the launcher's view can go after the console's appeared).
    override fun surfaceCreated(holder: SurfaceHolder) {
        owner = this
        HarborNative.hostSurface(holder.surface)
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        owner = this
        HarborNative.hostSurface(holder.surface)
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        if (owner === this) {
            owner = null
            HarborNative.hostSurface(null)
        }
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        val w = width.toFloat().coerceAtLeast(1f)
        val h = height.toFloat().coerceAtLeast(1f)
        val action = event.actionMasked
        for (i in 0 until event.pointerCount) {
            val up = action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL ||
                (action == MotionEvent.ACTION_POINTER_UP && i == event.actionIndex)
            HarborNative.hostTouch(event.getPointerId(i), event.getX(i) / w, event.getY(i) / h, !up)
        }
        return true
    }

    companion object {
        private var owner: HarborScreenView? = null

        /**
         * The view in the middle of a frame in the console's paper colour (the bars around the
         * 16:9 page), ready to be a screen's content or laid over the console's.
         */
        fun frame(context: Context): FrameLayout {
            val dark = HarborTheme.dark(context)
            return FrameLayout(context).apply {
                setBackgroundColor(if (dark) HarborTheme.PAPER_DARK else HarborTheme.PAPER_LIGHT)
                addView(
                    HarborScreenView(context),
                    FrameLayout.LayoutParams(
                        FrameLayout.LayoutParams.MATCH_PARENT,
                        FrameLayout.LayoutParams.MATCH_PARENT,
                        Gravity.CENTER
                    )
                )
            }
        }
    }
}

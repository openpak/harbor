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

    override fun surfaceCreated(holder: SurfaceHolder) {
        HarborNative.hostSurface(holder.surface)
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        HarborNative.hostSurface(holder.surface)
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        HarborNative.hostSurface(null)
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
        /**
         * The view in the middle of a black frame (the bars around the 16:9 page, as around the
         * console's own picture), ready to be a screen's content or laid over the console's.
         */
        fun frame(context: Context): FrameLayout {
            return FrameLayout(context).apply {
                setBackgroundColor(android.graphics.Color.BLACK)
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

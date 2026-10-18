// SPDX-License-Identifier: GPL-2.0-or-later
package org.citra.citra_emu.utils

import android.content.Context
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.view.View
import org.citra.citra_emu.R

/** Shared mask artwork, contained in the former clock area beside the launcher menu. */
class DekuiLauncherBackdrop(context: Context) : View(context) {
    private val artwork = BitmapFactory.decodeResource(resources, R.drawable.dekui_launcher_masks)
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG or Paint.FILTER_BITMAP_FLAG)
    init { importantForAccessibility = IMPORTANT_FOR_ACCESSIBILITY_NO }
    override fun onDraw(canvas: Canvas) {
        canvas.drawColor(Color.rgb(8, 7, 13))
        artwork?.let {
            val scale = minOf(width * .56f / it.width, height * .78f / it.height)
            val w = it.width * scale
            val h = it.height * scale
            val cx = width * .30f
            val cy = height * .55f
            canvas.drawBitmap(it, null, RectF(cx-w/2, cy-h/2, cx+w/2, cy+h/2), paint)
        }
    }
}

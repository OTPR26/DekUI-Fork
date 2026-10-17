// SPDX-License-Identifier: GPL-2.0-or-later
package org.citra.citra_emu.utils

import android.content.Context
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.Shader
import android.view.View
import org.citra.citra_emu.R

/** Fierce Deity launcher illustration with a quiet menu area. */
class DekuiLauncherBackdrop(context: Context) : View(context) {
    private val artwork=BitmapFactory.decodeResource(resources,R.drawable.dekui_launcher_fierce_deity)
    private val paint=Paint(Paint.ANTI_ALIAS_FLAG or Paint.FILTER_BITMAP_FLAG)
    private val shade=Paint()
    init { importantForAccessibility=IMPORTANT_FOR_ACCESSIBILITY_NO }
    override fun onSizeChanged(w: Int,h: Int,oldw: Int,oldh: Int) {
        shade.shader=LinearGradient(w*.5f,0f,w.toFloat(),0f,
            intArrayOf(Color.TRANSPARENT,0x8F08070D.toInt(),0xD108070D.toInt()),
            floatArrayOf(0f,.48f,1f),Shader.TileMode.CLAMP)
    }
    override fun onDraw(canvas: Canvas) {
        canvas.drawColor(Color.BLACK)
        artwork?.let {
            val scale=maxOf(width.toFloat()/it.width,height.toFloat()/it.height)
            val w=it.width*scale;val h=it.height*scale
            val offsetY=0f
            canvas.drawBitmap(it,null,RectF((width-w)/2,(height-h)/2+offsetY,
                (width+w)/2,(height+h)/2+offsetY),paint)
        }
        canvas.drawRect(width*.5f,0f,width.toFloat(),height.toFloat(),shade)
    }
}

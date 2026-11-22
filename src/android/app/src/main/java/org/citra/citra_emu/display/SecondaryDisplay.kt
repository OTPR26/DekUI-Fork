// Copyright 2025-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

package org.citra.citra_emu.display

import android.app.Presentation
import android.content.Context
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.media.MediaRouter
import android.os.Build
import android.os.Bundle
import android.view.Display
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.WindowManager
import org.citra.citra_emu.NativeLibrary
import org.citra.citra_emu.features.settings.model.BooleanSetting
import org.citra.citra_emu.features.settings.model.IntSetting
import org.citra.citra_emu.utils.Log

class SecondaryDisplay(val context: Context, private val onCastModeChanged: () -> Unit = {}) :
    DisplayManager.DisplayListener {
    private var pres: SecondaryDisplayPresentation? = null
    private val displayManager = context.getSystemService(Context.DISPLAY_SERVICE) as DisplayManager
    private val castRouter = context.getSystemService(Context.MEDIA_ROUTER_SERVICE) as? MediaRouter
    private var preferredCastMode = ReMaskedCast.OFF
    var activeCastMode = ReMaskedCast.OFF
        private set
    private var foreground = true
    private val castRoutes = object : MediaRouter.SimpleCallback() {
        override fun onRouteSelected(router: MediaRouter, type: Int, info: MediaRouter.RouteInfo) = updateDisplay()
        override fun onRouteUnselected(router: MediaRouter, type: Int, info: MediaRouter.RouteInfo) = updateDisplay()
        override fun onRoutePresentationDisplayChanged(router: MediaRouter, info: MediaRouter.RouteInfo) = updateDisplay()
    }
    private val vd: VirtualDisplay
    var preferredDisplayId = -1
    var currentDisplayId = -1

    val availableDisplays: List<Display>
        get() = getSecondaryDisplays()

    init {
        vd = displayManager.createVirtualDisplay(
            "HiddenDisplay",
            1920,
            1080,
            320,
            null,
            DisplayManager.VIRTUAL_DISPLAY_FLAG_PRESENTATION
        )
        displayManager.registerDisplayListener(this, null)
        if (org.citra.citra_emu.BuildConfig.DEKUI_PRESENTATION)
            castRouter?.addCallback(MediaRouter.ROUTE_TYPE_LIVE_VIDEO, castRoutes)
    }

    fun updateSurface(presentation: SecondaryDisplayPresentation) {
        if (pres !== presentation || !foreground) return
        val surface = presentation.getSurfaceHolder().surface
        if (surface.isValid) {
            val modeChanged = activeCastMode != preferredCastMode
            if (modeChanged) NativeLibrary.setReMaskedCastMode(preferredCastMode)
            NativeLibrary.secondarySurfaceChanged(surface)
            activeCastMode = preferredCastMode
            if (modeChanged) onCastModeChanged()
        } else {
            Log.warning("SecondaryDisplay Attempted to update null or invalid surface")
        }
    }

    fun destroySurface(presentation: SecondaryDisplayPresentation) {
        if (pres !== presentation) return
        NativeLibrary.secondarySurfaceDestroyed()
        clearCastMode()
    }

    private fun clearCastMode() {
        if (activeCastMode == ReMaskedCast.OFF) return
        activeCastMode = ReMaskedCast.OFF
        NativeLibrary.setReMaskedCastMode(ReMaskedCast.OFF)
        onCastModeChanged()
    }

    fun setForeground(value: Boolean) {
        foreground = value
        if (value) updateDisplay() else releasePresentation()
    }

    private fun getSecondaryDisplays(): List<Display> {
        val dm = context.getSystemService(Context.DISPLAY_SERVICE) as DisplayManager
        val currentDisplayId = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            context.display.displayId
        } else {
            @Suppress("DEPRECATION")
            (context.getSystemService(Context.WINDOW_SERVICE) as WindowManager)
                .defaultDisplay.displayId
        }
        val displays = dm.displays
        val presDisplays = dm.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION)
        return displays.filter {
            val isPresentable = presDisplays.any { pd -> pd.displayId == it.displayId }
            val isNotDefaultOrPresentable =
                (it != null && it.displayId != Display.DEFAULT_DISPLAY) || isPresentable

            isNotDefaultOrPresentable &&
                it.displayId != currentDisplayId &&
                it.name != "HiddenDisplay" &&
                it.state != Display.STATE_OFF &&
                it.isValid
        }
    }

    fun updateDisplay() {
        if (!foreground || context is android.app.Activity && (context.isFinishing || context.isDestroyed)) {
            return
        }

        val isReMasked = org.citra.citra_emu.BuildConfig.DEKUI_PRESENTATION
        val castMode = if (isReMasked) ReMaskedCast.mode(context) else ReMaskedCast.OFF
        val castDisplay = if (castMode == ReMaskedCast.OFF) null else {
            val primary = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) context.display?.displayId
                else Display.DEFAULT_DISPLAY
            ReMaskedCast.display(castRouter, primary ?: Display.DEFAULT_DISPLAY)
        }
        preferredCastMode = if (castDisplay == null) ReMaskedCast.OFF else castMode

        // DekUI presents one authored top-screen image. The emulator's hidden
        // fallback display must not receive a second, bottom-only render pass.
        if (isReMasked && castDisplay == null &&
            (availableDisplays.isEmpty() ||
             IntSetting.SECONDARY_DISPLAY_LAYOUT.int == SecondaryDisplayLayout.NONE.int ||
             !BooleanSetting.ENABLE_SECONDARY_DISPLAY.boolean)) {
            currentDisplayId = -1
            releasePresentation()
            return
        }

        val displayToUse = if (castDisplay != null) {
            currentDisplayId = castDisplay.displayId
            castDisplay
        } else if (availableDisplays.isEmpty() ||
            // Theoretically, the NONE option is no longer selectable, but
            // I am leaving this in for backwards compatibility
            IntSetting.SECONDARY_DISPLAY_LAYOUT.int == SecondaryDisplayLayout.NONE.int ||
            !BooleanSetting.ENABLE_SECONDARY_DISPLAY.boolean
        ) {
            currentDisplayId = -1
            vd.display
        } else if (preferredDisplayId >= 0 &&
            availableDisplays.any { it.displayId == preferredDisplayId }
        ) {
            currentDisplayId = preferredDisplayId
            availableDisplays.first { it.displayId == preferredDisplayId }
        } else {
            val dm = context.getSystemService(Context.DISPLAY_SERVICE) as DisplayManager
            val default = dm.displays.first { it.displayId == Display.DEFAULT_DISPLAY }
            // prioritize displays that have a different name from the default display, as
            // some devices create a permanent virtual display with the same
            // name as the default display that should be skipped in most cases
            currentDisplayId = availableDisplays.firstOrNull {
                it.name != default.name && !it.name.contains("Built", true)
            }?.displayId
                ?: availableDisplays[0].displayId
            availableDisplays.first { it.displayId == currentDisplayId }
        }

        // if our presentation is already on the right display, ignore
        val current = pres
        if (current != null && current.display == displayToUse) {
            if (activeCastMode != preferredCastMode && current.isShowing &&
                current.hasSurface()) updateSurface(current)
            return
        }

        // otherwise, make a new presentation
        releasePresentation()

        try {
            pres = SecondaryDisplayPresentation(context, displayToUse!!, this)
            if (castDisplay != null) pres?.window?.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            pres?.show()
        }
        // catch BadTokenException and InvalidDisplayException,
        // the display became invalid asynchronously, so we can assign to null
        // until onDisplayAdded/Removed/Changed is called and logic retriggered
        catch (_: WindowManager.BadTokenException) {
            pres = null
        } catch (_: WindowManager.InvalidDisplayException) {
            pres = null
        }
    }

    fun releasePresentation() {
        val previous = pres
        pres = null
        if (previous != null) {
            try {
                previous.dismiss()
            } catch (_: Exception) { }
            NativeLibrary.secondarySurfaceDestroyed()
        }
        clearCastMode()
    }

    fun releaseVD() {
        displayManager.unregisterDisplayListener(this)
        if (org.citra.citra_emu.BuildConfig.DEKUI_PRESENTATION) castRouter?.removeCallback(castRoutes)
        vd.release()
    }

    override fun onDisplayAdded(displayId: Int) {
        updateDisplay()
    }

    override fun onDisplayRemoved(displayId: Int) {
        updateDisplay()
    }
    override fun onDisplayChanged(displayId: Int) {
        updateDisplay()
    }
}
class SecondaryDisplayPresentation(
    context: Context,
    display: Display,
    val parent: SecondaryDisplay
) : Presentation(context, display) {
    private lateinit var surfaceView: SurfaceView
    fun hasSurface(): Boolean = ::surfaceView.isInitialized && surfaceView.holder.surface.isValid

    private var touchscreenPointerId = -1

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window?.setFlags(
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
                WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
                WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL
        )

        // Initialize SurfaceView
        surfaceView = SurfaceView(context)
        surfaceView.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {
                Log.debug("SecondaryDisplay Surface created")
            }

            override fun surfaceChanged(
                holder: SurfaceHolder,
                format: Int,
                width: Int,
                height: Int
            ) {
                Log.debug("SecondaryDisplay Surface changed: ${width}x$height")
                parent.updateSurface(this@SecondaryDisplayPresentation)
            }

            override fun surfaceDestroyed(holder: SurfaceHolder) {
                Log.debug("SecondaryDisplay Surface destroyed")
                parent.destroySurface(this@SecondaryDisplayPresentation)
            }
        })

        this.surfaceView.setOnTouchListener { _, event ->
            if (parent.activeCastMode != ReMaskedCast.OFF) return@setOnTouchListener false

            val pointerIndex = event.actionIndex
            val pointerId = event.getPointerId(pointerIndex)
            when (event.actionMasked) {
                MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                    if (touchscreenPointerId == -1) {
                        touchscreenPointerId = pointerId
                        NativeLibrary.onSecondaryTouchEvent(
                            event.getX(pointerIndex),
                            event.getY(pointerIndex),
                            true
                        )
                    }
                }

                MotionEvent.ACTION_MOVE -> {
                    val index = event.findPointerIndex(touchscreenPointerId)
                    if (index != -1) {
                        NativeLibrary.onSecondaryTouchMoved(
                            event.getX(index),
                            event.getY(index)
                        )
                    }
                }

                MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP, MotionEvent.ACTION_CANCEL -> {
                    if (pointerId == touchscreenPointerId) {
                        NativeLibrary.onSecondaryTouchEvent(0f, 0f, false)
                        touchscreenPointerId = -1
                    }
                }
            }
            true
        }

        setContentView(surfaceView) // Set SurfaceView as content
    }

    // Publicly accessible method to get the SurfaceHolder
    fun getSurfaceHolder(): SurfaceHolder = surfaceView.holder
}

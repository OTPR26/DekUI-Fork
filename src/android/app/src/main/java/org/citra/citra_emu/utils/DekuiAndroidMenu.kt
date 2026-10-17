// SPDX-License-Identifier: GPL-2.0-or-later
package org.citra.citra_emu.utils

import android.view.KeyEvent
import android.view.MotionEvent
import androidx.preference.PreferenceManager
import org.citra.citra_emu.CitraApplication
import org.citra.citra_emu.NativeLibrary
import org.citra.citra_emu.features.settings.model.AbstractStringSetting
import org.citra.citra_emu.features.settings.model.Settings
import org.citra.citra_emu.features.settings.model.view.InputBindingSetting

/** Controller capture stays in DekUI; mappings use the same persistent Android input backend. */
object DekuiAndroidMenu {
    private val prefs get() = PreferenceManager.getDefaultSharedPreferences(CitraApplication.appContext)
    val keys = Settings.buttonKeys + Settings.triggerKeys + Settings.dPadButtonKeys +
        Settings.circlePadKeys + Settings.cStickKeys + Settings.dPadAxisKeys
    var launcherCapture=false
    var capture = -1
        private set
    private var status = ""
    private fun setting(index: Int): InputBindingSetting {
        val key = keys[index]
        val value = object : AbstractStringSetting {
            override var string: String
                get() = prefs.getString(key, "") ?: ""
                set(value) { prefs.edit().putString(key,value).apply() }
            override val key = key
            override val section = Settings.SECTION_CONTROLS
            override val isRuntimeEditable = true
            override val valueAsString get() = string
            override val defaultValue = ""
        }
        return InputBindingSetting(value,0)
    }
    fun begin(index: Int) { if (index in keys.indices) { capture=index;status="Press a controller button or move the chosen axis." } }
    fun cancel() { capture=-1;status="Binding cancelled." }
    fun clear(index: Int) {
        if (index !in keys.indices) return
        setting(index).removeOldMapping()
        prefs.edit().remove(keys[index]).apply()
        capture=-1;status="Binding cleared."
    }
    fun onKey(event: KeyEvent): Boolean {
        if (capture < 0) return false
        if (!launcherCapture && !NativeLibrary.isDekuiSettingsOpen()) { cancel();return false }
        if (event.action != KeyEvent.ACTION_DOWN || event.repeatCount > 0) return true
        if (event.keyCode == KeyEvent.KEYCODE_BACK) { cancel();return true }
        val item=setting(capture)
        if (item.isButtonMappingSupported() && event.device != null) {
            item.onKeyInput(event);capture=-1;status="Binding saved."
        }
        return true
    }
    fun onMotion(event: MotionEvent): Boolean {
        if (capture < 0) return false
        if (!launcherCapture && !NativeLibrary.isDekuiSettingsOpen()) { cancel();return false }
        val item=setting(capture)
        if (!item.isAxisMappingSupported()) return true
        val device=event.device ?: return true
        val range=device.motionRanges.firstOrNull { kotlin.math.abs(event.getAxisValue(it.axis)) > 0.65f }
            ?: return true
        val direction=if(event.getAxisValue(range.axis)<0) '-' else '+'
        item.onMotionInput(device,range,direction);capture=-1;status="Binding saved."
        return true
    }
    // Both menus resolve logical A/B through the same saved game bindings.
    fun navigateKey(event: KeyEvent) {
        val action=DekuiMenuInput.action(event)
        if(action==DekuiMenuInput.Action.Back || action==DekuiMenuInput.Action.Start) {
            if(event.action==KeyEvent.ACTION_DOWN && event.repeatCount==0)NativeLibrary.closeDekuiSettings()
            return
        }
        val key=when(action) {
            DekuiMenuInput.Action.Up -> 0
            DekuiMenuInput.Action.Down -> 1
            DekuiMenuInput.Action.Left -> 2
            DekuiMenuInput.Action.Right -> 3
            DekuiMenuInput.Action.Confirm -> 4
            DekuiMenuInput.Action.Previous -> 5
            DekuiMenuInput.Action.Next -> 6
            else -> when(event.keyCode) {
                KeyEvent.KEYCODE_ENTER,KeyEvent.KEYCODE_DPAD_CENTER -> 4
                else -> return
            }
        }
        if(event.repeatCount==0)NativeLibrary.onDekuiMenuKey(key,event.action==KeyEvent.ACTION_DOWN)
    }
    fun navigateMotion(event: MotionEvent) {
        val x=event.getAxisValue(MotionEvent.AXIS_HAT_X).takeIf { kotlin.math.abs(it)>.5f }
            ?: event.getAxisValue(MotionEvent.AXIS_X)
        val y=event.getAxisValue(MotionEvent.AXIS_HAT_Y).takeIf { kotlin.math.abs(it)>.5f }
            ?: event.getAxisValue(MotionEvent.AXIS_Y)
        NativeLibrary.onDekuiMenuKey(0,y<-.55f)
        NativeLibrary.onDekuiMenuKey(1,y>.55f)
        NativeLibrary.onDekuiMenuKey(2,x<-.55f)
        NativeLibrary.onDekuiMenuKey(3,x>.55f)
        val scroll=event.getAxisValue(MotionEvent.AXIS_RZ)
        if(kotlin.math.abs(scroll)>.25f)NativeLibrary.onDekuiScroll(.75f,.55f,-scroll*.22f)
    }
    fun sync() {
        NativeLibrary.updateDekuiAndroidUi(keys.map { (prefs.getString(it, "") ?: "").substringAfter(": ") }.toTypedArray(),
            DekuiTextureStorage.description(),status,capture,EmulationMenuSettings.showOverlay,
            prefs.getInt("controlOpacity",50),prefs.getInt("controlScale",50),EmulationMenuSettings.hapticFeedback)
    }
}

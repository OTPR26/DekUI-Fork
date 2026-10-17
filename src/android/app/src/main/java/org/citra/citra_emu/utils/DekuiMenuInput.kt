// SPDX-License-Identifier: GPL-2.0-or-later
package org.citra.citra_emu.utils

import android.view.KeyEvent
import org.citra.citra_emu.NativeLibrary
import org.citra.citra_emu.features.settings.model.view.InputBindingSetting

/** Read the current game bindings on every event; never infer a controller layout. */
object DekuiMenuInput {
    enum class Action { Confirm, Back, Start, Up, Down, Left, Right, Previous, Next }

    fun action(event: KeyEvent): Action? {
        val buttons=InputBindingSetting.getButtonSet(event)
        return when {
            NativeLibrary.ButtonType.BUTTON_A in buttons -> Action.Confirm
            NativeLibrary.ButtonType.BUTTON_B in buttons -> Action.Back
            NativeLibrary.ButtonType.BUTTON_START in buttons -> Action.Start
            NativeLibrary.ButtonType.DPAD_UP in buttons -> Action.Up
            NativeLibrary.ButtonType.DPAD_DOWN in buttons -> Action.Down
            NativeLibrary.ButtonType.DPAD_LEFT in buttons -> Action.Left
            NativeLibrary.ButtonType.DPAD_RIGHT in buttons -> Action.Right
            NativeLibrary.ButtonType.BUTTON_ZL in buttons -> Action.Previous
            NativeLibrary.ButtonType.BUTTON_ZR in buttons -> Action.Next
            else -> null
        }
    }

    fun direction(action: Action?): Int? = when(action) {
        Action.Up -> KeyEvent.KEYCODE_DPAD_UP
        Action.Down -> KeyEvent.KEYCODE_DPAD_DOWN
        Action.Left -> KeyEvent.KEYCODE_DPAD_LEFT
        Action.Right -> KeyEvent.KEYCODE_DPAD_RIGHT
        else -> null
    }
}

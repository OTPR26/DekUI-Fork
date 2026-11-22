// SPDX-License-Identifier: GPL-2.0-or-later
package org.citra.citra_emu.display

import android.content.Context
import android.media.MediaRouter
import android.view.Display
import androidx.preference.PreferenceManager

/** The system's selected external video route. A built-in lower panel is independent of casting. */
object ReMaskedCast {
    const val OFF = 0
    const val SINGLE_SCREEN = 1
    const val TOP_SCREEN = 2
    private const val KEY = "ReMaskedCastLayout"

    fun mode(context: Context): Int =
        PreferenceManager.getDefaultSharedPreferences(context).getInt(KEY, OFF).coerceIn(OFF, TOP_SCREEN)

    fun setMode(context: Context, mode: Int) {
        require(mode in OFF..TOP_SCREEN)
        PreferenceManager.getDefaultSharedPreferences(context).edit().putInt(KEY, mode).apply()
    }

    fun display(router: MediaRouter?, primaryDisplayId: Int): Display? =
        router?.getSelectedRoute(MediaRouter.ROUTE_TYPE_LIVE_VIDEO)?.presentationDisplay
            ?.takeIf { it.displayId != primaryDisplayId && it.isValid && it.state == Display.STATE_ON }
}

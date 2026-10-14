// SPDX-License-Identifier: GPL-2.0-or-later
package org.citra.citra_emu.features.settings.model

import org.citra.citra_emu.NativeLibrary
import org.citra.citra_emu.features.settings.utils.SettingsFile

/** UI-thread bridge; the renderer only queues atomic requests. */
object DekuiSettingsBridge {
    private var pending: IntArray? = null
    var saveFailed = false
        private set

    fun poll(): Int {
        val update = NativeLibrary.pollDekuiSettings()
        if (update != null && update[0] >= 0) pending = update
        pending?.let { values ->
            val settings = listOf<AbstractSetting>(
                IntSetting.RESOLUTION_FACTOR.apply { int = values[0] },
                ScaledFloatSetting.AUDIO_VOLUME.apply { float = values[1].toFloat() },
                BooleanSetting.VSYNC.apply { boolean = values[2] != 0 },
                BooleanSetting.DISK_SHADER_CACHE.apply { boolean = values[3] != 0 },
                BooleanSetting.CUSTOM_TEXTURES.apply { boolean = values[4] != 0 },
                BooleanSetting.HW_SHADER.apply { boolean = values[5] != 0 },
                BooleanSetting.ASYNC_SHADERS.apply { boolean = values[6] != 0 }
            )
            saveFailed = !SettingsFile.saveDekuiSettings(settings)
            if (!saveFailed) pending = null // Failed writes are retried on the next poll.
        }
        return update?.get(7) ?: 0
    }
}

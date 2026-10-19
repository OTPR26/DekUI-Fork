// SPDX-License-Identifier: GPL-2.0-or-later
package org.citra.citra_emu.utils

import android.content.Context
import android.net.Uri
import androidx.annotation.Keep
import androidx.documentfile.provider.DocumentFile
import com.chaquo.python.Python
import com.chaquo.python.android.AndroidPlatform
import org.citra.citra_emu.NativeLibrary
import org.json.JSONObject
import java.io.File

object DekuiPakSetup {
    @Volatile var generating = false
        private set

    fun canImport(c: Context) = DekuiStorage.game(c).isFile
    fun description(c: Context): String {
        val report = File(DekuiStorage.root(c), "load/mm3d-models/pak-import/import.json")
        if (!report.isFile) return "No PAK model imported."
        return runCatching {
            "Imported model: " + JSONObject(report.readText()).optString("source_name", "Player model").take(256)
        }.getOrDefault("Imported PAK player model")
    }

    @Keep class Bridge(private val report: (String) -> Unit) {
        fun progress(message: String) = report(message)
    }

    @Synchronized fun importModel(c: Context, uri: Uri, progress: (String) -> Unit): String {
        check(!generating && !DekuiAdultLinkSetup.generating && !DekuiTwilightLinkSetup.generating) {
            "Another model is being imported"
        }
        check(!NativeLibrary.isRunning()) { "Return to the startup screen before importing a PAK" }
        check(canImport(c)) { "Set up your MM3D ROM first" }
        val name = DocumentFile.fromSingleUri(c, uri)?.name ?: "model.pak"
        check(name.endsWith(".pak", ignoreCase = true)) { "Select a .pak player-model package" }
        generating = true
        var input: File? = null
        try {
            val selected = File.createTempFile("player-model-", ".pak", c.cacheDir)
            input = selected
            progress("Reading the selected PAK…")
            c.contentResolver.openInputStream(uri)?.use { source ->
                selected.outputStream().use { target ->
                    val buffer = ByteArray(65536)
                    var total = 0L
                    while (true) {
                        val count = source.read(buffer)
                        if (count < 0) break
                        total += count
                        check(total <= 32L * 1024 * 1024) { "Model package exceeds 32 MiB" }
                        target.write(buffer, 0, count)
                    }
                }
            } ?: error("Cannot read the selected PAK")
            if (!Python.isStarted()) Python.start(AndroidPlatform(c.applicationContext))
            val result = JSONObject(Python.getInstance().getModule("zobj_import.android")
                .callAttr("import_model", DekuiStorage.root(c).absolutePath,
                    DekuiStorage.game(c).absolutePath, selected.absolutePath, Bridge(progress)).toString())
            DekuiPresentationStore(c).update(mapOf("pak_link_model" to true,
                "twilight_link_model" to false, "adult_link_model" to false, "dark_link_model" to false))
            return "Imported ${result.getString("name")}. Start the game to use it."
        } finally {
            input?.delete()
            generating = false
        }
    }
}

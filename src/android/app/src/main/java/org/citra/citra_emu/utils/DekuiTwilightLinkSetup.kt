// SPDX-License-Identifier: GPL-2.0-or-later
package org.citra.citra_emu.utils

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.provider.DocumentsContract
import androidx.annotation.Keep
import androidx.documentfile.provider.DocumentFile
import com.chaquo.python.Python
import com.chaquo.python.android.AndroidPlatform
import org.citra.citra_emu.NativeLibrary
import org.citra.citra_emu.features.settings.model.BooleanSetting
import org.citra.citra_emu.features.settings.utils.SettingsFile
import org.json.JSONArray
import org.json.JSONObject
import java.io.File

/** Separate source selections and generated files for the TP appearance mod. */
object DekuiTwilightLinkSetup {
    private fun prefs(c: Context) = c.getSharedPreferences("dekui-twilight-link", Context.MODE_PRIVATE)
    @Volatile var generating = false
        private set

    fun selectRom(c: Context, uri: Uri) {
        c.contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
        check(prefs(c).edit().putString("rom", uri.toString())
            .putString("romLabel", DocumentFile.fromSingleUri(c, uri)?.name ?: "Selected ROM").commit())
    }

    fun selectPack(c: Context, uri: Uri) {
        c.contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
        val folder = DocumentFile.fromTreeUri(c, uri)
        check(folder?.isDirectory == true && folder.canRead()) { "Choose a readable TP texture folder" }
        check(prefs(c).edit().putString("pack", uri.toString())
            .putString("packLabel", folder.name ?: "Selected texture pack").commit())
    }

    fun canGenerate(c: Context) = prefs(c).contains("rom") && DekuiStorage.game(c).isFile
    fun description(c: Context): String {
        val p = prefs(c)
        return "TP GameCube ROM: ${p.getString("romLabel", "Not selected")}\n" +
            "TP textures: ${p.getString("packLabel", "Original ROM textures")}\n" + p.getString("result", "")
    }

    private fun textureIndex(c: Context): JSONObject {
        val result = JSONObject()
        val pack = prefs(c).getString("pack", null) ?: return result
        val pattern = Regex("^tex1_\\d+x\\d+(?:_m)?_([0-9a-fA-F]{16})_\\d+(?:_mip0)?\\.(?:png|dds)$", RegexOption.IGNORE_CASE)
        val seen = mutableSetOf<String>()
        var visited = 0
        fun scan(uri: Uri, depth: Int) {
            check(depth <= 64 && visited++ < 100000) { "Texture folder is too deep or too large" }
            if (!seen.add(uri.toString())) return
            for (doc in FileUtil.listFiles(uri).sortedBy { it.filename }) {
                if (doc.mimeType == DocumentsContract.Document.MIME_TYPE_DIR) scan(doc.uri, depth + 1)
                else pattern.matchEntire(doc.filename)?.let { match ->
                    val hash = match.groupValues[1].uppercase()
                    val entries = result.optJSONArray(hash) ?: JSONArray().also { result.put(hash, it) }
                    entries.put(doc.uri.toString())
                }
            }
        }
        scan(Uri.parse(pack), 0)
        return result
    }

    @Keep class Bridge(private val context: Context, private val report: (String) -> Unit) {
        fun progress(message: String) = report(message)
        fun read(uri: String): ByteArray {
            return context.contentResolver.openInputStream(Uri.parse(uri))?.use { input ->
                val output = java.io.ByteArrayOutputStream()
                val buffer = ByteArray(65536)
                while (true) {
                    val count = input.read(buffer)
                    if (count < 0) break
                    check(output.size().toLong() + count <= 64L * 1024 * 1024) { "Replacement texture exceeds 64 MiB" }
                    output.write(buffer, 0, count)
                }
                output.toByteArray()
            } ?: error("Cannot read the selected replacement texture")
        }
    }

    @Synchronized fun generate(c: Context, progress: (String) -> Unit): String {
        check(!generating && !DekuiAdultLinkSetup.generating && !DekuiPakSetup.generating) { "Another mod is being generated" }
        check(!NativeLibrary.isRunning()) { "Return to the startup screen before generating a mod" }
        check(canGenerate(c)) { "Select your TP GameCube ROM and set up MM3D first" }
        generating = true
        var stage: File? = null
        try {
            progress("Preparing the TP importer…")
            System.loadLibrary("nod")
            if (!Python.isStarted()) Python.start(AndroidPlatform(c.applicationContext))
            val module = Python.getInstance().getModule("tp_import.android")
            val bridge = Bridge(c, progress)
            val root = DekuiStorage.root(c)
            val nativeDir = c.applicationInfo.nativeLibraryDir
            val uri = Uri.parse(prefs(c).getString("rom", null))
            val generated = c.contentResolver.openFileDescriptor(uri, "r")?.use { fd ->
                module.callAttr("create", root.absolutePath, DekuiStorage.game(c).absolutePath,
                    "/proc/self/fd/${fd.fd}", "$nativeDir/libnod.so", bridge).toString()
            } ?: error("Cannot open the selected Twilight Princess ROM")
            stage = File(generated)
            progress("Searching all texture-pack subfolders…")
            val textures = JSONObject(module.callAttr("textures", generated, textureIndex(c).toString(),
                "$nativeDir/libcitra-android.so", bridge).toString())
            progress("Installing Twilight Princess Link…")
            module.callAttr("finish", root.absolutePath, generated)
            stage = null
            DekuiPresentationStore(c).update(mapOf("pak_link_model" to false,"twilight_link_model" to true,
                "adult_link_model" to false, "dark_link_model" to false))
            BooleanSetting.DISK_SHADER_CACHE.boolean = false
            val graphics = mutableListOf<org.citra.citra_emu.features.settings.model.AbstractSetting>(BooleanSetting.DISK_SHADER_CACHE)
            if (textures.getInt("matched") > 0) {
                BooleanSetting.CUSTOM_TEXTURES.boolean = true
                graphics.add(BooleanSetting.CUSTOM_TEXTURES)
            }
            check(SettingsFile.saveDekuiSettings(graphics)) { "Mod generated, but graphics settings could not be saved" }
            val result = "Ready: Twilight Princess Link, TP action voices, two sword variants and ${textures.getInt("matched")} matching HD textures. Restart the game to apply."
            check(prefs(c).edit().putString("result", result).commit())
            return result
        } finally {
            stage?.deleteRecursively()
            generating = false
        }
    }
}

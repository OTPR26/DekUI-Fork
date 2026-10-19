// SPDX-License-Identifier: GPL-2.0-or-later
package org.citra.citra_emu.utils

import android.content.Context
import android.util.AtomicFile
import androidx.preference.PreferenceManager
import org.citra.citra_emu.features.settings.model.view.InputBindingSetting
import org.json.JSONObject
import java.io.File
import java.security.MessageDigest

/** Install the alternate input patch and its controller profile together, once. */
object DekuiPcControls {
    private fun hash(bytes: ByteArray) = MessageDigest.getInstance("SHA-256").digest(bytes)
        .joinToString("") { "%02x".format(it.toInt() and 255) }
    private fun write(file: File, bytes: ByteArray) {
        check(file.parentFile!!.isDirectory || file.parentFile!!.mkdirs())
        val atomic=AtomicFile(file);val stream=atomic.startWrite()
        try { stream.write(bytes);atomic.finishWrite(stream) }
        catch(e: Exception) { atomic.failWrite(stream);throw e }
    }
    fun apply(context: Context) {
        val prefs=PreferenceManager.getDefaultSharedPreferences(context)
        val root=org.citra.citra_emu.utils.DekuiStorage.root(context)
        val patch=File(root,"load/mods/0004000000125500/code.bps")
        check(patch.isFile) { "Installed gameplay patch is missing" }
        val spec=context.assets.open("dekui-pc-controls/provenance.json").bufferedReader().use { JSONObject(it.readText()) }
        val current=patch.readBytes();val currentHash=hash(current)
        val known=currentHash==spec.getString("baseline_patch_sha256") ||
            currentHash==spec.getString("patch_sha256") || currentHash==spec.optString("legacy_patch_sha256")
        if(!known && prefs.getBoolean("dekui-pc-input-v1",false))return // Preserve an opted-in custom patch.
        check(currentHash==spec.getString("baseline_patch_sha256") || currentHash==spec.getString("patch_sha256") || currentHash==spec.optString("legacy_patch_sha256")) {
            "Gameplay patch differs from the verified PC migration; existing patch retained"
        }
        val pc=context.assets.open("dekui-pc-controls/code.bps").use { it.readBytes() }
        check(hash(pc)==spec.getString("patch_sha256")) { "PC gameplay patch checksum mismatch" }
        if(prefs.getBoolean("dekui-pc-input-v1",false) ||
            ((currentHash==spec.getString("patch_sha256") || currentHash==spec.optString("legacy_patch_sha256")) && DekuiStorage.hasSharedBindings(context))) {
            if(currentHash!=hash(pc))write(patch,pc)
            check(prefs.edit().putBoolean("dekui-pc-input-v1",true).commit())
            return
        }
        val backups=File(root,"config/dekui-backups")
        val bindings=File(backups,"controls-before-pc-input.json")
        if(!bindings.exists())write(bindings,(JSONObject(prefs.all).toString(2)+"\n").toByteArray())
        val original=File(backups,"code-before-pc-input.bps")
        if(!original.exists())write(original,current)
        if(currentHash!=hash(pc))write(patch,pc)
        InputBindingSetting.applyDekuiPcSnapshotBindings()
        check(prefs.edit().putBoolean("dekui-pc-input-v1",true).commit())
    }
}

// SPDX-License-Identifier: GPL-2.0-or-later
package org.citra.citra_emu.utils

import android.content.Context
import android.util.Base64
import android.util.AtomicFile
import org.json.JSONObject
import java.io.File
import java.security.MessageDigest

/** Update texture bytes only; retain player geometry/settings and back up the authored archive. */
object DekuiHudUpgrade {
    private fun hash(data: ByteArray) = MessageDigest.getInstance("SHA-256").digest(data)
        .joinToString("") { "%02x".format(it.toInt() and 255) }
    private fun write(file: File, data: ByteArray) {
        check(file.parentFile!!.isDirectory || file.parentFile!!.mkdirs())
        val atomic = AtomicFile(file)
        val stream = atomic.startWrite()
        try { stream.write(data); atomic.finishWrite(stream) }
        catch (e: Exception) { atomic.failWrite(stream); throw e }
    }
    private fun authored(context:Context, source:ByteArray, recipe:String):ByteArray {
        val spec=JSONObject(context.assets.open("dekui-hud8x/$recipe").bufferedReader().use{it.readText()})
        if(hash(source)==spec.getString("sha256"))return source
        var input=source
        val variants=spec.optJSONArray("source_variants")
        if(variants!=null)for(i in 0 until variants.length()) {
            val variant=variants.getJSONObject(i)
            if(hash(source)!=variant.getString("sha256"))continue
            val normalized=source.copyOf();val changes=variant.getJSONArray("normalize")
            for(j in 0 until changes.length()) {
                val op=changes.getJSONObject(j);val at=op.getInt("at")
                val before=Base64.decode(op.getString("before"),Base64.DEFAULT)
                val after=Base64.decode(op.getString("after"),Base64.DEFAULT)
                check(before.size==after.size&&at>=0&&at<=source.size-before.size)
                check(source.copyOfRange(at,at+before.size).contentEquals(before))
                after.copyInto(normalized,at)
            }
            input=normalized;break
        }
        if(hash(input)!=spec.getString("source_sha256"))return source
        val size=spec.getInt("size");check(size in 1..(512*1024))
        val output=java.io.ByteArrayOutputStream(size);val ops=spec.getJSONArray("ops")
        for(i in 0 until ops.length()) {
            val op=ops.getJSONObject(i)
            if(op.has("copy")) {
                val at=op.getInt("copy");val count=op.getInt("size")
                check(at>=0&&count>=0&&at<=input.size-count)
                output.write(input,at,count)
            } else output.write(Base64.decode(op.getString("data"),Base64.DEFAULT))
            check(output.size()<=size)
        }
        val result=output.toByteArray();check(result.size==size&&hash(result)==spec.getString("sha256"))
        return result
    }
    fun apply(context: Context) {
        val root = org.citra.citra_emu.utils.DekuiStorage.root(context)
        val archive = File(root,"load/mods/0004000000125500/romfs/layout/Joker.Main.New.gar")
        if (!archive.isFile) return // A fresh ROM import installs its layouts first.
        val spec = context.assets.open("dekui-hud8x/upgrade.json").bufferedReader().use { JSONObject(it.readText()) }
        val archiveOriginal = archive.readBytes()
        val original = authored(context,archiveOriginal,"layout.json")
        val play=File(root,"load/mods/0004000000125500/romfs/layout/Joker.Main.Play.gar")
        val playOriginal=play.readBytes()
        val playUpdated=authored(context,playOriginal,"play.json")
        check(original.size == spec.getInt("archive_size")) { "HUD archive size differs; existing HUD retained" }
        val updated = original.copyOf()
        val ops = spec.getJSONArray("ops")
        for (i in 0 until ops.length()) {
            val op = ops.getJSONObject(i); val at = op.getInt("at")
            val before = Base64.decode(op.getString("before"), Base64.DEFAULT)
            val after = Base64.decode(op.getString("after"), Base64.DEFAULT)
            check(before.size == after.size && at >= 0 && at <= original.size-before.size)
            val current = original.copyOfRange(at, at+before.size)
            val alternate = op.optString("alternate").takeIf { it.isNotEmpty() }?.let { Base64.decode(it, Base64.DEFAULT) }
            check(current.contentEquals(before) || current.contentEquals(after) ||
                (alternate != null && current.contentEquals(alternate))) {
                "HUD texture differs from the PC upgrade; existing HUD retained"
            }
            after.copyInto(updated, at)
        }
        val name = spec.getString("texture")
        check(name.matches(Regex("tex1_[A-Za-z0-9_]+\\.png")))
        val textureBytes = context.assets.open("dekui-hud8x/$name").use { it.readBytes() }
        check(hash(textureBytes) == spec.getString("texture_sha256")) { "HUD texture checksum mismatch" }
        val texture = File(root,"load/textures/0004000000125500/CUSTOM_HUD/$name")
        if (!texture.isFile || hash(texture.readBytes()) != hash(textureBytes)) write(texture,textureBytes)
        if(!playOriginal.contentEquals(playUpdated)) {
            val backup=File(root,"config/dekui-backups/play-before-android-hud-${hash(playOriginal).take(12)}.gar")
            if(!backup.isFile)write(backup,playOriginal)
            write(play,playUpdated)
        }
        if (!archiveOriginal.contentEquals(updated)) {
            val backup = File(root,"config/dekui-backups/hud-before-8x-${hash(archiveOriginal).take(12)}.gar")
            if (!backup.isFile) write(backup,archiveOriginal)
            write(archive,updated)
        }
    }
}

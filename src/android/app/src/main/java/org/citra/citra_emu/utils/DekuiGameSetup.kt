// SPDX-License-Identifier: GPL-2.0-or-later
package org.citra.citra_emu.utils

import android.content.Context
import java.io.File
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.MessageDigest
import org.json.JSONObject

/** Reconstructs only fingerprint-approved layout files from the selected game. */
object DekuiGameSetup {
    private fun hash(data: ByteArray) = MessageDigest.getInstance("SHA-256")
        .digest(data).joinToString("") { "%02x".format(it.toInt() and 255) }
    private fun json(context: Context, name: String) = JSONObject(
        context.assets.open("dekui-setup/$name").bufferedReader().use { it.readText() })
    private fun File.child(path: String): File {
        val result = File(this, path).canonicalFile
        require(result.path.startsWith(canonicalPath + File.separator)) { "Invalid asset path" }
        return result
    }
    fun prepare(context: Context, rom: File, root: File) {
        val expected = json(context, "supported-rom.json")
        val recipe = json(context, "android-layout-recipe.json")
        val inputs = recipe.getJSONObject("inputs")
        val sources = mutableMapOf<String, ByteArray>()
        RandomAccessFile(rom, "r").use { stream ->
            fun read(at: Long, count: Int): ByteArray {
                require(at >= 0 && count >= 0 && at <= stream.length() - count) { "Truncated game file" }
                return ByteArray(count).also { stream.seek(at); stream.readFully(it) }
            }
            fun u32(at: Long) = ByteBuffer.wrap(read(at, 4)).order(ByteOrder.LITTLE_ENDIAN).int.toLong() and 0xffffffffL
            require(String(read(0x100,4), Charsets.US_ASCII) == "NCSD") { "Select a decrypted .cci or .3ds game" }
            val base = u32(0x120) * 512
            val length = u32(0x124) * 512
            require(base >= 512 && length == expected.getLong("main_partition_bytes")) { "Unsupported game revision" }
            require(String(read(base + 0x100,4), Charsets.US_ASCII) == "NCCH") { "Invalid game partition" }
            require(read(base + 0x18f,1)[0].toInt() and 4 != 0) { "The selected game must be decrypted" }
            val digest = MessageDigest.getInstance("SHA-256")
            var offset = 0L
            while (offset < length) {
                val bytes = read(base + offset, minOf(1024 * 1024L, length - offset).toInt())
                digest.update(bytes); offset += bytes.size
            }
            require(digest.digest().joinToString("") { "%02x".format(it.toInt() and 255) } ==
                expected.getString("main_partition_sha256")) { "Unsupported game: select the decrypted USA Rev 1 version" }
            val start = base + u32(base + 0x1b0) * 512 + 0x1000
            val romfsLength = u32(base + 0x1b4) * 512 - 0x1000
            fun rr(at: Long, count: Int): ByteArray {
                require(at >= 0 && count >= 0 && at <= romfsLength - count) { "Invalid game filesystem" }
                return read(start + at, count)
            }
            fun words(at: Long, count: Int) = ByteBuffer.wrap(rr(at,count)).order(ByteOrder.LITTLE_ENDIAN)
            val header = words(0,40)
            require(header.getInt(0) == 40) { "Unsupported game filesystem" }
            val dt = header.getInt(12).toLong(); val ft = header.getInt(28).toLong()
            val payload = header.getInt(36).toLong()
            val wanted = inputs.keys().asSequence().filter { it.startsWith("original/") }.toSet()
            val visited = mutableSetOf<Long>()
            fun directory(at: Long, parent: String, depth: Int): Long {
                require(depth <= 32 && visited.add(at)) { "Invalid directory tree" }
                val entry = words(dt + at,24)
                val sibling = entry.getInt(4).toLong() and 0xffffffffL
                var child = entry.getInt(8).toLong() and 0xffffffffL
                var file = entry.getInt(12).toLong() and 0xffffffffL
                val n = entry.getInt(20); require(n in 0..1024)
                val name = String(rr(dt + at + 24,n),Charsets.UTF_16LE)
                val path = parent + if (name.isEmpty()) "" else "$name/"
                val seen = mutableSetOf<Long>()
                while (file != 0xffffffffL) {
                    require(seen.add(file))
                    val f = words(ft + file,32)
                    val size = f.getLong(16); val nameSize = f.getInt(28)
                    require(nameSize in 0..1024)
                    val key = "original/romfs/" + path + String(rr(ft + file + 32,nameSize),Charsets.UTF_16LE)
                    if (key in wanted) {
                        require(size in 0..16*1024*1024L && key !in sources)
                        sources[key] = rr(payload + f.getLong(8),size.toInt())
                    }
                    file = f.getInt(4).toLong() and 0xffffffffL
                }
                while (child != 0xffffffffL) child = directory(child,path,depth+1)
                return sibling
            }
            directory(0,"",0)
            require(sources.keys == wanted) { "Required game layouts missing" }
        }
        for (key in inputs.keys()) {
            if (key.startsWith("restoration/")) sources[key] = context.assets
                .open("dekui-setup/project-restoration/" + key.removePrefix("restoration/"))
                .use { it.readBytes() }
            require(hash(sources.getValue(key)) == inputs.getString(key)) { "Layout input mismatch: $key" }
        }
        val stage = File(root, ".dekui-setup-${java.util.UUID.randomUUID()}")
        check(stage.mkdirs())
        try {
            val mods = File(stage,"mods"); check(mods.mkdirs())
            val files = recipe.getJSONArray("files")
            for (i in 0 until files.length()) {
                val item = files.getJSONObject(i)
                val size = item.getInt("size"); require(size in 0..16*1024*1024)
                val output = sources.getValue(item.getString("base")).copyOf(size)
                val ops = item.getJSONArray("ops")
                for (j in 0 until ops.length()) {
                    val op = ops.getJSONObject(j); val at = op.getInt("at")
                    if (op.has("byte")) output[at] = op.getInt("byte").toByte()
                    else {
                        val from = op.getInt("offset"); val count = op.getInt("length")
                        sources.getValue(op.getString("copy")).copyInto(output,at,from,from+count)
                    }
                }
                require(hash(output) == item.getString("sha256")) { "Layout reconstruction mismatch" }
                val target = mods.child(item.getString("output")); target.parentFile!!.mkdirs(); target.writeBytes(output)
            }
            // Ship the expanded owl prompt beside the layout mods. The native
            // hook restores the original payload whenever Classic saving is off.
            val owl = File(stage,"mods/romfs/message/us/use.gmsg")
            context.assets.open("dekui-setup/project-restoration/romfs/message/us/use.gmsg").use {
                owl.parentFile!!.mkdirs(); owl.outputStream().use { out -> it.copyTo(out) }
            }
            val target = File(root,"load/mods/0004000000125500")
            // Retain an existing installation, including the user's edited layouts.
            if (!target.exists()) {
                target.parentFile!!.mkdirs(); check(mods.renameTo(target))
            } else {
                val existingOwl = target.child("romfs/message/us/use.gmsg")
                if (!existingOwl.isFile) { existingOwl.parentFile!!.mkdirs(); check(owl.copyTo(existingOwl).isFile) }

                for (i in 0 until files.length()) {
                    check(target.child(files.getJSONObject(i).getString("output")).isFile) {
                        "Existing game layouts are incomplete; existing files were retained"
                    }
                }
            }
            for ((key, bytes) in sources) if (key.startsWith("original/")) {
                val file = File(root,"config/mm3d-original").child(key.removePrefix("original/"))
                if (!file.exists()) { file.parentFile!!.mkdirs(); file.writeBytes(bytes) }
            }
        } finally { stage.deleteRecursively() }
    }
}

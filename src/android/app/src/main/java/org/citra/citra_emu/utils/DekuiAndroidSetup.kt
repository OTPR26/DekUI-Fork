// SPDX-License-Identifier: GPL-2.0-or-later
package org.citra.citra_emu.utils

import android.content.Context
import android.net.Uri
import androidx.documentfile.provider.DocumentFile
import java.io.IOException

/** Android-safe storage helpers for Dekui's optional presentation layer. */
object DekuiAndroidSetup {
    fun initializeStorage(context: Context) {
        if (!org.citra.citra_emu.BuildConfig.DEKUI_PRESENTATION) return
        if (PermissionsHandler.citraDirectory.toString().isEmpty()) {
            val root = java.io.File(context.filesDir, "dekui")
            check(root.isDirectory || root.mkdirs())
            PermissionsHandler.setCitraDirectory(Uri.fromFile(root).toString())
        }
    }

    fun isPrivateStorage(context: Context): Boolean =
        org.citra.citra_emu.BuildConfig.DEKUI_PRESENTATION &&
            PermissionsHandler.citraDirectory == Uri.fromFile(java.io.File(context.filesDir, "dekui"))

    /** Install the whole authored baseline where the native overlay loads it.
     * Existing settings/profiles are retained. Partial writes are deleted so
     * a later startup can retry instead of accepting an incomplete file.
     */
    fun installDefaults(context: Context, userTree: Uri): Boolean {
        if (!org.citra.citra_emu.BuildConfig.DEKUI_PRESENTATION) return true
        return try {
            val root = if (isPrivateStorage(context)) {
                DocumentFile.fromFile(java.io.File(context.filesDir, "dekui"))
            } else DocumentFile.fromTreeUri(context, userTree) ?: return false
            fun directory(parent: DocumentFile, name: String): DocumentFile =
                (parent.findFile(name) ?: parent.createDirectory(name))
                    ?.takeIf { it.isDirectory } ?: throw IOException("Cannot create $name")
            val config = directory(root, "config")
            val targetRoot = directory(config, "mm3d-local")
            if (isPrivateStorage(context)) {
                java.io.File(context.filesDir,"dekui/config/mm3d-local/menu-font.otf").delete()
                val font=java.io.File(context.filesDir,"dekui/config/mm3d-local/menu-font.ttf")
                if (!font.isFile || font.length()==0L) {
                    val atomic=android.util.AtomicFile(font)
                    val output=atomic.startWrite()
                    try {
                        context.resources.openRawResource(org.citra.citra_emu.R.font.dekui_sans_regular).use { it.copyTo(output) }
                        atomic.finishWrite(output)
                    } catch(error: Exception) { atomic.failWrite(output);throw error }
                }
            }
            val legacy = root.findFile("dekui")?.findFile("presentation.json")
            fun copy(asset: String, parent: DocumentFile, name: String) {
                val children = context.assets.list(asset) ?: throw IOException("Missing $asset")
                if (children.isNotEmpty()) {
                    val target = directory(parent, name)
                    children.forEach { copy("$asset/$it", target, it) }
                } else {
                    val existing = parent.findFile(name)
                    if (existing != null) {
                        if (!existing.isFile) throw IOException("Not a file: $name")
                        return
                    }
                    // Preserve settings changed during the earlier Android experiment.
                    val input = if (asset == "dekui-defaults/presentation.json" && legacy?.isFile == true) {
                        if (legacy.uri.scheme == "file") java.io.File(legacy.uri.path!!).inputStream() else context.contentResolver.openInputStream(legacy.uri)
                            ?: throw IOException("Cannot read earlier settings")
                    } else context.assets.open(asset)
                    input.use { source ->
                        val target = parent.createFile("application/json", name.removeSuffix(".json"))
                            ?: throw IOException("Cannot create $name")
                        try {
                            val bytes = source.readBytes()
                            org.json.JSONObject(String(bytes, Charsets.UTF_8))
                            val output = if (target.uri.scheme == "file") java.io.File(target.uri.path!!).outputStream() else context.contentResolver.openOutputStream(target.uri, "wt")
                                ?: throw IOException("Cannot write $name")
                            output.use { it.write(bytes) }
                        } catch (error: Exception) {
                            target.delete()
                            throw error
                        }
                    }
                }
            }
            val files = context.assets.list("dekui-defaults") ?: return false
            if (files.isEmpty()) throw IOException("Missing Dekui defaults")
            files.forEach { copy("dekui-defaults/$it", targetRoot, it) }
            true
        } catch (error: Exception) {
            android.util.Log.e("DekuiSetup", "Could not install presentation defaults", error)
            false
        }
    }

    /** Basic container check before handing a selected game to native loading. */
    fun looksLikeMm3dContainer(context: Context, uri: Uri): Boolean {
        val name = DocumentFile.fromSingleUri(context, uri)?.name?.lowercase() ?: return false
        if (!name.endsWith(".cci") && !name.endsWith(".3ds")) return false
        return try {
            // NCSD is the standard 3DS container signature at offset 0x100.
            context.contentResolver.openInputStream(uri)?.use { input ->
                if (input.skip(0x100) != 0x100L) return false
                val magic = ByteArray(4)
                input.read(magic) == 4 && magic.contentEquals(byteArrayOf('N'.code.toByte(), 'C'.code.toByte(), 'S'.code.toByte(), 'D'.code.toByte()))
            } ?: false
        } catch (_: IOException) {
            false
        }
    }
}

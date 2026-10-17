// SPDX-License-Identifier: GPL-2.0-or-later
package org.citra.citra_emu.utils

import android.content.Context
import android.content.Intent
import android.net.Uri
import androidx.documentfile.provider.DocumentFile
import org.citra.citra_emu.CitraApplication

/** Read-only SAF mount, independent of the writable game/user directory. */
object DekuiTextureStorage {
    private const val PREFIX = "/@textures/"
    private const val TITLE = "0004000000125500"
    private val context get() = CitraApplication.appContext
    private val prefs get() = context.getSharedPreferences("dekui-textures", Context.MODE_PRIVATE)
    private var cached: DocumentsTree? = null
    private var initialized = false

    fun handles(path: String) = path.startsWith(PREFIX)
    private fun relative(path: String) = "/" + path.removePrefix(PREFIX)

    @Synchronized
    private fun tree(): DocumentsTree? {
        if (!initialized) {
            initialized = true
            val root = prefs.getString("folder", null)
            if (root != null) cached = DocumentsTree(externalReadOnly = true).apply { setRoot(Uri.parse(root)) }
        }
        return cached
    }

    fun description(): String {
        val folder = prefs.getString("folder", null) ?: return "App texture folder (no external pack selected)"
        return android.provider.DocumentsContract.getDocumentId(Uri.parse(folder)).substringAfter(':')
    }

    /** Accept the title directory, textures directory, or an Azahar user directory. */
    fun select(uri: Uri): String {
        val root = DocumentFile.fromTreeUri(context, uri) ?: error("Cannot open this folder")
        val candidates = listOf(
            root.takeIf { it.name.equals(TITLE, ignoreCase = true) },
            root.findFile(TITLE),
            root.findFile("textures")?.findFile(TITLE),
            root.findFile("load")?.findFile("textures")?.findFile(TITLE)
        )
        val folder = candidates.firstOrNull { it?.isDirectory == true }
            ?: root.takeIf { dir -> FileUtil.listFiles(dir.uri).any {
                it.filename.startsWith("tex1_") || it.filename == "pack.json"
            } }
            ?: error("No MM3D texture pack found. Choose ROMs/n3DS, load/textures, or the $TITLE folder.")
        check(folder.canRead()) { "Android cannot read the selected texture folder" }
        context.contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
        val label = android.provider.DocumentsContract.getDocumentId(folder.uri).substringAfter(':')
        check(prefs.edit().putString("folder", folder.uri.toString()).putString("label", label).commit())
        synchronized(this) {
            cached = DocumentsTree(externalReadOnly = true).apply { setRoot(folder.uri) }
            initialized = true
        }
        return label
    }

    @Synchronized
    fun clear() {
        prefs.edit().clear().commit()
        cached = null
        initialized = true
    }

    fun readable(): Boolean {
        return try {
        val folder = prefs.getString("folder", null) ?: return true
        FileUtil.isDirectory(folder) && FileUtil.exists(folder)
    } catch (_: Exception) { false }
    }
    fun open(path: String, mode: String): Int = if (mode != "r") -1 else tree()?.openContentUri(relative(path), mode) ?: -1
    fun names(path: String): Array<String?> = tree()?.getFilesName(relative(path)) ?: emptyArray()
    fun size(path: String): Long = tree()?.getFileSize(relative(path)) ?: 0
    fun exists(path: String): Boolean = tree()?.exists(relative(path)) ?: false
    fun isDirectory(path: String): Boolean = tree()?.isDirectory(relative(path)) ?: false
}

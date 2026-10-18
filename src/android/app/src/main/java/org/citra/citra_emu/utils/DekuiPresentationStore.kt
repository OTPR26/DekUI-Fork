// SPDX-License-Identifier: GPL-2.0-or-later
package org.citra.citra_emu.utils

import android.content.Context
import android.util.AtomicFile
import java.io.File
import org.json.JSONObject

/** Edit only requested keys; preserve all installed HUD customization and unknown fields. */
class DekuiPresentationStore(context: Context) {
    private val file=java.io.File(org.citra.citra_emu.utils.DekuiStorage.root(context),"config/mm3d-local/presentation.json")
    fun read(): JSONObject {
        val value=JSONObject(file.readText())
        check(value.optInt("version",1)==1) { "Unsupported settings version" }
        return value
    }
    @Synchronized fun update(values: Map<String,Any>) {
        val json=read()
        values.forEach { (key,value)->json.put(key,value) }
        val atomic=AtomicFile(file);val output=atomic.startWrite()
        try { output.write(json.toString(2).toByteArray(Charsets.UTF_8));atomic.finishWrite(output) }
        catch(error: Exception) { atomic.failWrite(output);throw error }
    }
}

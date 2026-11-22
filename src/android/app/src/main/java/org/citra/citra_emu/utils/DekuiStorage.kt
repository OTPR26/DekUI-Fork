// SPDX-License-Identifier: GPL-2.0-or-later
package org.citra.citra_emu.utils

import android.content.Context
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.util.AtomicFile
import androidx.preference.PreferenceManager
import java.io.File
import org.json.JSONObject
import org.json.JSONArray
import org.citra.citra_emu.NativeLibrary

/** Shared, user-visible game data. App runtime libraries remain in app-private storage. */
object DekuiStorage {
    private fun prefs(context: Context) = context.getSharedPreferences("dekui-storage", Context.MODE_PRIVATE)
    fun isEuropean(context: Context) = prefs(context).getBoolean("european", false)
    fun sharedRoot(context: Context, european: Boolean = isEuropean(context)) =
        File(Environment.getExternalStorageDirectory(), if (european) "mm3dport-eur-preview" else "mm3dport")
    fun hasAccess(context:Context)=if(Build.VERSION.SDK_INT>=30)Environment.isExternalStorageManager()
        else androidx.core.content.ContextCompat.checkSelfPermission(context,android.Manifest.permission.WRITE_EXTERNAL_STORAGE)==android.content.pm.PackageManager.PERMISSION_GRANTED
    fun root(context: Context): File =
        if (prefs(context).getBoolean("shared-v1", false)) sharedRoot(context) else File(context.filesDir, "dekui")
    fun game(context: Context) = if (prefs(context).getBoolean("shared-v1", false))
        File(root(context), "game/game.cci") else File(context.filesDir, "dekui-game/game.cci")

    fun prepareRegion(context: Context, european: Boolean): File {
        val target = sharedRoot(context, european)
        check(hasAccess(context)) { "Allow file access to use Internal storage/${target.name}" }
        val suffix = if (european) "-eur" else ""
        DekuiStorageMigration.migrate(File(context.filesDir, "dekui$suffix"),
            File(context.filesDir, "dekui-game$suffix/game.cci"), target)
        return target
    }

    fun migrate(context: Context) {
        val european = isEuropean(context)
        val target = sharedRoot(context, european)
        if (prefs(context).getBoolean("shared-v1", false))
            check(File(target, ".dekui-storage-v1").isFile) { "Restore the ${target.name} data folder before starting." }
        else prepareRegion(context, european)
        activate(context, european)
    }

    fun activate(context: Context, european: Boolean) {
        val target = sharedRoot(context, european)
        check(File(target, ".dekui-storage-v1").isFile) { "The selected game region is not prepared." }
        check(PreferenceManager.getDefaultSharedPreferences(context).edit().putString(
            PermissionsHandler.CITRA_DIRECTORY, Uri.fromFile(target).toString()).commit())
        NativeLibrary.setUserDirectory(target.absolutePath)
        NativeLibrary.setDekuiEuropeanPreview(european)
        DirectoryInitialization.resetCitraDirectoryState()
        check(DirectoryInitialization.start()==DirectoryInitialization.DirectoryInitializationState.CITRA_DIRECTORIES_INITIALIZED){"Shared game data could not be initialized"}
        check(prefs(context).edit().putBoolean("shared-v1",true).putBoolean("european",european).commit())
        NativeLibrary.playTimeManagerInit()
        controls(context)
    }
    fun hasSharedBindings(context:Context):Boolean {
        val file=File(root(context),"config/android-controls.json")
        if(!file.isFile)return false
        val json=JSONObject(file.readText())
        return json.optInt("version")==1 && json.optJSONObject("bindings")?.length()?.let { it>0 }==true
    }
    private var listener:android.content.SharedPreferences.OnSharedPreferenceChangeListener?=null
    private fun bindingKey(key:String):Boolean {
        val keys=org.citra.citra_emu.features.settings.model.Settings.run { buttonKeys+triggerKeys+dPadButtonKeys+circlePadKeys+cStickKeys+dPadAxisKeys }
        return key.startsWith("InputMapping") || key in keys || key == DekuiAndroidMenu.PICTOGRAPH_KEY
    }
    private fun controls(context:Context){
        val prefs=PreferenceManager.getDefaultSharedPreferences(context)
        val file=File(root(context),"config/android-controls.json")
        listener?.let{prefs.unregisterOnSharedPreferenceChangeListener(it)}
        listener=null
        if(file.isFile){
            val json=JSONObject(file.readText());check(json.optInt("version")==1){"Unsupported controller settings version"}
            val values=json.getJSONObject("bindings");val editor=prefs.edit()
            prefs.all.keys.filter{bindingKey(it)}.forEach{editor.remove(it)}
            values.keys().forEach{key->if(bindingKey(key))when(val value=values.get(key)){
                is JSONArray->editor.putStringSet(key,(0 until value.length()).map{value.getString(it)}.toSet())
                is Int->editor.putInt(key,value)
                is Long->editor.putLong(key,value)
                is Double->editor.putFloat(key,value.toFloat())
                is Boolean->editor.putBoolean(key,value)
                is String->editor.putString(key,value)
            }}
            check(editor.commit())
        }
        org.citra.citra_emu.overlay.ReMaskedShoulderBindings.migrateLegacy(prefs)
        fun save(){
            val values=JSONObject();prefs.all.filterKeys{bindingKey(it)}.forEach{(k,v)->values.put(k,if(v is Set<*>)JSONArray(v.toList())else v)}
            val json=JSONObject().put("version",1).put("bindings",values)
            file.parentFile!!.mkdirs();val atomic=AtomicFile(file);val stream=atomic.startWrite()
            try{stream.write((json.toString(2)+"\n").toByteArray());atomic.finishWrite(stream)}catch(e:Exception){atomic.failWrite(stream);throw e}
        }
        save()
        listener?.let{prefs.unregisterOnSharedPreferenceChangeListener(it)}
        listener=android.content.SharedPreferences.OnSharedPreferenceChangeListener{_,key->if(key!=null&&bindingKey(key))save()}
        prefs.registerOnSharedPreferenceChangeListener(listener)
    }
}

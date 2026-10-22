// SPDX-License-Identifier: GPL-2.0-or-later
package org.citra.citra_emu.utils

import android.app.Activity
import android.app.Application
import android.content.ComponentName
import android.content.Context
import android.content.pm.PackageManager
import android.os.Bundle
import android.os.Handler
import android.os.Looper

/** Keep the current task's launcher alias alive until the app leaves the foreground. */
object LauncherIconSelection {
    private fun components(context: Context) = (0..4).map {
        ComponentName(context, "org.citra.citra_emu.LauncherIcon$it")
    }
    private fun prefs(context: Context) = context.getSharedPreferences("launcher-icon", Context.MODE_PRIVATE)

    fun selected(context: Context): Int {
        val stored = prefs(context).getInt("selection", -1)
        if (stored in 0..4) return stored
        return components(context).indexOfFirst {
            context.packageManager.getComponentEnabledSetting(it) == PackageManager.COMPONENT_ENABLED_STATE_ENABLED
        }.coerceAtLeast(0)
    }

    fun select(context: Context, index: Int) {
        require(index in 0..4)
        val component = components(context)[index]
        if (context.packageManager.getComponentEnabledSetting(component) != PackageManager.COMPONENT_ENABLED_STATE_ENABLED) {
            context.packageManager.setComponentEnabledSetting(component,
                PackageManager.COMPONENT_ENABLED_STATE_ENABLED, PackageManager.DONT_KILL_APP)
        }
        prefs(context).edit().putInt("selection", index).apply()
    }

    private fun refresh(context: Context) {
        val selected = selected(context)
        val manager = context.packageManager
        val changes = components(context).mapIndexedNotNull { index, component ->
            val desired = if (index == selected) PackageManager.COMPONENT_ENABLED_STATE_ENABLED
                else PackageManager.COMPONENT_ENABLED_STATE_DISABLED
            if (manager.getComponentEnabledSetting(component) == desired) null else component to desired
        }
        if (changes.isEmpty()) return
        if (android.os.Build.VERSION.SDK_INT >= 33) {
            manager.setComponentEnabledSettings(changes.map { (component, state) ->
                PackageManager.ComponentEnabledSetting(component, state, PackageManager.DONT_KILL_APP)
            })
        } else {
            changes.forEach { (component, state) ->
                manager.setComponentEnabledSetting(component, state, PackageManager.DONT_KILL_APP)
            }
        }
    }

    fun initialize(application: Application) {
        val visible = mutableSetOf<Activity>()
        val handler = Handler(Looper.getMainLooper())
        val refresh = Runnable {
            if (visible.isEmpty()) runCatching { refresh(application) }
        }
        application.registerActivityLifecycleCallbacks(object : Application.ActivityLifecycleCallbacks {
            override fun onActivityStarted(activity: Activity) {
                visible.add(activity)
                handler.removeCallbacks(refresh)
            }
            override fun onActivityStopped(activity: Activity) {
                visible.remove(activity)
                if (visible.isEmpty()) handler.postDelayed(refresh, 300)
            }
            override fun onActivityCreated(activity: Activity, state: Bundle?) {}
            override fun onActivityResumed(activity: Activity) {}
            override fun onActivityPaused(activity: Activity) {}
            override fun onActivitySaveInstanceState(activity: Activity, state: Bundle) {}
            override fun onActivityDestroyed(activity: Activity) { visible.remove(activity) }
        })
    }
}

@file:OptIn(ExperimentalProjectedApi::class)

package com.jpcottin.vulkanspaceinvaders

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.os.Build
import android.util.Log
import androidx.xr.projected.ProjectedContext
import androidx.xr.projected.experimental.ExperimentalProjectedApi
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch

/**
 * Thin JVM-side bridge between the native game and the AI-Glasses projected
 * display. The native code calls these static methods over JNI:
 *  - [startMonitoring] once at startup (phone instance) to begin observing
 *    glasses connect/disconnect,
 *  - [isConnected] to poll the latest state (cheap volatile read),
 *  - [launchOnGlasses] to start [GlassesGameActivity] on the projected display.
 *
 * Everything is guarded to API 36+ (androidx.xr.projected requirement); on
 * older devices the glasses simply never report as connected.
 */
object GlassesBridge {
    private const val TAG = "SpaceInvaders"

    @Volatile private var connected = false
    private var scope: CoroutineScope? = null

    @JvmStatic
    fun startMonitoring(context: Context) {
        if (Build.VERSION.SDK_INT < 36 || scope != null) return
        val s = CoroutineScope(SupervisorJob() + Dispatchers.Main)
        scope = s
        s.launch {
            try {
                ProjectedContext.isProjectedDeviceConnected(
                    context.applicationContext,
                    s.coroutineContext
                ).collect { value ->
                    connected = value
                    Log.i(TAG, "Glasses connected: $value")
                }
            } catch (t: Throwable) {
                Log.w(TAG, "Glasses monitoring unavailable", t)
                connected = false
            }
        }
    }

    @JvmStatic
    fun isConnected(): Boolean = connected

    @JvmStatic
    fun launchOnGlasses(activity: Activity): Boolean {
        if (Build.VERSION.SDK_INT < 36 || !connected) return false
        return try {
            val intent = Intent(activity, GlassesGameActivity::class.java)
            val options = ProjectedContext.createProjectedActivityOptions(activity)
            activity.startActivity(intent, options.toBundle())
            true
        } catch (t: Throwable) {
            Log.w(TAG, "Failed to launch on glasses", t)
            false
        }
    }
}

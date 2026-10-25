package com.jpcottin.vulkanspaceinvaders

import android.app.NativeActivity
import android.os.Build
import android.os.Bundle
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController

/**
 * The game on the phone: the native library as a NativeActivity, run
 * immersive. With targetSdk 35+ the window spans the whole display, so
 * without this the navigation bar sat over the bottom of the control strip:
 * taps there went to Back, Home and Recents instead of the ship, and a drag
 * starting in the gesture band went to the home gesture. The bars come back
 * on a swipe from the edge and hide again on their own.
 */
class GameActivity : NativeActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.insetsController?.let {
                it.hide(WindowInsets.Type.systemBars())
                it.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            }
        } else {
            @Suppress("DEPRECATION")
            window.decorView.systemUiVisibility = View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or
                View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_FULLSCREEN or
                View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
        }
    }
}

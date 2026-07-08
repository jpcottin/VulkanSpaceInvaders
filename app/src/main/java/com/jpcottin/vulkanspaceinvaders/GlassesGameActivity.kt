package com.jpcottin.vulkanspaceinvaders

import android.app.NativeActivity
import android.os.Bundle
import android.view.WindowManager

/**
 * The game on the AI Glasses: the exact same native library, running as a
 * second NativeActivity on the projected display (see the manifest's
 * requiredDisplayCategory). The native side detects this activity class over
 * JNI and switches to touchbar controls + a pure-black clear colour (black
 * renders transparent on additive AR lenses).
 */
class GlassesGameActivity : NativeActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // Projected displays may blank on idle; the game is the foreground
        // experience while this activity lives.
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
    }
}

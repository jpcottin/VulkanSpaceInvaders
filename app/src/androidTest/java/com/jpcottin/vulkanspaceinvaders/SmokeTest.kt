package com.jpcottin.vulkanspaceinvaders

import android.app.NativeActivity
import androidx.lifecycle.Lifecycle
import androidx.test.core.app.ActivityScenario
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import android.graphics.Bitmap
import android.os.ParcelFileDescriptor
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

/**
 * Instrumented smoke test: launches the NativeActivity and verifies that
 * Vulkan initialises without crashing and the activity reaches RESUMED state.
 * A screenshot is captured while the game is in the foreground and saved to
 * external app storage so CI can pull it with `adb pull`.
 */
@RunWith(AndroidJUnit4::class)
class SmokeTest {

    private fun shell(cmd: String): String {
        val pfd = InstrumentationRegistry.getInstrumentation()
            .uiAutomation.executeShellCommand(cmd)
        return ParcelFileDescriptor.AutoCloseInputStream(pfd)
            .bufferedReader().readText()
    }

    @Test
    fun vulkanInitialisesWithoutCrash() {
        // Clear logcat BEFORE launching so the swapchain assertion can only be
        // satisfied by THIS launch — the buffer is boot-persistent, and a
        // "Swapchain ready" from an earlier run would mask a dead renderer.
        // (That's also why the activity is launched manually rather than via
        // ActivityScenarioRule, which starts it before the test body runs.)
        shell("logcat -c")

        ActivityScenario.launch(NativeActivity::class.java).use { scenario ->
            // Poll for the first frame instead of a fixed sleep: the
            // software-rasterized CI legs (swiftshader/lavapipe) can take
            // well over any single guess.
            // Note: a single filterspec — a second one for the same tag would
            // override the first (":I" already includes warnings and errors).
            val deadline = System.currentTimeMillis() + 60_000
            var log = ""
            while (System.currentTimeMillis() < deadline) {
                log = shell("logcat -d -s SpaceInvaders:I")
                if (log.contains("Swapchain ready")) break
                Thread.sleep(500)
            }

            val state = scenario.state
            assertEquals(
                "NativeActivity should be RESUMED after Vulkan init, but was $state",
                Lifecycle.State.RESUMED,
                state
            )

            // RESUMED alone doesn't prove pixels: the game survives a
            // Vulkan-less device without crashing (it just renders nothing).
            // Assert the renderer actually brought a swapchain up, so a dead
            // render path fails the test instead of silent black screenshots.
            assertTrue(
                "Renderer never reached 'Swapchain ready' — Vulkan log was:\n" +
                    log.lines().takeLast(20).joinToString("\n"),
                log.contains("Swapchain ready")
            )

            // Screenshot the verified foreground moment for CI: screencap runs
            // as the shell user and /data/local/tmp survives the post-test
            // uninstall, so the uploaded artifact is THIS asserted frame — not
            // an unverified relaunch after the test.
            shell("screencap -p /data/local/tmp/smoke.png")

            // Capture a screenshot while the game is guaranteed to be in the
            // foreground. App-specific storage avoids EPERM on API 30+.
            val context = InstrumentationRegistry.getInstrumentation().targetContext
            val screenshotFile = File(context.getExternalFilesDir(null), "smoke.png")

            val bitmap = InstrumentationRegistry.getInstrumentation().uiAutomation.takeScreenshot()
            if (bitmap != null) {
                screenshotFile.outputStream().use { stream ->
                    bitmap.compress(Bitmap.CompressFormat.PNG, 100, stream)
                }
                println("Screenshot saved to: ${screenshotFile.absolutePath}")
            } else {
                println("takeScreenshot() returned null — skipping screenshot save")
            }
        }
    }
}

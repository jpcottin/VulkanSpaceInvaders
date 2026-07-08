// Top-level build file. This is a pure-native (NDK + Vulkan) app, so only the
// Android application plugin is needed — no Kotlin/Compose.
plugins {
  alias(libs.plugins.android.application) apply false
}

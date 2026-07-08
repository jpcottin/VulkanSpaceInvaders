// Top-level build file. The game is pure-native (NDK + Vulkan); the only JVM
// code is the thin AI-Glasses bridge, compiled by AGP 9's built-in Kotlin.
plugins {
  alias(libs.plugins.android.application) apply false
}

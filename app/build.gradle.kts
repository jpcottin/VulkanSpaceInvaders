// AGP 9 compiles the (single) Kotlin bridge file with its built-in Kotlin
// support — no explicit Kotlin plugin.
plugins {
  alias(libs.plugins.android.application)
}

// Push and run the native Google Test binary on the connected device/emulator.
// Usage: ./gradlew runNativeTests
tasks.register("runNativeTests") {
    dependsOn("externalNativeBuildDebug")
    // ABI defaults to arm64-v8a (local device); override with -PtestAbi=x86_64 for CI.
    val abi = (project.findProperty("testAbi") as String?) ?: "arm64-v8a"
    val binPath = layout.buildDirectory
        .file("intermediates/cmake/debug/obj/$abi/game_tests")
        .get().asFile.absolutePath
    doLast {
        val binFile = File(binPath)
        if (!binFile.exists()) {
            throw GradleException("Native test binary not found at $binPath. " +
                    "Ensure externalNativeBuildDebug has run for ABI $abi.")
        }
        fun adb(vararg args: String) {
            println("Executing: adb ${args.joinToString(" ")}")
            val rc = ProcessBuilder("adb", *args).inheritIO().start().waitFor()
            if (rc != 0) {
                throw GradleException("adb ${args.toList()} exited $rc")
            }
        }
        adb("push", binPath, "/data/local/tmp/game_tests")
        adb("shell", "chmod", "+x", "/data/local/tmp/game_tests")
        adb("shell", "/data/local/tmp/game_tests")
    }
}

android {
    namespace = "com.jpcottin.vulkanspaceinvaders"
    // 37 is required by androidx.xr.projected (AI Glasses); the game itself
    // only needs 24+.
    compileSdk = 37
    ndkVersion = "29.0.14206865"

    defaultConfig {
        applicationId = "com.jpcottin.vulkanspaceinvaders"
        // Vulkan requires API 24+.
        minSdk = 24
        targetSdk = 36
        versionCode = 1
        versionName = "1.0"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

        externalNativeBuild {
            cmake {
                cppFlags += "-std=c++17"
                arguments += "-DANDROID_STL=c++_static"
            }
        }
        ndk {
            // Emulator is x86_64; arm64-v8a covers physical devices.
            abiFilters += listOf("arm64-v8a", "x86_64")
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    buildFeatures {
        compose = false
        aidl = false
        buildConfig = false
    }
}

dependencies {
    // AI Glasses: projected-display detection + activity launch (API 36+ at runtime).
    implementation(libs.androidx.xr.projected)
    implementation(libs.kotlinx.coroutines.android)

    androidTestImplementation(libs.androidx.test.runner)
    androidTestImplementation(libs.androidx.test.ext.junit)
}

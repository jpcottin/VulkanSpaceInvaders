# Vulkan Space Invaders

A classic **Space Invaders**-style arcade game for Android, rendered entirely with
**Vulkan** from native C++ (NDK). No game engine, no textures, no bundled assets:
every sprite is generated geometry, every sound is synthesized in real time.

Sibling project of [VulkanAsteroids](https://github.com/jpcottin/VulkanAsteroids) —
same renderer, audio engine, and CI architecture, with a brand-new game on top.

## Gameplay

Defend the bottom of the screen from descending waves of invaders across
**10 levels**.

| Element | Behaviour |
|---|---|
| **Controls** | Press the strip **below the ship**: the ship steers toward your finger and **auto-fires** while you press. |
| **Invaders** | Level 1: 3 rows × 8. Rows grow to 5 with level. March side to side, drop down at each edge, speed up as the wave thins. |
| **Row tiers** | Squid (top) 30 pts · Crab (middle) 20 pts · Octopus (bottom) 10 pts — all × level. |
| **Bombs** | Bottom-most invader of a column fires sporadically. You can shoot bombs down. |
| **Mystery saucer** | Crosses the top with a warbling siren. 100 pts × level. |
| **Power-ups** | 8% drop on kill: **Shield** (absorbs one hit, until hit) or **Rapid fire** (8 s). +25 pts × level. |
| **Lives** | 3. Lose one to a bomb hit or an invader collision. |
| **Invasion** | If the wave reaches the control strip, the invasion succeeds — instant game over. |
| **Level clear** | +100 pts × level. Clear level 10 to win. |
| **High scores** | Top 5 persisted locally, podium on the title screen. |
| **Auto Play** | Toggle in Settings (gear icon): an autopilot dodges bombs, collects power-ups, leads the saucer, and hunts columns — through the exact same control path as a finger. |
| **Settings** | Gear icon (top right): sound on/off, Auto Play on/off. Persisted. |

## Tech

- **Pure native**: `android.app.NativeActivity` + `android_main` in C++17. The only
  Kotlin in the repo is the instrumented smoke test.
- **Vulkan 1.0 renderer** (`vk_renderer.cpp`): one pipeline, one vertex buffer holding
  every shape, per-draw push constants (2×2 transform + color + fill style), FIFO
  present, two frames in flight. Logs a full Vulkan extension audit at startup.
- **Sprites as geometry**: the invaders are classic 2-frame pixel bitmaps expanded
  into triangles at startup; the player ship is the three-layer delta-wing fighter
  from VulkanAsteroids. HUD digits are 7-segment quads, text is a stroke font.
- **Shaders**: GLSL compiled to SPIR-V **at build time** by the NDK's `glslc`
  (`-mfmt=c`) and `#include`d as C arrays — no runtime shader compiler, no assets.
- **Audio** (`audio.cpp`): [Oboe](https://github.com/google/oboe) low-latency stream,
  everything procedurally synthesized — laser sweeps, explosion noise, the classic
  **four-note march bass** that accelerates with the wave, a saucer siren, and an
  ambient A-minor music bed.
- **Haptics**: JNI `VibrationEffect` pulse on ship hits.

## Building

```bash
./gradlew assembleDebug          # requires NDK 29 + CMake 3.22.1
android run --apks app/build/outputs/apk/debug/app-debug.apk
```

## Testing

| Layer | What | How |
|---|---|---|
| Native unit tests | 55 Google Test cases: formation marching, tier scoring, bombs, collisions, invasion, power-ups, saucer, settings persistence, high scores, auto-play AI | `./gradlew runNativeTests` (pushes `game_tests` to a device/emulator; `-PtestAbi=x86_64` for x86 emulators) |
| Instrumented smoke test | Launches the NativeActivity, asserts RESUMED after Vulkan init, captures a screenshot | `./gradlew connectedAndroidTest` |

With several devices connected, pin one: `ANDROID_SERIAL=emulator-5556 ./gradlew ...`

## CI (GitHub Actions)

| Job | What it does |
|---|---|
| **Build APK** | `assembleDebug`, uploads the APK artifact |
| **Native Tests** | Runs the Google Test binary on x86_64 emulators (API 34 + 36) |
| **Smoke Test** | `connectedAndroidTest` + launch + screenshot + logcat on API 34/36, plus non-blocking API 37 16 KB-page-size legs across three GPU backends |

## License

Apache 2.0 — see [LICENSE](LICENSE).

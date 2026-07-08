# Vulkan Space Invaders

A 2D **Space Invaders-like** game for **Android**, rendered with **Vulkan**.

Defend the bottom of the screen from marching waves of invaders across ten
increasingly difficult levels. Sibling project of
[VulkanAsteroids](https://github.com/jpcottin/VulkanAsteroids) — same
hand-written renderer, procedural audio engine and CI architecture, with a
brand-new game on top.

## Gameplay

| Control | Action |
|---------|--------|
| Hold the **strip below the ship** (bottom ~15% of screen) | The ship steers toward your finger **and auto-fires** while pressed |
| Tap the **gear icon** (top-right) | Open Settings from any screen |
| Tap anywhere on the title / game-over screen | Start / return to title |

- **Goal:** destroy every invader in the wave before it reaches your line.
- **Progression:** 10 levels. Level 1 fields 3 rows of 8 invaders; a row is
  added every other level (capped at 5 rows of 8). Each level marches faster,
  bombs more often, and starts lower.
- **Marching:** the wave sweeps side to side, drops down at each edge, and —
  as in the 1978 original — **speeds up as it thins**; the last survivor is
  ~3.5× faster than a full wave.
- **Row tiers:** 🟣 Squid (top) 30 pts · 🟢 Crab (middle) 20 pts ·
  🟠 Octopus (bottom) 10 pts — all ×level. Classic two-frame sprite animation
  synced to the march beat.
- **Bombs:** the bottom-most invader of a random column fires; bombs wobble as
  they fall and can be **shot down** in mid-air. Concurrent bombs and bomb
  speed scale with level.
- **Mystery saucer:** crosses the top of the screen with a warbling siren —
  100 pts ×level, with the bonus value flashed where it died.
- **Power-ups:** 8% drop chance on each kill: 🔵 Shield (absorbs one hit,
  lasts until hit) · 🟡 Rapid fire (halved cooldown, 8 s with HUD timer bar).
  +25 pts ×level on pickup.
- **Lives:** 3. Lose one to a bomb hit or an invader collision (the invader is
  destroyed too); invulnerability blinks after each hit.
- **Invasion:** if the wave reaches the control strip, the invasion succeeds —
  **instant game over**, regardless of remaining lives.
- **Level clear:** +100 pts ×level. Clear level 10 to win.
- **HUD:** score (top-left) · lives as mini-ships (top-center) · level
  (top-right) · active power-ups with timer bars (left edge).
- **Ship banking:** the ship tilts ±20° into its direction of travel.
- **High scores:** top-5 leaderboard persisted locally; gold/silver/bronze
  podium on the title screen; gold pulsing score + rank medal on game over
  when a new record is set.
- **Screen shake** on ship hits; the HUD stays stable.
- **Haptic feedback** (50 ms vibration) on ship hit.
- **Sound:** all effects synthesised in real time — laser sweeps, explosion
  bursts, hit thuds, a rising pickup sparkle, the saucer siren, an ambient
  A-minor music bed, and the classic **four-note march bass** that accelerates
  with the wave.
- **Settings:** gear icon opens an overlay from any game state. Toggles:
  Sound on/off, Auto Play. Both persist across app restarts.
- **Auto Play:** AI autopilot — dodges incoming bombs, intercepts falling
  power-ups when nothing is shooting at it, lead-aims the saucer and the
  marching columns, and fires when aligned. It drives the exact same control
  path as a finger. Activate from Settings; the gear turns green with a
  pulsing "AUTO" label while active.

## Tech

- **Pure native C++17** — Android
  [`NativeActivity`](https://developer.android.com/ndk/reference/group/native-activity)
  with `native_app_glue`; no Kotlin, no Compose, no Java (the only Kotlin file
  is the instrumented smoke test).
- **Hand-written Vulkan 1.0 2D renderer** — one graphics pipeline, one vertex
  buffer holding every shape, per-draw push constants (2×2 transform +
  translation + RGBA + fill style), FIFO present, two frames in flight.
- **Sprites as geometry, no textures** — the three invader types are classic
  11×8-ish **pixel-art bitmaps expanded into triangles at startup**, two march
  frames each; the saucer is a 16×7 bitmap. The player ship is the same
  three-layer delta-wing fighter as VulkanAsteroids (dark blue-gray swept
  wings, bright cyan fuselage, white nose spike), matching the app icon.
  Player lasers and alien bombs are two-layer bolts (glow + bright core);
  explosions are expanding glow rings (radial-falloff fragment style) plus 10
  spinning debris fragments. Two-speed parallax starfield (60 far + 20 near
  stars). Stroke vector font for text; 7-segment font for HUD numbers.
- **Procedural audio via [Oboe](https://github.com/google/oboe)** — an 8-voice
  synth on a low-latency mono float stream: pitch-swept laser, noise-burst
  explosions, hit thud, level-clear arpeggio, pickup sparkle, the four-note
  square-wave march loop (A2–G2–F2–E2), a warbling saucer siren with click-free
  envelope, and an 80 BPM ambient track (pad + bass + arpeggio). No audio files.
- **GLSL → SPIR-V** compiled at build time with the NDK's `glslc` (`-mfmt=c`)
  and `#include`d directly as C arrays — no runtime shader compiler, no assets.
- **Runtime diagnostics via Logcat** — every launch logs a full Vulkan
  extension audit (`✓ USED` / `~ PRESENT` / `✗ ABSENT` / `? UNKNOWN`) for
  instance and device extensions, and a periodic FPS line (frames/s, average
  frame time, draw calls) every 5 s. Filter with `adb logcat -s SpaceInvaders`.
- `minSdk 24` (Vulkan requires API 24+), AGP 9, NDK r29, CMake 3.22.

## Build & run

```bash
# Build
./gradlew assembleDebug

# Install and launch with the Android CLI
android run --apks app/build/outputs/apk/debug/app-debug.apk

# Or with adb
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n com.jpcottin.vulkanspaceinvaders/android.app.NativeActivity
```

Requires a device with a Vulkan driver (API 24+).

## Testing

### Native unit tests (Google Test)

55 tests covering the formation (rows per level, march direction, edge
reversal + descent, speed-up as the wave thins, side-margin containment),
invasion game-over, alien-ship collision, touch-strip ship control (steer,
stop-on-finger, clamping, zone boundaries), firing (auto-fire, cooldown,
3-laser cap, despawn), per-tier kill scoring, bombs (drop cadence, concurrency
cap, ship hits, invulnerability window, shoot-down), the saucer (crossing,
despawn, bonus payout), power-ups (pickup, shield absorb, shield persistence,
rapid-fire cooldown + expiry), level progression (clear bonus, advance, win at
10, game over on zero lives), the settings state machine (gear tap, toggles,
back button, persistence across instances), high-score persistence, and the
Auto Play AI (autonomous fire, bomb dodging, power-up interception, saucer
lead-aiming, wave completion). Run on a connected device or emulator:

```bash
# ARM device (default)
./gradlew runNativeTests

# x86_64 emulator
./gradlew runNativeTests -PtestAbi=x86_64

# Several devices connected? Pin one:
ANDROID_SERIAL=emulator-5556 ./gradlew runNativeTests
```

### Instrumented smoke test

Launches the `NativeActivity` on a connected device, waits 4 s for Vulkan to
initialise, asserts the activity is still `RESUMED`, and captures a
screenshot:

```bash
./gradlew connectedAndroidTest
```

## CI/CD

Three GitHub Actions jobs run on every push and pull request to `main`:

| Job | What it does | Artifacts |
|-----|-------------|-----------|
| **Build APK** | Compiles the debug APK | `debug-apk` |
| **Native Tests** | Runs the 55 Google Test cases on x86\_64 emulators (API 34 + API 36) | — |
| **Smoke Test** | Runs the Android instrumented test on x86\_64 emulators and captures an in-game screenshot via `UiAutomation`. Blocking on API 34 + 36; non-blocking preview legs on API 37.0 (`google_apis_ps16k`, 16 KB pages) across the swiftshader / lavapipe / auto GPU backends | `smoke-screenshot-api*`, `smoke-test-results-api*`, `smoke-logcat-api*` (suffixed per leg) |

## License

[Apache License 2.0](LICENSE).

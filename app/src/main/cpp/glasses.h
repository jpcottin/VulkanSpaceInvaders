#pragma once
#include <atomic>

struct android_app;

// JNI bridge to GlassesBridge.kt / GlassesGameActivity. All calls attach the
// current thread on demand and are safe from the game loop.

// True when this android_app instance is the GlassesGameActivity (projected
// display) rather than the phone's NativeActivity.
bool glassesIsGlassesActivity(android_app* app);

// Phone side: begin observing glasses connect/disconnect (no-op below API 36).
void glassesStartMonitoring(android_app* app);

// Latest connect state observed by the bridge (cheap volatile read JVM-side).
bool glassesIsConnected(android_app* app);

// Launch the game on the projected display. Returns false if it failed.
bool glassesLaunch(android_app* app);

// Ask the current activity to finish (used by the glasses instance when the
// phone requests the game back).
void glassesFinishActivity(android_app* app);

// Shared between the phone and glasses android_main instances (same process,
// same .so): true while a glasses session is running. The phone instance
// freezes its gameplay and offers "PLAY ON PHONE"; clearing it makes the
// glasses instance finish itself.
extern std::atomic<bool> g_glassesSessionActive;

#include <android_native_app_glue.h>
#include <android/input.h>
#include <time.h>
#include <vector>
#include <jni.h>
#include <thread>
#include <chrono>

#include "common.h"
#include "vk_renderer.h"
#include "game.h"
#include "audio.h"
#include "glasses.h"

// Trigger a 50 ms haptic pulse on a detached thread so the game loop never
// blocks. The pulse thread gets its own global ref (minted on the calling
// thread while `activity` is valid), so it can never race NativeActivity
// deleting its activity reference during teardown.
static void triggerHaptic(JavaVM* vm, JNIEnv* callerEnv, jobject activity) {
    if (!callerEnv || !activity) return;
    jobject ref = callerEnv->NewGlobalRef(activity);
    if (!ref) return;

    std::thread([vm, ref]() {
        JNIEnv* env = nullptr;
        vm->AttachCurrentThread(&env, nullptr);
        if (!env) return;

        jclass  cls        = env->GetObjectClass(ref);
        jstring svcStr     = env->NewStringUTF("vibrator");
        jmethodID getSvc   = env->GetMethodID(cls, "getSystemService",
                                              "(Ljava/lang/String;)Ljava/lang/Object;");
        jobject vibrator   = env->CallObjectMethod(ref, getSvc, svcStr);
        if (env->ExceptionCheck()) { env->ExceptionClear(); vibrator = nullptr; }
        env->DeleteLocalRef(svcStr);

        if (vibrator) {
            jclass vibCls = env->GetObjectClass(vibrator);
            jclass veCls  = env->FindClass("android/os/VibrationEffect");
            if (veCls) {
                jmethodID create    = env->GetStaticMethodID(veCls, "createOneShot",
                                                             "(JI)Landroid/os/VibrationEffect;");
                jmethodID doVibrate = env->GetMethodID(vibCls, "vibrate",
                                                       "(Landroid/os/VibrationEffect;)V");
                if (create && doVibrate) {
                    jobject effect = env->CallStaticObjectMethod(veCls, create,
                                                                 (jlong)50, (jint)-1);
                    // Calling further JNI with an exception pending is illegal:
                    // check after each call that can throw.
                    if (env->ExceptionCheck()) { env->ExceptionClear(); effect = nullptr; }
                    if (effect) {
                        env->CallVoidMethod(vibrator, doVibrate, effect);
                        // Clear any SecurityException (missing VIBRATE permission) so
                        // the thread exits cleanly instead of crashing the process.
                        env->ExceptionClear();
                        env->DeleteLocalRef(effect);
                    }
                } else {
                    env->ExceptionClear();
                }
                env->DeleteLocalRef(veCls);
            } else {
                env->ExceptionClear();
            }
            env->DeleteLocalRef(vibrator);
        }
        env->DeleteGlobalRef(ref);
        vm->DetachCurrentThread();
    }).detach();
}

struct Engine {
    android_app* app = nullptr;
    VkRenderer   renderer;
    Game         game;
    AudioEngine  audio;
    bool instanceReady = false;
    bool focused = true;
    // Phone vs AI-Glasses role (see android_main); the projected window may
    // never hold window focus, so focus-driven pausing is phone-only.
    bool glassesRole = false;
    // Consecutive renderer recovery failures, for the retry backoff.
    int  recoveryFailures = 0;
    double lastTime = 0.0;
    // FPS log accumulators. Per engine, not static: the phone and glasses
    // activities run their own android_main threads in one process.
    float fpsAccum  = 0.0f;
    int   fpsFrames = 0;
};

static double now_s() {
    struct timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

// A launch-unique RNG seed. Mixing the integer fields avoids the
// double -> uint32_t conversion of a large uptime, which is undefined once
// the value exceeds UINT32_MAX (after ~72 min on the monotonic clock) and
// saturates to a constant on AArch64.
static uint32_t seedFromClock() {
    struct timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)t.tv_nsec ^ ((uint32_t)t.tv_sec * 2654435761u);
}

// Minimal glue save-state: enough to resume a run after process death.
// Entity positions aren't persisted — the level restarts with a fresh wave
// but the same level, score, and lives.
struct SavedGame {
    int32_t level;
    int64_t score;
    int32_t lives;
};

static void handle_cmd(android_app* app, int32_t cmd) {
    auto* e = (Engine*)app->userData;
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            if (app->window && e->instanceReady) {
                if (!e->renderer.initWindow(app->window) && e->renderer.unsupported()) {
                    // No physical device / no graphics+present queue: the
                    // window would stay black forever, same as no instance.
                    LOGE("Vulkan device unavailable — finishing activity");
                    ANativeActivity_finish(app->activity);
                    break;
                }
                // Any other failure is retried from the main loop's recovery timer.
                if (!e->glassesRole && !e->focused) e->audio.pause();   // open silent, start on focus
                e->audio.init();
                e->game.setAudioEngine(&e->audio);
                e->lastTime = now_s();
            } else if (app->window && !e->instanceReady) {
                // No usable Vulkan driver: without this the loop blocks in
                // ALooper_pollOnce(-1) behind a frozen black window forever.
                LOGE("Vulkan unavailable — finishing activity");
                ANativeActivity_finish(app->activity);
            }
            break;
        case APP_CMD_SAVE_STATE:
            if (e->game.isMidGame()) {
                auto* s = (SavedGame*)malloc(sizeof(SavedGame));  // glue frees it
                *s = {(int32_t)e->game.level(), (int64_t)e->game.score(),
                      (int32_t)e->game.lives()};
                app->savedState = s;
                app->savedStateSize = sizeof(SavedGame);
            }
            break;
        case APP_CMD_TERM_WINDOW:
            e->game.setAudioEngine(nullptr);
            e->audio.shutdown();
            e->renderer.termWindow();
            break;
        case APP_CMD_LOST_FOCUS:
            // Shade pulled, system dialog, multi-window focus change: the
            // main loop stops updating the game so lives aren't lost
            // unattended (phone role only — see the paused check there),
            // and the music / siren stop with it.
            e->focused = false;
            if (!e->glassesRole) e->audio.pause();
            break;
        case APP_CMD_GAINED_FOCUS:
            e->focused = true;
            e->audio.resume();
            // Another instance (phone <-> glasses) may have saved scores or
            // settings while we were away; adopt them instead of clobbering
            // the files with our stale startup copy on the next save.
            e->game.reloadFromDisk();
            e->lastTime = now_s();   // don't feed the paused time as one dt
            break;
        default:
            break;
    }
}

static int32_t handle_input(android_app* app, AInputEvent* ev) {
    auto* e = (Engine*)app->userData;
    if (AInputEvent_getType(ev) != AINPUT_EVENT_TYPE_MOTION) return 0;

    int32_t action = AMotionEvent_getAction(ev);
    int32_t flag = action & AMOTION_EVENT_ACTION_MASK;
    int32_t idx = (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK)
                  >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;

    switch (flag) {
        case AMOTION_EVENT_ACTION_DOWN:
        case AMOTION_EVENT_ACTION_POINTER_DOWN: {
            int id = AMotionEvent_getPointerId(ev, idx);
            e->game.onPointerDown(id, AMotionEvent_getX(ev, idx), AMotionEvent_getY(ev, idx));
            break;
        }
        case AMOTION_EVENT_ACTION_UP:
        case AMOTION_EVENT_ACTION_POINTER_UP: {
            int id = AMotionEvent_getPointerId(ev, idx);
            e->game.onPointerUp(id);
            break;
        }
        case AMOTION_EVENT_ACTION_MOVE: {
            size_t n = AMotionEvent_getPointerCount(ev);
            for (size_t i = 0; i < n; i++) {
                int id = AMotionEvent_getPointerId(ev, i);
                e->game.onPointerMove(id, AMotionEvent_getX(ev, i), AMotionEvent_getY(ev, i));
            }
            break;
        }
        case AMOTION_EVENT_ACTION_CANCEL:
            e->game.onPointersCancel();
            break;
        default:
            break;
    }
    return 1;
}

void android_main(android_app* app) {
    Engine engine;
    engine.app = app;
    app->userData = &engine;
    app->onAppCmd = handle_cmd;
    app->onInputEvent = handle_input;

    engine.instanceReady = engine.renderer.initInstance();
    engine.game.setDataPath(app->activity->internalDataPath);
    // Tests keep the fixed default seed; production launches should differ.
    engine.game.seedRng(seedFromClock());

    // Resume a run killed by the system (see SavedGame above).
    if (app->savedState && app->savedStateSize == sizeof(SavedGame)) {
        auto* s = (const SavedGame*)app->savedState;
        engine.game.restoreSession(s->level, (long)s->score, s->lives);
    }

    // Keep the game thread JNI-attached for its whole life: glasses polling
    // reuses the attachment (ScopedEnv's GetEnv succeeds), and haptic pulses
    // mint their per-pulse global refs from a reference we own instead of
    // racing NativeActivity's own activity ref against teardown.
    JNIEnv* mainEnv = nullptr;
    app->activity->vm->AttachCurrentThread(&mainEnv, nullptr);
    jobject activityRef =
        mainEnv ? mainEnv->NewGlobalRef(app->activity->clazz) : nullptr;

    engine.game.setHapticCallback([app, mainEnv, activityRef]() {
        triggerHaptic(app->activity->vm, mainEnv, activityRef);
    });

    // Phone vs AI-Glasses role: the same android_main runs for both the
    // launcher NativeActivity and GlassesGameActivity (projected display).
    const bool glassesRole = glassesIsGlassesActivity(app);
    engine.glassesRole = glassesRole;
    if (glassesRole) {
        LOGI("Running on the glasses (touchbar controls)");
        engine.game.setControlMode(Game::CONTROL_TOUCHBAR);
        // The projected display runs at 30 Hz: every buffered frame costs a
        // full 33 ms, so trim the present queue to its minimum.
        engine.renderer.setLowLatencyMode(true);
        // Floating quarter-size window: easier on the lenses and 4x fewer
        // pixels to rasterise and encode into the projection stream.
        engine.renderer.setRenderScale(0.5f);
        g_glassesSessionActive = true;
    } else {
        glassesStartMonitoring(app);
        engine.game.setGlassesLaunchCallback([app]() {
            bool ok = glassesLaunch(app);
            if (ok) g_glassesSessionActive = true;
            return ok;
        });
        engine.game.setGlassesExitCallback([]() {
            g_glassesSessionActive = false;   // the glasses instance sees this and finishes
        });
    }
    float glassesPollTimer = 0.0f;
    bool  finishRequested  = false;

    engine.lastTime = now_s();

    std::vector<DrawCmd> cmds;   // reused across frames: no per-frame allocation
    while (true) {
        int events;
        android_poll_source* source;
        // A renderer that lost its swapchain (or device) while the window is
        // still up is retried on a timer rather than waiting for a window
        // event that may never come.
        bool recovering = engine.renderer.needsRecovery();
        // Exponential backoff (250 ms .. 8 s) while the rebuild keeps failing.
        int backoff = 250 << (engine.recoveryFailures < 5 ? engine.recoveryFailures : 5);
        int timeout = engine.renderer.ready() ? 0 : (recovering ? backoff : -1);
        while (ALooper_pollOnce(timeout, nullptr, &events, (void**)&source) >= 0) {
            if (source) source->process(app, source);
            if (app->destroyRequested) {
                if (glassesRole) g_glassesSessionActive = false;
                engine.renderer.cleanup();
                if (mainEnv && activityRef) mainEnv->DeleteGlobalRef(activityRef);
                if (mainEnv) app->activity->vm->DetachCurrentThread();
                return;
            }
            timeout = 0;  // drain remaining events without blocking
        }

        if (recovering && !engine.renderer.ready()) {
            engine.renderer.tryRecover();
            if (engine.renderer.ready()) {
                engine.recoveryFailures = 0;
                engine.lastTime = now_s();
            } else if (++engine.recoveryFailures >= 20 || engine.renderer.unsupported()) {
                // ~2 minutes of rebuild attempts (a wedged driver after a
                // device loss): give up the same way INIT_WINDOW does.
                LOGE("Renderer could not be recovered — finishing activity");
                ANativeActivity_finish(app->activity);
                engine.recoveryFailures = 0;
            }
        }

        if (engine.renderer.ready()) {
            double now = now_s();
            float dt = (float)(now - engine.lastTime);
            engine.lastTime = now;

            if (glassesRole) {
                // The phone asked for the game back (Settings -> BACK TO PHONE).
                if (!g_glassesSessionActive && !finishRequested) {
                    finishRequested = true;
                    glassesFinishActivity(app);
                }
            } else {
                // Poll the Kotlin bridge at ~1 Hz (JNI call) and mirror the
                // process-wide session flag into the game every frame.
                glassesPollTimer += dt;
                if (glassesPollTimer >= 1.0f) {
                    glassesPollTimer = 0.0f;
                    engine.game.setGlassesConnected(glassesIsConnected(app));
                }
                engine.game.setGlassesActive(g_glassesSessionActive.load());
            }

            engine.game.setViewport(engine.renderer.width(), engine.renderer.height());
            // Pause gameplay while unfocused — but only on the phone: the
            // projected glasses window may never hold window focus at all.
            if (glassesRole || engine.focused) engine.game.update(dt);

            cmds.clear();
            engine.game.render(cmds);
            float clear[3];
            engine.game.clearColor(clear);
            engine.renderer.drawFrame(cmds, clear);

            engine.fpsAccum  += dt;
            engine.fpsFrames += 1;
            if (engine.fpsAccum >= 5.0f) {
                LOGI("FPS: %.1f  |  avg frame: %.2f ms  |  draws/frame: %zu",
                     (float)engine.fpsFrames / engine.fpsAccum,
                     engine.fpsAccum / (float)engine.fpsFrames * 1000.0f,
                     cmds.size());
                engine.fpsAccum  = 0.0f;
                engine.fpsFrames = 0;
            }

            if (glassesRole) {
                // MAILBOX present free-runs (no vsync block). Pace just above
                // the 30 Hz panel so the queued frame stays fresh (low
                // latency) without burning CPU on frames that would be
                // discarded anyway.
                double spent = now_s() - now;
                const double kTarget = 1.0 / 34.0;
                if (spent < kTarget)
                    std::this_thread::sleep_for(
                        std::chrono::duration<double>(kTarget - spent));
            } else if (engine.game.isIdleScreen()) {
                // Idle screens (title / game over / win) don't need 60 fps.
                // Sleeping past one 60 Hz vsync period makes the FIFO present
                // snap to every second vsync (~30 fps), saving battery. A
                // plain 16 ms sleep would only overlap the present-wait.
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
        }
    }
}

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

// Trigger a 50 ms haptic pulse on a detached thread so the game loop never blocks.
static void triggerHaptic(android_app* app) {
    JavaVM* vm = app->activity->vm;
    jobject activityRef = app->activity->clazz;

    std::thread([vm, activityRef]() {
        JNIEnv* env = nullptr;
        vm->AttachCurrentThread(&env, nullptr);
        if (!env) return;

        jclass  cls        = env->GetObjectClass(activityRef);
        jstring svcStr     = env->NewStringUTF("vibrator");
        jmethodID getSvc   = env->GetMethodID(cls, "getSystemService",
                                              "(Ljava/lang/String;)Ljava/lang/Object;");
        jobject vibrator   = env->CallObjectMethod(activityRef, getSvc, svcStr);
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
                    env->CallVoidMethod(vibrator, doVibrate, effect);
                    // Clear any SecurityException (missing VIBRATE permission) so
                    // the thread exits cleanly instead of crashing the process.
                    env->ExceptionClear();
                    env->DeleteLocalRef(effect);
                }
                env->DeleteLocalRef(veCls);
            }
            env->DeleteLocalRef(vibrator);
        }
        vm->DetachCurrentThread();
    }).detach();
}

struct Engine {
    android_app* app = nullptr;
    VkRenderer   renderer;
    Game         game;
    AudioEngine  audio;
    bool instanceReady = false;
    double lastTime = 0.0;
};

static double now_s() {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static void handle_cmd(android_app* app, int32_t cmd) {
    auto* e = (Engine*)app->userData;
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            if (app->window && e->instanceReady) {
                e->renderer.initWindow(app->window);
                e->audio.init();
                e->game.setAudioEngine(&e->audio);
                e->lastTime = now_s();
            }
            break;
        case APP_CMD_TERM_WINDOW:
            e->game.setAudioEngine(nullptr);
            e->audio.shutdown();
            e->renderer.termWindow();
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
    engine.game.setHapticCallback([app]() { triggerHaptic(app); });

    // Phone vs AI-Glasses role: the same android_main runs for both the
    // launcher NativeActivity and GlassesGameActivity (projected display).
    const bool glassesRole = glassesIsGlassesActivity(app);
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

    while (true) {
        int events;
        android_poll_source* source;
        int timeout = engine.renderer.ready() ? 0 : -1;
        while (ALooper_pollOnce(timeout, nullptr, &events, (void**)&source) >= 0) {
            if (source) source->process(app, source);
            if (app->destroyRequested) {
                if (glassesRole) g_glassesSessionActive = false;
                engine.renderer.cleanup();
                return;
            }
            timeout = 0;  // drain remaining events without blocking
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
            engine.game.update(dt);

            std::vector<DrawCmd> cmds;
            engine.game.render(cmds);
            float clear[3];
            engine.game.clearColor(clear);
            engine.renderer.drawFrame(cmds, clear);

            static float fpsAccum  = 0.0f;
            static int   fpsFrames = 0;
            fpsAccum  += dt;
            fpsFrames += 1;
            if (fpsAccum >= 5.0f) {
                LOGI("FPS: %.1f  |  avg frame: %.2f ms  |  draws/frame: %zu",
                     fpsFrames / fpsAccum,
                     fpsAccum / fpsFrames * 1000.0f,
                     cmds.size());
                fpsAccum  = 0.0f;
                fpsFrames = 0;
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

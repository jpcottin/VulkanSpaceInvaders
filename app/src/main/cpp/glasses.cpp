#include "glasses.h"
#include "common.h"

#include <android_native_app_glue.h>
#include <jni.h>
#include <cstring>

std::atomic<bool> g_glassesSessionActive{false};

namespace {

// Scoped JNIEnv for the calling thread. NativeActivity threads are not
// attached by default; detach again only if we did the attach.
class ScopedEnv {
public:
    explicit ScopedEnv(android_app* app) : vm_(app->activity->vm) {
        if (vm_->GetEnv(reinterpret_cast<void**>(&env_), JNI_VERSION_1_6) != JNI_OK) {
            if (vm_->AttachCurrentThread(&env_, nullptr) == JNI_OK) attached_ = true;
            else env_ = nullptr;
        }
    }
    ~ScopedEnv() { if (attached_) vm_->DetachCurrentThread(); }
    JNIEnv* get() const { return env_; }

private:
    JavaVM* vm_;
    JNIEnv* env_ = nullptr;
    bool attached_ = false;
};

// Load an app class through the activity's ClassLoader — FindClass on a
// native thread only sees system classes.
jclass loadAppClass(JNIEnv* env, jobject activity, const char* name) {
    jclass activityCls = env->GetObjectClass(activity);
    jmethodID getLoader = env->GetMethodID(activityCls, "getClassLoader",
                                           "()Ljava/lang/ClassLoader;");
    jobject loader = env->CallObjectMethod(activity, getLoader);
    jclass loaderCls = env->GetObjectClass(loader);
    jmethodID loadClass = env->GetMethodID(loaderCls, "loadClass",
                                           "(Ljava/lang/String;)Ljava/lang/Class;");
    jstring jname = env->NewStringUTF(name);
    auto cls = (jclass)env->CallObjectMethod(loader, loadClass, jname);
    env->DeleteLocalRef(jname);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return nullptr;
    }
    return cls;
}

// GlassesBridge class + static method IDs, resolved once and cached: the
// connection poll runs every second for the process lifetime, and repeating
// the ClassLoader.loadClass + GetStaticMethodID dance each time is pure
// waste. The global ref pins the class, which keeps the jmethodIDs valid.
// Only the phone instance's game thread calls this — no locking needed.
struct BridgeCache {
    jclass    cls             = nullptr;   // global ref
    jmethodID startMonitoring = nullptr;
    jmethodID isConnected     = nullptr;
    jmethodID launchOnGlasses = nullptr;
};
BridgeCache g_bridge;

// A failed GetStaticMethodID leaves NoSuchMethodError pending; detaching a
// thread (ScopedEnv destructor) with it pending would kill the process.
jmethodID staticMethod(JNIEnv* env, jclass cls, const char* name, const char* sig) {
    jmethodID m = env->GetStaticMethodID(cls, name, sig);
    if (!m) { env->ExceptionClear(); LOGW("GlassesBridge.%s not found", name); }
    return m;
}

const BridgeCache* bridge(JNIEnv* env, android_app* app) {
    if (g_bridge.cls) return &g_bridge;
    jclass cls = loadAppClass(env, app->activity->clazz,
                              "com.jpcottin.vulkanspaceinvaders.GlassesBridge");
    if (!cls) { LOGW("GlassesBridge class not found"); return nullptr; }
    jmethodID start  = staticMethod(env, cls, "startMonitoring",
                                    "(Landroid/content/Context;)V");
    jmethodID isConn = staticMethod(env, cls, "isConnected", "()Z");
    jmethodID launch = staticMethod(env, cls, "launchOnGlasses",
                                    "(Landroid/app/Activity;)Z");
    if (!start || !isConn || !launch) return nullptr;
    g_bridge.cls = (jclass)env->NewGlobalRef(cls);
    if (!g_bridge.cls) return nullptr;
    g_bridge.startMonitoring = start;
    g_bridge.isConnected     = isConn;
    g_bridge.launchOnGlasses = launch;
    return &g_bridge;
}

}  // namespace

bool glassesIsGlassesActivity(android_app* app) {
    ScopedEnv se(app);
    JNIEnv* env = se.get();
    if (!env) return false;
    jclass cls = env->GetObjectClass(app->activity->clazz);
    jmethodID getName = env->GetMethodID(env->GetObjectClass(cls), "getName",
                                         "()Ljava/lang/String;");
    auto jname = (jstring)env->CallObjectMethod(cls, getName);
    const char* name = env->GetStringUTFChars(jname, nullptr);
    bool isGlasses = strstr(name, "GlassesGameActivity") != nullptr;
    env->ReleaseStringUTFChars(jname, name);
    return isGlasses;
}

void glassesStartMonitoring(android_app* app) {
    ScopedEnv se(app);
    JNIEnv* env = se.get();
    if (!env) return;
    const BridgeCache* b = bridge(env, app);
    if (!b) return;
    env->CallStaticVoidMethod(b->cls, b->startMonitoring, app->activity->clazz);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

bool glassesIsConnected(android_app* app) {
    ScopedEnv se(app);
    JNIEnv* env = se.get();
    if (!env) return false;
    const BridgeCache* b = bridge(env, app);
    if (!b) return false;
    jboolean r = env->CallStaticBooleanMethod(b->cls, b->isConnected);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    return r == JNI_TRUE;
}

bool glassesLaunch(android_app* app) {
    ScopedEnv se(app);
    JNIEnv* env = se.get();
    if (!env) return false;
    const BridgeCache* b = bridge(env, app);
    if (!b) return false;
    jboolean r = env->CallStaticBooleanMethod(b->cls, b->launchOnGlasses,
                                              app->activity->clazz);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    return r == JNI_TRUE;
}

void glassesFinishActivity(android_app* app) {
    ANativeActivity_finish(app->activity);
}

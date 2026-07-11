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

jclass bridgeClass(JNIEnv* env, android_app* app) {
    return loadAppClass(env, app->activity->clazz,
                        "com.jpcottin.vulkanspaceinvaders.GlassesBridge");
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
    jclass cls = bridgeClass(env, app);
    if (!cls) { LOGW("GlassesBridge class not found"); return; }
    jmethodID m = env->GetStaticMethodID(cls, "startMonitoring",
                                         "(Landroid/content/Context;)V");
    if (!m) { env->ExceptionClear(); LOGW("startMonitoring not found"); return; }
    env->CallStaticVoidMethod(cls, m, app->activity->clazz);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

bool glassesIsConnected(android_app* app) {
    ScopedEnv se(app);
    JNIEnv* env = se.get();
    if (!env) return false;
    jclass cls = bridgeClass(env, app);
    if (!cls) return false;
    jmethodID m = env->GetStaticMethodID(cls, "isConnected", "()Z");
    // A failed lookup leaves NoSuchMethodError pending; detaching the thread
    // (ScopedEnv destructor) with it pending would kill the process.
    if (!m) { env->ExceptionClear(); return false; }
    jboolean r = env->CallStaticBooleanMethod(cls, m);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    return r == JNI_TRUE;
}

bool glassesLaunch(android_app* app) {
    ScopedEnv se(app);
    JNIEnv* env = se.get();
    if (!env) return false;
    jclass cls = bridgeClass(env, app);
    if (!cls) return false;
    jmethodID m = env->GetStaticMethodID(cls, "launchOnGlasses",
                                         "(Landroid/app/Activity;)Z");
    if (!m) { env->ExceptionClear(); return false; }
    jboolean r = env->CallStaticBooleanMethod(cls, m, app->activity->clazz);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    return r == JNI_TRUE;
}

void glassesFinishActivity(android_app* app) {
    ANativeActivity_finish(app->activity);
}

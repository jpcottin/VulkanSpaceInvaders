// Host stand-in for the NDK's <android/log.h>, used by the Auto Play bench
// (test/autoplay_bench.cpp), which builds game.cpp with the host compiler.
// The on-device test binary links the real liblog.
#pragma once
#include <cstdio>
#include <cstdarg>

enum { ANDROID_LOG_INFO = 4, ANDROID_LOG_WARN = 5, ANDROID_LOG_ERROR = 6 };

static inline int __android_log_print(int prio, const char* tag, const char* fmt, ...) {
    (void)prio;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s: ", tag);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    return 0;
}

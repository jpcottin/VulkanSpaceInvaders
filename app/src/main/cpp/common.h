#pragma once
#include <android/log.h>
#include <cstdint>

#define LOG_TAG "SpaceInvaders"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Drawable primitives the renderer knows how to draw.
enum Shape {
    SHAPE_QUAD = 0,        // [-1,1] square: HUD, digit segments, lasers, bombs
    SHAPE_DISC,            // unit circle (32-gon): explosion rings, gear, shield
    SHAPE_SHIP_WINGS,      // wide swept wings layer (dark blue-gray)
    SHAPE_SHIP_BODY,       // narrow fuselage layer (bright cyan)
    SHAPE_SHIP_NOSE,       // sharp nose spike layer (white)
    SHAPE_INVADER_A0,      // squid invader (top rows), march frame 0
    SHAPE_INVADER_A1,      // squid invader, march frame 1
    SHAPE_INVADER_B0,      // crab invader (middle rows), march frame 0
    SHAPE_INVADER_B1,      // crab invader, march frame 1
    SHAPE_INVADER_C0,      // octopus invader (bottom rows), march frame 0
    SHAPE_INVADER_C1,      // octopus invader, march frame 1
    SHAPE_SAUCER,          // bonus mystery saucer crossing the top
    SHAPE_COUNT
};

// Fragment-shader fill styles (DrawCmd::style).
enum FillStyle {
    STYLE_FLAT = 0,   // solid colour (default)
    STYLE_GLOW = 1,   // radial alpha falloff — soft glows and explosion rings
};

// One draw: a 2x2 linear transform + NDC translation + RGBA colour, applied to
// a shape. The renderer repacks these into per-instance vertex attributes
// (InstanceData in vk_renderer.cpp) consumed by shape.vert/.frag.
struct DrawCmd {
    float mtx[4];   // m00, m01, m10, m11
    float tx, ty;   // NDC translation
    float color[4]; // r, g, b, a
    float style;    // FillStyle (as float — goes straight into the instance attributes)
    float seed;     // reserved per-draw parameter (unused by current styles)
    int shape;
};

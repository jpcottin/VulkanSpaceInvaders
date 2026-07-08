#version 450

layout(push_constant) uniform PC {
    vec4 mtx;
    vec2 trans;
    vec2 style;  // x: FillStyle (0 = flat, 1 = glow), y: reserved
    vec4 color;
} pc;

layout(location = 0) in vec2 vLocal;
layout(location = 0) out vec4 outColor;

void main() {
    vec4 col = pc.color;

    if (pc.style.x > 0.5) {
        // Soft radial glow: full alpha at the centre, fading to nothing at the
        // silhouette. Used for explosion rings, laser halos and the shield bubble.
        float r = length(vLocal);
        col.a *= 1.0 - smoothstep(0.15, 1.0, r);
    }

    outColor = col;
}

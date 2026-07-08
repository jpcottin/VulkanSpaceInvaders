#version 450

layout(location = 0) in vec2 vLocal;
layout(location = 1) flat in vec2 vStyle;  // x: FillStyle (0 = flat, 1 = glow)
layout(location = 2) flat in vec4 vColor;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 col = vColor;

    if (vStyle.x > 0.5) {
        // Soft radial glow: full alpha at the centre, fading to nothing at the
        // silhouette. Used for explosion rings, laser halos and the shield bubble.
        float r = length(vLocal);
        col.a *= 1.0 - smoothstep(0.15, 1.0, r);
    }

    outColor = col;
}

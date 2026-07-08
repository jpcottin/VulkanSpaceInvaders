#version 450

// Per-vertex shape-local position; everything else is per-instance so that
// consecutive draws of the same shape batch into one instanced draw call.
layout(location = 0) in vec2 inPos;
layout(location = 1) in vec4 iMtx;    // m00, m01, m10, m11
layout(location = 2) in vec2 iTrans;  // NDC translation
layout(location = 3) in vec2 iStyle;  // x: FillStyle, y: reserved
layout(location = 4) in vec4 iColor;

layout(location = 0) out vec2 vLocal;       // pre-transform shape-local position
layout(location = 1) flat out vec2 vStyle;
layout(location = 2) flat out vec4 vColor;

void main() {
    vLocal = inPos;
    vStyle = iStyle;
    vColor = iColor;
    vec2 p = vec2(iMtx.x * inPos.x + iMtx.y * inPos.y,
                  iMtx.z * inPos.x + iMtx.w * inPos.y);
    gl_Position = vec4(p + iTrans, 0.0, 1.0);
}

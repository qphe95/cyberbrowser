#version 450
#extension GL_GOOGLE_include_directive : require
// A mask's end (gpu_vk.c): over the masked box, what was there before the
// mask began (saved) and what was drawn since (current) mix by the mask's
// alpha, as raster_cpu.c's DL_MASK_END does.  Drawn without blending.

#include "common.glsl"

layout(set = 1, binding = 0) uniform sampler2D mask_image;  // kind 1's image (else a 1 x 1 stand-in)
layout(set = 2, binding = 0) uniform sampler2D saved;
layout(set = 2, binding = 1) uniform sampler2D current;

layout(location = 0) flat in int v_item;
layout(location = 0) out vec4 out_color;

float mask_alpha(Mask m, vec2 p) {
    int kind = m.kind.x;
    if (kind == 2) {
        float rad = m.misc.x * 3.14159265 / 180.0;
        float dx = sin(rad), dy = -cos(rad);
        float line = abs(m.box.z * dx) + abs(m.box.w * dy);
        if (line <= 0.0) return 1.0;
        float t = ((p.x - (m.box.x + m.box.z * 0.5)) * dx + (p.y - (m.box.y + m.box.w * 0.5)) * dy) / line + 0.5;
        int n = int(m.misc.w);
        if (t <= stop_pos(m, 0)) return stop_alpha(m, 0);
        for (int k = 1; k < n; k++) {
            if (t <= stop_pos(m, k)) {
                float span = stop_pos(m, k) - stop_pos(m, k - 1);
                float f = span > 0.0 ? (t - stop_pos(m, k - 1)) / span : 1.0;
                return stop_alpha(m, k - 1) + (stop_alpha(m, k) - stop_alpha(m, k - 1)) * f;
            }
        }
        return stop_alpha(m, n - 1);
    }
    if (kind == 3) return rrect_alpha(m.box, m.rx, m.ry, p);
    if (kind == 4) {
        vec4 b = m.border;      // top, right, bottom, left
        vec4 irx = max(vec4(m.rx[0] - b[3], m.rx[1] - b[1], m.rx[2] - b[1], m.rx[3] - b[3]), 0.0);
        vec4 iry = max(vec4(m.ry[0] - b[0], m.ry[1] - b[0], m.ry[2] - b[2], m.ry[3] - b[2]), 0.0);
        float outer = rrect_alpha(m.box, m.rx, m.ry, p);
        float inner = rrect_alpha(vec4(m.box.x + b[3], m.box.y + b[0], m.box.z - b[1] - b[3], m.box.w - b[0] - b[2]),
                                  irx, iry, p);
        return outer * (1.0 - inner);
    }
    // kind 1: an image's alpha (none: transparent).
    if (m.kind.y == 0 || m.img.z <= 0.0 || m.img.w <= 0.0) return 0.0;
    ivec2 ts = textureSize(mask_image, 0);
    float u = div_(p.x - m.img.x, m.img.z), v = div_(p.y - m.img.y, m.img.w);
    if (m.misc.z != 0.0) { u -= floor(u); v -= floor(v); }
    else if (u < 0.0 || u >= 1.0 || v < 0.0 || v >= 1.0) return 0.0;
    int sx = min(int(u * float(ts.x)), ts.x - 1), sy = min(int(v * float(ts.y)), ts.y - 1);
    return texelFetch(mask_image, ivec2(sx, sy), 0).a;
}

void main() {
    Item it = items[v_item];
    ivec2 pix = ivec2(gl_FragCoord.xy);
    vec4 cur = texelFetch(current, pix, 0);
    float a = mask_alpha(masks[it.info.y], vec2(pix) + 0.5);
    if (a >= 0.999) { out_color = cur; return; }
    vec4 old = texelFetch(saved, pix, 0);
    out_color = old + (cur - old) * max(a, 0.0);
}

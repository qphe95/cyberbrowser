#version 450
#extension GL_GOOGLE_include_directive : require
// The GPU process's records (gpu_vk.c): rectangles, border rings, glyphs,
// images and box shadows, as raster_cpu.c paints them, blended over the
// target with the record's alpha.

#include "common.glsl"

layout(set = 1, binding = 0) uniform sampler2D tex;     // the glyph atlas or image, if the record has one

layout(location = 0) flat in int v_item;
layout(location = 0) out vec4 out_color;

const int RECT = 0, BORDER = 1, GLYPH = 2, IMAGE = 3, SHADOW = 6;

void main() {
    Item it = items[v_item];
    ivec2 pix = ivec2(gl_FragCoord.xy);
    vec2 p = vec2(pix) + 0.5;
    int type = it.info.x;
    float a = it.color.a;
    vec3 rgb = it.color.rgb;
    if (type == BORDER) {
        // Rings 0 .. t-1 inward, each its four edges (corners twice), each
        // blended in turn: the coverage that many blends add up to.
        int x0 = int(floor(it.rect.x)), y0 = int(floor(it.rect.y));
        int x1 = int(floor(it.rect.x + it.rect.z)), y1 = int(floor(it.rect.y + it.rect.w));
        int n = 0;
        for (int k = 0; k < it.info.z; k++) {
            int ax0 = x0 + k, ay0 = y0 + k, ax1 = x1 - k, ay1 = y1 - k;
            bool in_x = pix.x >= min(ax0, ax1) && pix.x <= max(ax0, ax1);
            bool in_y = pix.y >= min(ay0, ay1) && pix.y <= max(ay0, ay1);
            if (pix.y == ay0 && in_x) n++;
            if (pix.y == ay1 && in_x) n++;
            if (pix.x == ax0 && in_y) n++;
            if (pix.x == ax1 && in_y) n++;
        }
        if (n == 0) discard;
        a = a >= 1.0 ? 1.0 : 1.0 - pow(1.0 - a, float(n));
    } else if (type == GLYPH) {
        ivec2 ts = textureSize(tex, 0);
        float v = div_(p.y - it.rect.y, it.rect.w), u = div_(p.x - it.rect.x, it.rect.z);
        int sy = clamp(int(floor((it.uv.y + v * (it.uv.w - it.uv.y)) * float(ts.y))), 0, ts.y - 1);
        int sx = clamp(int(floor((it.uv.x + u * (it.uv.z - it.uv.x)) * float(ts.x))), 0, ts.x - 1);
        float c = texelFetch(tex, ivec2(sx, sy), 0).r;
        if (c <= 0.0) discard;
        a *= c;
    } else if (type == IMAGE) {
        ivec2 ts = textureSize(tex, 0);
        int dx0 = int(floor(it.rect.x)), dy0 = int(floor(it.rect.y));
        float v = it.rect.w > 0.0 ? div_(float(pix.y - dy0), it.rect.w) : 0.0;
        float u = it.rect.z > 0.0 ? div_(float(pix.x - dx0), it.rect.z) : 0.0;
        int sy = clamp(int(floor(v * float(ts.y))), 0, ts.y - 1);
        int sx = clamp(int(floor(u * float(ts.x))), 0, ts.x - 1);
        vec4 s = texelFetch(tex, ivec2(sx, sy), 0);
        if (s.a <= 0.0) discard;
        rgb = s.rgb;
        a = s.a;
    } else if (type == SHADOW) {
        Mask m = masks[it.info.y];
        float sigma = m.misc.y * 0.5;
        float hw = m.box.z * 0.5, hh = m.box.w * 0.5, cx = m.box.x + hw, cy = m.box.y + hh;
        int c = p.x < cx ? (p.y < cy ? 0 : 3) : (p.y < cy ? 1 : 2);
        float r = min(min(m.rx[c], m.ry[c]), min(hw, hh));
        float qx = abs(p.x - cx) - hw + r, qy = abs(p.y - cy) - hh + r;
        float ox = max(qx, 0.0), oy = max(qy, 0.0);
        float d = sqrt(ox * ox + oy * oy) + (qx > qy ? min(qx, 0.0) : min(qy, 0.0)) - r;
        float cov = sigma > 0.01 ? 0.5 * erfc_(d / (sigma * 1.41421356))
                                 : (d <= -0.5 ? 1.0 : d >= 0.5 ? 0.0 : 0.5 - d);
        cov *= 1.0 - rrect_alpha(m.img, m.ex_rx, m.ex_ry, p);
        a = cov * it.color.a;
        if (a <= 0.002) discard;
    }
    if (a <= 0.0) discard;
    out_color = vec4(rgb, min(a, 1.0));
}

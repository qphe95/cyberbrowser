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
    if (kind == 7) {
        // A polygon (clip-path): up to ten corners from the box's corner,
        // x y pairs through rx, ry, border, ex_rx, ex_ry; misc.y their
        // count; misc.x 1 for evenodd, else nonzero.
        float v[20];
        for (int k = 0; k < 4; k++) {
            v[k] = m.rx[k]; v[4 + k] = m.ry[k]; v[8 + k] = m.border[k]; v[12 + k] = m.ex_rx[k]; v[16 + k] = m.ex_ry[k];
        }
        int n = min(int(m.misc.y + 0.5), 10);
        vec2 q = p - m.box.xy;
        int wind = 0;
        bool odd = false;
        for (int i = 0; i < n; i++) {
            int j = i + 1 < n ? i + 1 : 0;
            vec2 a = vec2(v[2 * i], v[2 * i + 1]), b = vec2(v[2 * j], v[2 * j + 1]);
            if ((a.y <= q.y) != (b.y <= q.y)) {
                float x = a.x + (q.y - a.y) / (b.y - a.y) * (b.x - a.x);
                if (q.x < x) { odd = !odd; wind += b.y > a.y ? 1 : -1; }
            }
        }
        return (m.misc.x > 0.5 ? odd : wind != 0) ? 1.0 : 0.0;
    }
    if (kind == 6) {
        // A border side, as raster_cpu.c's kind 6: turned to be the top one.
        vec4 b = m.border;      // top, right, bottom, left
        int side = int(m.misc.x + 0.5) & 3;
        float u, v, len, a, c, t;
        if (side == 0) { u = p.x - m.box.x; v = p.y - m.box.y; len = m.box.z; a = b[3]; c = b[1]; t = b[0]; }
        else if (side == 2) { u = m.box.x + m.box.z - p.x; v = m.box.y + m.box.w - p.y; len = m.box.z; a = b[1]; c = b[3]; t = b[2]; }
        else if (side == 3) { u = m.box.y + m.box.w - p.y; v = p.x - m.box.x; len = m.box.w; a = b[2]; c = b[0]; t = b[3]; }
        else { u = p.y - m.box.y; v = m.box.x + m.box.z - p.x; len = m.box.w; a = b[0]; c = b[2]; t = b[1]; }
        if (t <= 0.0) return 0.0;
        float f0 = stop_pos(m, 0) * t, f1 = stop_pos(m, 1) * t;
        float cov = clamp(v - f0 + 0.5, 0.0, 1.0) * clamp(f1 - v + 0.5, 0.0, 1.0);
        // (an end not cut, stop_alpha 0 there: the side fills its corner)
        if (stop_alpha(m, 0) > 0.5) cov *= clamp((a > 0.0 ? (u * t - v * a) / sqrt(a * a + t * t) : u) + 0.5, 0.0, 1.0);
        if (stop_alpha(m, 1) > 0.5) cov *= clamp((c > 0.0 ? ((len - u) * t - v * c) / sqrt(c * c + t * t) : len - u) + 0.5, 0.0, 1.0);
        return cov;
    }
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
    // (repeat: bit 1 across, bit 2 down)
    int rep = int(m.misc.z + 0.5);
    if ((rep & 1) != 0) u -= floor(u);
    else if (u < 0.0 || u >= 1.0) return 0.0;
    if ((rep & 2) != 0) v -= floor(v);
    else if (v < 0.0 || v >= 1.0) return 0.0;
    int sx = min(int(u * float(ts.x)), ts.x - 1), sy = min(int(v * float(ts.y)), ts.y - 1);
    return texelFetch(mask_image, ivec2(sx, sy), 0).a;
}

// A separable blend mode's B(cb, cs) (Compositing 1 5.3), by mode 1 .. 11.
float blend1(int mode, float b, float s) {
    if (mode == 1) return b * s;
    if (mode == 2) return b + s - b * s;
    if (mode == 3) return b <= 0.5 ? 2.0 * b * s : 1.0 - 2.0 * (1.0 - b) * (1.0 - s);
    if (mode == 4) return min(b, s);
    if (mode == 5) return max(b, s);
    if (mode == 6) return b <= 0.0 ? 0.0 : s >= 1.0 ? 1.0 : min(1.0, b / (1.0 - s));
    if (mode == 7) return b >= 1.0 ? 1.0 : s <= 0.0 ? 0.0 : 1.0 - min(1.0, (1.0 - b) / s);
    if (mode == 8) return s <= 0.5 ? 2.0 * b * s : 1.0 - 2.0 * (1.0 - b) * (1.0 - s);
    if (mode == 9) {
        if (s <= 0.5) return b - (1.0 - 2.0 * s) * b * (1.0 - b);
        float d = b <= 0.25 ? ((16.0 * b - 12.0) * b + 4.0) * b : sqrt(b);
        return b + (2.0 * s - 1.0) * (d - b);
    }
    if (mode == 10) return abs(b - s);
    if (mode == 11) return b + s - 2.0 * b * s;
    return s;
}
float lum_(vec3 c) { return 0.3 * c.r + 0.59 * c.g + 0.11 * c.b; }
vec3 clipc_(vec3 c) {
    float l = lum_(c), n = min(min(c.r, c.g), c.b), x = max(max(c.r, c.g), c.b);
    if (n < 0.0) c = l + (c - l) * l / (l - n);
    if (x > 1.0) c = l + (c - l) * (1.0 - l) / (x - l);
    return c;
}
vec3 setlum_(vec3 c, float l) { return clipc_(c + (l - lum_(c))); }
float sat_(vec3 c) { return max(max(c.r, c.g), c.b) - min(min(c.r, c.g), c.b); }
vec3 setsat_(vec3 c, float s) {
    float mx = max(max(c.r, c.g), c.b), mn = min(min(c.r, c.g), c.b);
    if (mx <= mn) return vec3(0.0);
    return (c - mn) * s / (mx - mn);
}
vec3 blend3(int mode, vec3 b, vec3 s) {
    if (mode == 12) return setlum_(setsat_(s, sat_(b)), lum_(b));
    if (mode == 13) return setlum_(setsat_(b, sat_(s)), lum_(b));
    if (mode == 14) return setlum_(s, lum_(b));
    if (mode == 15) return setlum_(b, lum_(s));
    return vec3(blend1(mode, b.r, s.r), blend1(mode, b.g, s.g), blend1(mode, b.b, s.b));
}

void main() {
    Item it = items[v_item];
    ivec2 pix = ivec2(gl_FragCoord.xy);
    vec4 cur = texelFetch(current, pix, 0);
    Mask bm = masks[it.info.y];
    if (bm.kind.x == 9 || bm.kind.x == 10) {
        // filter: cur is the subtree alone (premultiplied); its functions in turn, then over old.
        // backdrop-filter (10): the functions on old within the box instead, cur over that.
        vec4 old0 = texelFetch(saved, pix, 0);
        bool back = bm.kind.x == 10;
        vec2 pp = vec2(pix) + 0.5;
        bool inbox = pp.x >= bm.box.x && pp.y >= bm.box.y && pp.x < bm.box.x + bm.box.z && pp.y < bm.box.y + bm.box.w;
        vec4 fg = cur;
        vec4 src = back ? vec4(old0.rgb * old0.a, old0.a) : cur;
        if (back && !inbox) {
            float ab = fg.a + old0.a * (1.0 - fg.a);
            vec3 cb2 = fg.rgb + old0.rgb * old0.a * (1.0 - fg.a);
            out_color = vec4(ab > 0.0 ? cb2 / ab : vec3(0.0), ab);
            return;
        }
        int n = min(int(bm.misc.w + 0.5), 8);
        float amt[8] = float[8](bm.rx[0], bm.rx[1], bm.rx[2], bm.rx[3], bm.ry[0], bm.ry[1], bm.ry[2], bm.ry[3]);
        for (int k = 0; k < n; k++) {
            int t = int(stop_pos(bm, k) * 32.0 + 0.5);
            float a = amt[k];
            vec3 c = src.a > 0.0 ? src.rgb / src.a : vec3(0.0);
            if (t == 1) {
                float s = 1.0 - a;
                mat3 m = mat3(0.2126 + 0.7874 * s, 0.2126 - 0.2126 * s, 0.2126 - 0.2126 * s,
                              0.7152 - 0.7152 * s, 0.7152 + 0.2848 * s, 0.7152 - 0.7152 * s,
                              0.0722 - 0.0722 * s, 0.0722 - 0.0722 * s, 0.0722 + 0.9278 * s);
                c = m * c;
            } else if (t == 2) {
                float s = 1.0 - a;
                mat3 m = mat3(0.393 + 0.607 * s, 0.349 - 0.349 * s, 0.272 - 0.272 * s,
                              0.769 - 0.769 * s, 0.686 + 0.314 * s, 0.534 - 0.534 * s,
                              0.189 - 0.189 * s, 0.168 - 0.168 * s, 0.131 + 0.869 * s);
                c = m * c;
            } else if (t == 3) {
                mat3 m = mat3(0.213 + 0.787 * a, 0.213 - 0.213 * a, 0.213 - 0.213 * a,
                              0.715 - 0.715 * a, 0.715 + 0.285 * a, 0.715 - 0.715 * a,
                              0.072 - 0.072 * a, 0.072 - 0.072 * a, 0.072 + 0.928 * a);
                c = m * c;
            } else if (t == 4) {
                float r = a * 3.14159265 / 180.0, cs = cos(r), sn = sin(r);
                mat3 m = mat3(0.213 + cs * 0.787 - sn * 0.213, 0.213 - cs * 0.213 + sn * 0.143, 0.213 - cs * 0.213 - sn * 0.787,
                              0.715 - cs * 0.715 - sn * 0.715, 0.715 + cs * 0.285 + sn * 0.140, 0.715 - cs * 0.715 + sn * 0.715,
                              0.072 - cs * 0.072 + sn * 0.928, 0.072 - cs * 0.072 - sn * 0.283, 0.072 + cs * 0.928 + sn * 0.072);
                c = m * c;
            } else if (t == 5) {
                c = c * (1.0 - a) + (1.0 - c) * a;
            } else if (t == 6) {
                src.a *= a;
            } else if (t == 7) {
                c = c * a;
            } else if (t == 8) {
                c = (c - 0.5) * a + 0.5;
            } else if (t == 10) {
                // drop-shadow: the alpha moved by the offsets, in its color, under it
                ivec2 sp = ivec2(vec2(pix) - vec2(bm.img.x, bm.img.y) + 0.5);
                float sa = texelFetch(current, sp, 0).a * bm.border.a / 255.0;
                vec4 sh = vec4(bm.border.rgb / 255.0 * sa, sa);
                vec4 pre = vec4(clamp(c, 0.0, 1.0) * src.a, src.a);
                vec4 o = pre + sh * (1.0 - pre.a);
                src = o;
                continue;
            }
            c = clamp(c, 0.0, 1.0);
            src = vec4(c * src.a, src.a);
        }
        if (back) {
            // the subtree over the filtered backdrop
            float ab = fg.a + src.a * (1.0 - fg.a);
            vec3 cb2 = fg.rgb + src.rgb * (1.0 - fg.a);
            out_color = vec4(ab > 0.0 ? cb2 / ab : vec3(0.0), ab);
            return;
        }
        // over what was there (premultiplied in, straight out as the canvas keeps it)
        float ao = src.a + old0.a * (1.0 - src.a);
        vec3 co = src.rgb + old0.rgb * old0.a * (1.0 - src.a);
        out_color = vec4(ao > 0.0 ? co / ao : vec3(0.0), ao);
        return;
    }
    if (bm.kind.x == 8) {
        // mix-blend-mode: cur is the subtree alone (premultiplied), old what's under it.
        vec4 old0 = texelFetch(saved, pix, 0);
        float as = cur.a, ab = old0.a;
        if (as <= 0.0) { out_color = old0; return; }
        vec3 cs = cur.rgb / as, cb = ab > 0.0 ? old0.rgb / ab : vec3(0.0);
        int mode = int(bm.misc.x + 0.5);
        vec3 mixed = mode == 16 ? min(cb * ab + cs * as, vec3(1.0)) : (1.0 - ab) * cs + ab * blend3(mode, cb, cs);
        if (mode == 16) { out_color = vec4(mixed, min(1.0, as + ab)); return; }
        float ao = as + ab * (1.0 - as);
        vec3 co = as * mixed + (1.0 - as) * ab * cb;   // premultiplied
        out_color = vec4(ao > 0.0 ? co / ao : vec3(0.0), ao);
        return;
    }
    float a = mask_alpha(masks[it.info.y], item_point(it, vec2(pix) + 0.5));
    if (a >= 0.999) { out_color = cur; return; }
    vec4 old = texelFetch(saved, pix, 0);
    out_color = old + (cur - old) * max(a, 0.0);
}

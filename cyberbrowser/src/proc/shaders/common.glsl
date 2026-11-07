// Shared by the GPU process's fragment shaders: the records and masks
// (gpu_vk.c fills them from the decoded display list) and the coverage
// functions of raster_cpu.c, line for line.

struct Item {
    vec4 rect;      // x, y, w, h: the record's box, px
    vec4 color;
    vec4 uv;        // u0, v0, u1, v1
    ivec4 quad;     // x0, y0, x1, y1: the pixels to cover (x1, y1 excluded)
    ivec4 info;     // type, mask index, border rings, level
    vec4 extra;
    vec4 xf0;       // under a transform: its inverse's a b c d
    vec4 xf1;       // e, f, 1 (0: none)
    ivec4 lquad;    // the pixels to cover before the transform
};

// The point the record is drawn at for the target's pixel p: p itself, or
// under a transform where it came from.
vec2 item_point(Item it, vec2 p) {
    if (it.xf1.z < 0.5) return p;
    return vec2(it.xf0.x * p.x + it.xf0.z * p.y + it.xf1.x, it.xf0.y * p.x + it.xf0.w * p.y + it.xf1.y);
}

struct Mask {
    vec4 box;       // x, y, w, h
    vec4 img;       // ix, iy, iw, ih
    vec4 rx, ry, border, ex_rx, ex_ry;
    vec4 stop_pos0, stop_pos1, stop_alpha0, stop_alpha1;
    vec4 misc;      // angle_deg, blur, repeat, nstops
    ivec4 kind;     // kind, has image
};

layout(std430, set = 0, binding = 0) readonly buffer Items { Item items[]; };
layout(std430, set = 0, binding = 1) readonly buffer Masks { Mask masks[]; };

// a / b as the CPU computes it: Vulkan's division may be 2.5 ulp off, and
// floor() of a texel coordinate that lands on an integer then picks the
// neighbouring texel.  One correction step (fma is exact) restores it.
float div_(float a, float b) {
    precise float q = a / b;
    precise float r = fma(-q, b, a);
    return q + r / b;
}

float stop_pos(Mask m, int k) { return k < 4 ? m.stop_pos0[k] : m.stop_pos1[k - 4]; }
float stop_alpha(Mask m, int k) { return k < 4 ? m.stop_alpha0[k] : m.stop_alpha1[k - 4]; }

// Coverage of a rounded rectangle at p (raster_cpu.c's rrect_alpha).
float rrect_alpha(vec4 r, vec4 rx, vec4 ry, vec2 p) {
    float lx = p.x - r.x, ly = p.y - r.y, w = r.z, h = r.w;
    if (w <= 0.0 || h <= 0.0 || lx < 0.0 || ly < 0.0 || lx > w || ly > h) return 0.0;
    int c = -1;
    float cx = 0.0, cy = 0.0;
    if (lx < rx[0] && ly < ry[0]) { c = 0; cx = rx[0]; cy = ry[0]; }
    else if (lx > w - rx[1] && ly < ry[1]) { c = 1; cx = w - rx[1]; cy = ry[1]; }
    else if (lx > w - rx[2] && ly > h - ry[2]) { c = 2; cx = w - rx[2]; cy = h - ry[2]; }
    else if (lx < rx[3] && ly > h - ry[3]) { c = 3; cx = rx[3]; cy = h - ry[3]; }
    if (c < 0 || rx[c] <= 0.0 || ry[c] <= 0.0) return 1.0;
    float nx = (lx - cx) / rx[c], ny = (ly - cy) / ry[c];
    float d = sqrt(nx * nx + ny * ny);
    float rr = min(rx[c], ry[c]);
    return clamp(0.5 - (d - 1.0) * rr, 0.0, 1.0);
}

// (corner-shape: raster_cpu.c's corner_din: the distance in from a corner's
// curve, superellipse(K), at u, v in from its edges; g its normal)
float corner_din(float u, float v, float rx, float ry, float K, out vec2 g) {
    bool cv = K < 0.0;
    float n = exp2(abs(K));
    float p = cv ? u / rx : 1.0 - u / rx, q = cv ? v / ry : 1.0 - v / ry;
    // (a bevel's line goes on past its ends; a curve's ends meet the sides square)
    if (n == 1.0) {
        vec2 g1 = vec2(1.0 / rx, 1.0 / ry);
        g = normalize(g1);
        return (1.0 - p - q) / length(g1);
    }
    p = max(p, 0.0);
    q = max(q, 0.0);
    float m = max(p, q), norm, dp, dq;
    if (m <= 0.0) { g = vec2(0.7071); return cv ? -min(rx, ry) : min(rx, ry); }
    if (n > 60.0) { norm = m; dp = p >= q ? 1.0 : 0.0; dq = 1.0 - dp; }
    else if (n == 1.0) { norm = p + q; dp = 1.0; dq = 1.0; }
    else {
        norm = m * pow(pow(p / m, n) + pow(q / m, n), 1.0 / n);
        dp = p > 0.0 ? pow(p / norm, n - 1.0) : 0.0;
        dq = q > 0.0 ? pow(q / norm, n - 1.0) : 0.0;
    }
    vec2 gg = vec2(dp / rx, dq / ry);
    float gl = max(length(gg), 1e-6);
    g = gg / gl;
    return (cv ? norm - 1.0 : 1.0 - norm) / gl;
}

// (raster_cpu.c's shaped_alpha: a rounded box with corner shapes K, its
// contour moved in by ins (t r b l))
float shaped_alpha(vec4 r, vec4 rx, vec4 ry, vec4 K, vec4 ins, vec2 p) {
    float lx = p.x - r.x, ly = p.y - r.y, w = r.z, h = r.w;
    if (w <= 0.0 || h <= 0.0 || lx < ins[3] || ly < ins[0] || lx > w - ins[1] || ly > h - ins[2]) return 0.0;
    int c = lx < w * 0.5 ? (ly < h * 0.5 ? 0 : 3) : (ly < h * 0.5 ? 1 : 2);
    float u = (c == 0 || c == 3) ? lx : w - lx, v = c < 2 ? ly : h - ly;
    float iu = (c == 0 || c == 3) ? ins[3] : ins[1], iv = c < 2 ? ins[0] : ins[2];
    if (rx[c] <= 0.0 || ry[c] <= 0.0 || K[c] >= 15.0) return 1.0;
    vec2 g;
    float d = corner_din(u, v, rx[c], ry[c], K[c], g);
    if (g.x + g.y > 0.0) d -= (g.x * iu + g.y * iv) / (g.x + g.y);
    return clamp(d + 0.5, 0.0, 1.0);
}

// erfc, with a fractional error under 1.2e-7 (Numerical Recipes' erfcc).
float erfc_(float x) {
    float z = abs(x);
    float t = 1.0 / (1.0 + 0.5 * z);
    float r = t * exp(-z * z - 1.26551223 + t * (1.00002368 + t * (0.37409196 + t * (0.09678418 +
              t * (-0.18628806 + t * (0.27886807 + t * (-1.13520398 + t * (1.48851587 +
              t * (-0.82215223 + t * 0.17087277)))))))));
    return x >= 0.0 ? r : 2.0 - r;
}

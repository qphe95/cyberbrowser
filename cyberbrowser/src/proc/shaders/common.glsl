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
};

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

// erfc, with a fractional error under 1.2e-7 (Numerical Recipes' erfcc).
float erfc_(float x) {
    float z = abs(x);
    float t = 1.0 / (1.0 + 0.5 * z);
    float r = t * exp(-z * z - 1.26551223 + t * (1.00002368 + t * (0.37409196 + t * (0.09678418 +
              t * (-0.18628806 + t * (0.27886807 + t * (-1.13520398 + t * (1.48851587 +
              t * (-0.82215223 + t * 0.17087277)))))))));
    return x >= 0.0 ? r : 2.0 - r;
}

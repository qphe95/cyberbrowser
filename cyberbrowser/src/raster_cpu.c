/*
 * raster_cpu.c - the CPU rasterizer (raster_cpu.h): paints a display list
 * into an RGB buffer and writes it as a JPEG.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "raster_cpu.h"
#include "display_list.h"
#include "font_registry.h"
#include "text_shaper.h"
#include "image_cache.h"
#include "stb_image_write.h"

typedef struct {
    uint8_t r, g, b;
} RGB;

static inline void set_pixel(RGB *pixels, int width, int height, int x, int y, RGB c) {
    if (x < 0 || y < 0 || x >= width || y >= height) return;
    pixels[y * width + x] = c;
}

static inline RGB blend_over(RGB dst, float sr, float sg, float sb, float sa) {
    if (sa <= 0.0f) return dst;
    if (sa >= 1.0f) {
        RGB r = { (uint8_t)(sr * 255.0f + 0.5f),
                  (uint8_t)(sg * 255.0f + 0.5f),
                  (uint8_t)(sb * 255.0f + 0.5f) };
        return r;
    }
    RGB out;
    out.r = (uint8_t)(dst.r * (1.0f - sa) + sr * 255.0f * sa + 0.5f);
    out.g = (uint8_t)(dst.g * (1.0f - sa) + sg * 255.0f * sa + 0.5f);
    out.b = (uint8_t)(dst.b * (1.0f - sa) + sb * 255.0f * sa + 0.5f);
    return out;
}

static void fill_rect(RGB *pixels, int width, int height,
                      int x0, int y0, int x1, int y1,
                      float r, float g, float b, float a) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= width) x1 = width - 1;
    if (y1 >= height) y1 = height - 1;
    for (int y = y0; y <= y1; y++) {
        RGB *row = &pixels[y * width];
        for (int x = x0; x <= x1; x++) {
            row[x] = blend_over(row[x], r, g, b, a);
        }
    }
}

static void draw_hline(RGB *pixels, int width, int height,
                       int x0, int x1, int y,
                       float r, float g, float b, float a) {
    if (y < 0 || y >= height) return;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (x0 < 0) x0 = 0;
    if (x1 >= width) x1 = width - 1;
    RGB *row = &pixels[y * width];
    for (int x = x0; x <= x1; x++) {
        row[x] = blend_over(row[x], r, g, b, a);
    }
}

static void draw_vline(RGB *pixels, int width, int height,
                       int x, int y0, int y1,
                       float r, float g, float b, float a) {
    if (x < 0 || x >= width) return;
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    if (y0 < 0) y0 = 0;
    if (y1 >= height) y1 = height - 1;
    for (int y = y0; y <= y1; y++) {
        RGB *p = &pixels[y * width + x];
        *p = blend_over(*p, r, g, b, a);
    }
}

static void draw_rect_outline(RGB *pixels, int width, int height,
                              int x0, int y0, int x1, int y1,
                              float r, float g, float b, float a) {
    draw_hline(pixels, width, height, x0, x1, y0, r, g, b, a);
    draw_hline(pixels, width, height, x0, x1, y1, r, g, b, a);
    draw_vline(pixels, width, height, x0, y0, y1, r, g, b, a);
    draw_vline(pixels, width, height, x1, y0, y1, r, g, b, a);
}

static void draw_glyph(RGB *pixels, int img_width, int img_height,
                       const uint8_t *atlas, int atlas_w, int atlas_h,
                       const DisplayListCmd *cmd)
{
    /* Resample the atlas region into the destination rect (cmd->x/y/w/h) so
     * glyphs render at the scaled font size, not the 16px atlas size.
     * Sampling at pixel centers keeps the nearest-neighbor lookup stable. */
    if (cmd->w <= 0.0f || cmd->h <= 0.0f) return;
    int dx0 = (int)floorf(cmd->x);
    int dy0 = (int)floorf(cmd->y);
    int dx1 = (int)ceilf(cmd->x + cmd->w);
    int dy1 = (int)ceilf(cmd->y + cmd->h);
    if (dx0 < 0) dx0 = 0;
    if (dy0 < 0) dy0 = 0;
    if (dx1 > img_width) dx1 = img_width;
    if (dy1 > img_height) dy1 = img_height;
    if (dx0 >= dx1 || dy0 >= dy1) return;

    for (int dy = dy0; dy < dy1; dy++) {
        float v = (dy + 0.5f - cmd->y) / cmd->h;
        float av = cmd->u.glyph.v0 + v * (cmd->u.glyph.v1 - cmd->u.glyph.v0);
        int sy = (int)floorf(av * (float)atlas_h);
        if (sy < 0) sy = 0;
        if (sy >= atlas_h) sy = atlas_h - 1;
        for (int dx = dx0; dx < dx1; dx++) {
            float u = (dx + 0.5f - cmd->x) / cmd->w;
            float au = cmd->u.glyph.u0 + u * (cmd->u.glyph.u1 - cmd->u.glyph.u0);
            int sx = (int)floorf(au * (float)atlas_w);
            if (sx < 0) sx = 0;
            if (sx >= atlas_w) sx = atlas_w - 1;
            uint8_t a8 = atlas[sy * atlas_w + sx];
            if (a8 == 0) continue;
            float alpha = (a8 / 255.0f) * cmd->a;
            pixels[dy * img_width + dx] = blend_over(pixels[dy * img_width + dx],
                                                      cmd->r, cmd->g, cmd->b, alpha);
        }
    }
}

static void draw_image(RGB *pixels, int img_width, int img_height,
                       ImageCache *cache, const DisplayListCmd *cmd)
{
    int src_w = 0, src_h = 0, src_ch = 0;
    uint8_t *src = NULL;
    if (!image_cache_get(cache, cmd->u.image.image_handle,
                         &src_w, &src_h, &src_ch, &src)) {
        return;
    }
    if (src_w <= 0 || src_h <= 0) return;

    int dx0 = (int)floorf(cmd->x);
    int dy0 = (int)floorf(cmd->y);
    int dx1 = (int)floorf(cmd->x + cmd->w);
    int dy1 = (int)floorf(cmd->y + cmd->h);
    if (dx0 < 0) dx0 = 0;
    if (dy0 < 0) dy0 = 0;
    if (dx1 > img_width) dx1 = img_width;
    if (dy1 > img_height) dy1 = img_height;
    if (dx0 >= dx1 || dy0 >= dy1) return;

    for (int dy = dy0; dy < dy1; dy++) {
        float v = (cmd->h > 0.0f) ? ((float)(dy - dy0) / cmd->h) : 0.0f;
        int sy = (int)floorf(v * (float)src_h);
        if (sy < 0) sy = 0;
        if (sy >= src_h) sy = src_h - 1;
        for (int dx = dx0; dx < dx1; dx++) {
            float u = (cmd->w > 0.0f) ? ((float)(dx - dx0) / cmd->w) : 0.0f;
            int sx = (int)floorf(u * (float)src_w);
            if (sx < 0) sx = 0;
            if (sx >= src_w) sx = src_w - 1;
            int si = (sy * src_w + sx) * 4;
            uint8_t a = src[si + 3];
            if (a == 0) continue;
            pixels[dy * img_width + dx] = blend_over(pixels[dy * img_width + dx],
                                                     src[si] / 255.0f,
                                                     src[si + 1] / 255.0f,
                                                     src[si + 2] / 255.0f,
                                                     a / 255.0f);
        }
    }
}

/* Coverage of a rounded rectangle at (px, py): inside unless in a corner's
 * quadrant and outside its ellipse (about one pixel of anti-aliasing). */
static float rrect_alpha(float x, float y, float w, float h, const float *rx, const float *ry,
                         float px, float py)
{
    float lx = px - x, ly = py - y;
    if (w <= 0.0f || h <= 0.0f || lx < 0.0f || ly < 0.0f || lx > w || ly > h) return 0.0f;
    int c = -1;
    float cx = 0, cy = 0;
    if (lx < rx[0] && ly < ry[0]) { c = 0; cx = rx[0]; cy = ry[0]; }
    else if (lx > w - rx[1] && ly < ry[1]) { c = 1; cx = w - rx[1]; cy = ry[1]; }
    else if (lx > w - rx[2] && ly > h - ry[2]) { c = 2; cx = w - rx[2]; cy = h - ry[2]; }
    else if (lx < rx[3] && ly > h - ry[3]) { c = 3; cx = rx[3]; cy = h - ry[3]; }
    if (c < 0 || rx[c] <= 0.0f || ry[c] <= 0.0f) return 1.0f;
    float nx = (lx - cx) / rx[c], ny = (ly - cy) / ry[c];
    float d = sqrtf(nx * nx + ny * ny);
    float r = rx[c] < ry[c] ? rx[c] : ry[c];
    float a = 0.5f - (d - 1.0f) * r;
    return a < 0.0f ? 0.0f : a > 1.0f ? 1.0f : a;
}

/* Mask alpha at pixel centre (px, py): a linear gradient along the CSS
 * angle through the box, or an image's alpha (src: w x h x ch, NULL while
 * not decoded = transparent). */
static float mask_alpha_at(const DisplayMask *m, float px, float py,
                           const uint8_t *src, int sw, int sh, int sch)
{
    if (m->kind == 2) {
        float rad = m->angle_deg * 3.14159265f / 180.0f;
        float dx = sinf(rad), dy = -cosf(rad);
        float line = fabsf(m->w * dx) + fabsf(m->h * dy);
        if (line <= 0.0f) return 1.0f;
        float t = ((px - (m->x + m->w * 0.5f)) * dx + (py - (m->y + m->h * 0.5f)) * dy) / line + 0.5f;
        if (t <= m->stop_pos[0]) return m->stop_alpha[0];
        for (int k = 1; k < m->nstops; k++) {
            if (t <= m->stop_pos[k]) {
                float span = m->stop_pos[k] - m->stop_pos[k - 1];
                float f = span > 0.0f ? (t - m->stop_pos[k - 1]) / span : 1.0f;
                return m->stop_alpha[k - 1] + (m->stop_alpha[k] - m->stop_alpha[k - 1]) * f;
            }
        }
        return m->stop_alpha[m->nstops - 1];
    }
    if (m->kind == 3)
        return rrect_alpha(m->x, m->y, m->w, m->h, m->rx, m->ry, px, py);
    if (m->kind == 4) {
        /* Border ring: the rounded box minus its inset by the border
         * widths (inner radii shrink by the adjacent widths). */
        const float *b = m->border;   /* top, right, bottom, left */
        float irx[4] = { m->rx[0] - b[3], m->rx[1] - b[1], m->rx[2] - b[1], m->rx[3] - b[3] };
        float iry[4] = { m->ry[0] - b[0], m->ry[1] - b[0], m->ry[2] - b[2], m->ry[3] - b[2] };
        for (int k = 0; k < 4; k++) { if (irx[k] < 0.0f) irx[k] = 0.0f; if (iry[k] < 0.0f) iry[k] = 0.0f; }
        float outer = rrect_alpha(m->x, m->y, m->w, m->h, m->rx, m->ry, px, py);
        float inner = rrect_alpha(m->x + b[3], m->y + b[0], m->w - b[1] - b[3], m->h - b[0] - b[2], irx, iry, px, py);
        return outer * (1.0f - inner);
    }
    if (!src || sw <= 0 || sh <= 0 || m->iw <= 0.0f || m->ih <= 0.0f) return 0.0f;
    float u = (px - m->ix) / m->iw, v = (py - m->iy) / m->ih;
    if (m->repeat) { u -= floorf(u); v -= floorf(v); }
    else if (u < 0.0f || u >= 1.0f || v < 0.0f || v >= 1.0f) return 0.0f;
    int sx = (int)(u * (float)sw), sy = (int)(v * (float)sh);
    if (sx >= sw) sx = sw - 1;
    if (sy >= sh) sy = sh - 1;
    if (sch < 4) return 1.0f;
    return (float)src[((size_t)sy * sw + sx) * sch + 3] / 255.0f;
}

typedef struct {
    int x0, y0, x1, y1;
    RGB *saved;
    int mask;
} MaskLayer;

bool render_display_list_to_jpg(const DisplayList *dl, const char *path,
                                       int img_width, int img_height) {
    MaskLayer layers[64];
    int nlayers = 0;
    size_t pixel_count = (size_t)img_width * (size_t)img_height;
    RGB *pixels = (RGB *)calloc(pixel_count, sizeof(RGB));
    if (!pixels) {
        printf("FATAL: out of memory allocating %dx%d image buffer\n", img_width, img_height);
        return false;
    }

    /* White background. */
    for (size_t i = 0; i < pixel_count; i++) {
        pixels[i].r = 255;
        pixels[i].g = 255;
        pixels[i].b = 255;
    }

    /* Per-slot atlases (font_registry.h), resolved when a glyph first
     * references the slot. */
    TextShaper *slot_fonts[FONT_SLOTS_MAX] = {0};
    const uint8_t *slot_atlas[FONT_SLOTS_MAX] = {0};
    int slot_atlas_w[FONT_SLOTS_MAX] = {0};
    int slot_atlas_h[FONT_SLOTS_MAX] = {0};

    for (int i = 0; i < dl->count; i++) {
        const DisplayListCmd *cmd = &dl->cmds[i];
        int x0 = (int)floorf(cmd->x);
        int y0 = (int)floorf(cmd->y);
        int x1 = (int)floorf(cmd->x + cmd->w);
        int y1 = (int)floorf(cmd->y + cmd->h);

        if (cmd->type == DL_RECT) {
            fill_rect(pixels, img_width, img_height,
                      x0, y0, x1, y1,
                      cmd->r, cmd->g, cmd->b, cmd->a);
        } else if (cmd->type == DL_BORDER) {
            float thickness = cmd->u.border.thickness;
            if (thickness <= 0) thickness = 1.0f;
            int t = (int)ceilf(thickness);
            for (int offset = 0; offset < t; offset++) {
                draw_rect_outline(pixels, img_width, img_height,
                                  x0 + offset, y0 + offset,
                                  x1 - offset, y1 - offset,
                                  cmd->r, cmd->g, cmd->b, cmd->a);
            }
        } else if (cmd->type == DL_GLYPH) {
            int slot = cmd->u.glyph.font_slot;
            if (slot < FONT_SLOTS_MAX) {
                if (!slot_fonts[slot]) {
                    slot_fonts[slot] = font_registry_slot(slot);
                    slot_atlas[slot] = slot_fonts[slot]
                        ? text_shaper_atlas_pixels(slot_fonts[slot]) : NULL;
                    slot_atlas_w[slot] = slot_fonts[slot]
                        ? text_shaper_atlas_width(slot_fonts[slot]) : 0;
                    slot_atlas_h[slot] = slot_fonts[slot]
                        ? text_shaper_atlas_height(slot_fonts[slot]) : 0;
                }
                if (slot_atlas[slot]) {
                    draw_glyph(pixels, img_width, img_height,
                               slot_atlas[slot], slot_atlas_w[slot],
                               slot_atlas_h[slot], cmd);
                }
            }
        } else if (cmd->type == DL_IMAGE) {
            ImageCache *cache = display_list_get_image_cache();
            if (cache) draw_image(pixels, img_width, img_height, cache, cmd);
        } else if (cmd->type == DL_SHADOW) {
            /* Blurred rounded rectangle: coverage from the signed distance
             * to its edge through a Gaussian (sigma = blur / 2), minus the
             * part under the box that casts it. */
            const DisplayMask *m = &dl->masks[cmd->u.mask_index];
            int sx0 = (int)floorf(cmd->x), sy0 = (int)floorf(cmd->y);
            int sx1 = (int)ceilf(cmd->x + cmd->w), sy1 = (int)ceilf(cmd->y + cmd->h);
            if (sx0 < 0) sx0 = 0;
            if (sy0 < 0) sy0 = 0;
            if (sx1 > img_width) sx1 = img_width;
            if (sy1 > img_height) sy1 = img_height;
            float sigma = m->blur * 0.5f;
            float hw = m->w * 0.5f, hh = m->h * 0.5f, cx = m->x + hw, cy = m->y + hh;
            for (int y = sy0; y < sy1; y++) {
                for (int x = sx0; x < sx1; x++) {
                    float px = (float)x + 0.5f, py = (float)y + 0.5f;
                    int c = px < cx ? (py < cy ? 0 : 3) : (py < cy ? 1 : 2);
                    float r = m->rx[c] < m->ry[c] ? m->rx[c] : m->ry[c];
                    if (r > hw) r = hw;
                    if (r > hh) r = hh;
                    float qx = fabsf(px - cx) - hw + r, qy = fabsf(py - cy) - hh + r;
                    float ox = qx > 0.0f ? qx : 0.0f, oy = qy > 0.0f ? qy : 0.0f;
                    float d = sqrtf(ox * ox + oy * oy) + (qx > qy ? (qx < 0.0f ? qx : 0.0f) : (qy < 0.0f ? qy : 0.0f)) - r;
                    float cov = sigma > 0.01f ? 0.5f * erfcf(d / (sigma * 1.41421356f))
                                              : (d <= -0.5f ? 1.0f : d >= 0.5f ? 0.0f : 0.5f - d);
                    cov *= 1.0f - rrect_alpha(m->ix, m->iy, m->iw, m->ih, m->ex_rx, m->ex_ry, px, py);
                    float a = cov * cmd->a;
                    if (a <= 0.002f) continue;
                    RGB *p = &pixels[(size_t)y * img_width + x];
                    p->r = (uint8_t)(p->r + ((float)(cmd->r * 255.0f) - p->r) * a + 0.5f);
                    p->g = (uint8_t)(p->g + ((float)(cmd->g * 255.0f) - p->g) * a + 0.5f);
                    p->b = (uint8_t)(p->b + ((float)(cmd->b * 255.0f) - p->b) * a + 0.5f);
                }
            }
        } else if (cmd->type == DL_MASK_BEGIN) {
            /* Remember what is under the masked box; MASK_END blends the
             * box's painting back over it by the mask's alpha. */
            const DisplayMask *m = &dl->masks[cmd->u.mask_index];
            MaskLayer *L = nlayers < 64 ? &layers[nlayers] : NULL;
            if (!L) continue;
            L->x0 = (int)floorf(m->x); L->y0 = (int)floorf(m->y);
            L->x1 = (int)ceilf(m->x + m->w); L->y1 = (int)ceilf(m->y + m->h);
            if (L->x0 < 0) L->x0 = 0;
            if (L->y0 < 0) L->y0 = 0;
            if (L->x1 > img_width) L->x1 = img_width;
            if (L->y1 > img_height) L->y1 = img_height;
            L->mask = cmd->u.mask_index;
            L->saved = NULL;
            if (L->x1 > L->x0 && L->y1 > L->y0) {
                int w = L->x1 - L->x0;
                L->saved = (RGB *)malloc((size_t)w * (size_t)(L->y1 - L->y0) * sizeof(RGB));
                for (int y = L->y0; L->saved && y < L->y1; y++)
                    memcpy(L->saved + (size_t)(y - L->y0) * w, pixels + (size_t)y * img_width + L->x0,
                           (size_t)w * sizeof(RGB));
            }
            nlayers++;
        } else if (cmd->type == DL_MASK_END) {
            if (nlayers == 0) continue;
            MaskLayer *L = &layers[--nlayers];
            const DisplayMask *m = &dl->masks[L->mask];
            int sw = 0, sh = 0, sch = 0;
            uint8_t *src = NULL;
            ImageCache *cache = display_list_get_image_cache();
            if (m->kind == 1 && cache && !image_cache_get(cache, m->image_handle, &sw, &sh, &sch, &src)) src = NULL;
            int w = L->x1 - L->x0;
            for (int y = L->y0; L->saved && y < L->y1; y++) {
                for (int x = L->x0; x < L->x1; x++) {
                    float a = mask_alpha_at(m, (float)x + 0.5f, (float)y + 0.5f, src, sw, sh, sch);
                    if (a >= 0.999f) continue;
                    RGB *cur = &pixels[(size_t)y * img_width + x];
                    const RGB *old = &L->saved[(size_t)(y - L->y0) * w + (x - L->x0)];
                    if (a < 0.0f) a = 0.0f;
                    cur->r = (uint8_t)(old->r + (cur->r - old->r) * a + 0.5f);
                    cur->g = (uint8_t)(old->g + (cur->g - old->g) * a + 0.5f);
                    cur->b = (uint8_t)(old->b + (cur->b - old->b) * a + 0.5f);
                }
            }
            free(L->saved);
        }
    }
    while (nlayers > 0) free(layers[--nlayers].saved);

    /* stbi_write_jpg expects interleaved RGB. */
    int ok = stbi_write_jpg(path, img_width, img_height, 3, pixels, 95);
    free(pixels);
    return ok != 0;
}

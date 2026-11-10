/*
 * Display List - Implementation
 */

#include "http_download.h"
#include "display_list.h"
#include "platform.h"
#include "url_utils.h"
#include "text_shaper.h"
#include "font_registry.h"
#include "url_whatwg.h"
#include "image_cache.h"
#include "raster_cpu.h"
#include "svg_filter.h"
#include "third_party/plutosvg/plutosvg.h"
#include "svg_render.h"
#include "browser_api_impl_types.h"
#include "dom_store.h"
#include "css_parser.h"
#include "dom_query.h"
#include "browser/webgpu_bridge.h"

/* The frame a <video> shows now (browser/media_source_api.c). */
bool media_video_frame_for_node(uint32_t node, const uint8_t **rgba, int *w, int *h,
                                uint64_t *frame_id);

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <math.h>
#include <pthread.h>
#include "css_props.h"

#define LOG_TAG "display_list"
#define LOG_ERROR(...) platform_log(LOG_LEVEL_ERROR, LOG_TAG, __VA_ARGS__)

void display_list_init(DisplayList *dl)
{
    if (!dl) return;
    dl->cmds = NULL;
    dl->count = 0;
    dl->capacity = 0;
    dl->masks = NULL;
    dl->mask_count = dl->mask_capacity = 0;
}

void display_list_free(DisplayList *dl)
{
    if (!dl) return;
    free(dl->cmds);
    dl->cmds = NULL;
    dl->count = 0;
    dl->capacity = 0;
    free(dl->masks);
    dl->masks = NULL;
    dl->mask_count = dl->mask_capacity = 0;
}

bool display_list_reserve(DisplayList *dl, int extra)
{
    if (!dl) return false;
    int needed = dl->count + extra;
    if (needed <= dl->capacity) return true;

    int new_capacity = dl->capacity ? dl->capacity * 2 : 64;
    while (new_capacity < needed) new_capacity *= 2;

    DisplayListCmd *new_cmds = (DisplayListCmd*)realloc(dl->cmds, new_capacity * sizeof(DisplayListCmd));
    if (!new_cmds) {
        LOG_ERROR("Out of memory growing display list");
        return false;
    }
    dl->cmds = new_cmds;
    dl->capacity = new_capacity;
    return true;
}

/* (the last rectangle snapped to whole pixels: where it was put, and how far each edge moved to get there) */
#define DL_SNAP_MARK 7777.0f
static __thread float t_snap_at[4], t_snap_by[4];
static __thread bool t_snap_on;
bool display_list_add_rect(DisplayList *dl, float x, float y, float w, float h,
                           float r, float g, float b, float a)
{
    if (!display_list_reserve(dl, 1)) return false;
    DisplayListCmd *cmd = &dl->cmds[dl->count++];
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = DL_RECT;
    cmd->x = x; cmd->y = y; cmd->w = w; cmd->h = h;
    cmd->r = r; cmd->g = g; cmd->b = b; cmd->a = a;
    /* (a snapped fill: its true edges with it, for what moves it by a fraction of a pixel) */
    if (t_snap_on && x == t_snap_at[0] && y == t_snap_at[1] && w == t_snap_at[2] && h == t_snap_at[3]) {
        memcpy(cmd->u.xf, t_snap_by, sizeof t_snap_by);
        cmd->u.xf[5] = DL_SNAP_MARK;
    }
    return true;
}

bool display_list_add_border(DisplayList *dl, float x, float y, float w, float h,
                             float thickness, float r, float g, float b, float a)
{
    if (!display_list_reserve(dl, 1)) return false;
    DisplayListCmd *cmd = &dl->cmds[dl->count++];
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = DL_BORDER;
    cmd->x = x; cmd->y = y; cmd->w = w; cmd->h = h;
    cmd->r = r; cmd->g = g; cmd->b = b; cmd->a = a;
    cmd->u.border.thickness = thickness;
    return true;
}

/* (font-palette: the palette and override colors this box's color glyphs use) */
static void dl_apply_font_palette(const LayoutStyle *st)
{
    const char *v = st->font_palette;
    if (!v) { text_shaper_set_palette(0, 0, NULL, NULL, 0); return; }
    if (!strcasecmp(v, "light")) { text_shaper_set_palette(1, 0, NULL, NULL, 0); return; }
    if (!strcasecmp(v, "dark")) { text_shaper_set_palette(2, 0, NULL, NULL, 0); return; }
    int kind = 0, index = 0, n = 0, oi[16];
    float oc[16][4];
    char over[512];
    over[0] = 0;
    if (strncmp(v, "--", 2) || !font_palette_values_get(st->font_family, v, &kind, &index, over, sizeof over)) {
        text_shaper_set_palette(0, 0, NULL, NULL, 0);
        return;
    }
    /* (index color, index color, ...) */
    for (const char *p = over; *p && n < 16;) {
        while (isspace((unsigned char)*p) || *p == ',') p++;
        if (!isdigit((unsigned char)*p)) break;
        char *e;
        long ix = strtol(p, &e, 10);
        p = e;
        while (isspace((unsigned char)*p)) p++;
        const char *ce = p;
        int depth = 0;
        while (*ce && (depth || *ce != ',')) { if (*ce == '(') depth++; else if (*ce == ')') depth--; ce++; }
        char col[64];
        snprintf(col, sizeof col, "%.*s", (int)(ce - p), p);
        double rgba[4];
        if (css_layout_parse_color_text(col, rgba)) {
            oi[n] = (int)ix;
            for (int k = 0; k < 4; k++) oc[n][k] = (float)rgba[k];
            n++;
        }
        p = ce;
    }
    text_shaper_set_palette(kind, index, oi, (const float (*)[4])oc, n);
}

bool display_list_add_glyph(DisplayList *dl, float x, float y, float w, float h,
                            float u0, float v0, float u1, float v1,
                            uint32_t glyph_index,
                            float r, float g, float b, float a)
{
    if (!display_list_reserve(dl, 1)) return false;
    DisplayListCmd *cmd = &dl->cmds[dl->count++];
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = DL_GLYPH;
    cmd->x = x; cmd->y = y; cmd->w = w; cmd->h = h;
    cmd->r = r; cmd->g = g; cmd->b = b; cmd->a = a;
    cmd->u.glyph.u0 = u0; cmd->u.glyph.v0 = v0;
    cmd->u.glyph.u1 = u1; cmd->u.glyph.v1 = v1;
    cmd->u.glyph.glyph_index = glyph_index;
    return true;
}

bool display_list_add_image(DisplayList *dl, float x, float y, float w, float h,
                            int image_handle,
                            float u0, float v0, float u1, float v1)
{
    if (!display_list_reserve(dl, 1)) return false;
    DisplayListCmd *cmd = &dl->cmds[dl->count++];
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = DL_IMAGE;
    cmd->x = x; cmd->y = y; cmd->w = w; cmd->h = h;
    cmd->u.image.image_handle = image_handle;
    cmd->u.image.u0 = u0; cmd->u.image.v0 = v0;
    cmd->u.image.u1 = u1; cmd->u.image.v1 = v1;
    return true;
}

static ImageCache *g_image_cache = NULL;

static void emit_image(DisplayList *dl, float x, float y, float w, float h, int handle);
static bool grad_image_key(const char *g, int w, int h, char *key, size_t kn);
static __thread double t_grad_em = 16.0;   /* (an em in a gradient's stops: the box's font size) */
static bool dl_paint_contained(const LayoutBox *b);
static void dl_clip_range(DisplayList *dl, int from, const float c[4]);

/* A box's background image (CSS Backgrounds §3): sized (background-size),
 * placed in the padding box (or the viewport, fixed) by background-position
 * and repeated per background-repeat, clipped to the border box (to area,
 * when given: the canvas, for the root's). */
/* How far table box i's captions (outside the table box) reach below its
 * top (*bot: above its bottom). */
static float table_caption_top(const LayoutContext *ctx, int i, const LayoutBox *box, float *bot)
{
    double ext = 0.0, ebot = 0.0;
    for (int c = ctx->tree.nodes[i].first_child_idx; c >= 0; c = ctx->tree.nodes[c].next_sibling_idx) {
        const LayoutBox *cb = &ctx->boxes[c];
        if (!(cb->s->table_caption || cb->s->table_role == 5) || cb->s->display == CSS_DISPLAY_NONE) continue;
        if (css_layout_caption_bottom(ctx, c)) {
            double e = box->y + box->height - (cb->y - cb->margin_top);
            if (e > ebot) ebot = e;
            continue;
        }
        double e = cb->y + cb->height + cb->margin_bottom - box->y;
        if (e > ext) ext = e;
    }
    *bot = (float)ebot;
    return (float)ext;
}

static void emit_background_image(const LayoutContext *ctx, DisplayList *dl, const LayoutBox *box,
                                  const float *area)
{
    const LayoutStyle *s = box->s;
    t_grad_em = s->font_size > 0.0 ? s->font_size : 16.0;
    int nw = 0, nh = 0;
    const char *grad = s->bg_grad;
    if (grad) nw = nh = 1;   /* (a gradient: no size of its own, the tile's) */
    else if (!image_cache_natural_size(g_image_cache, s->background_image_url, &nw, &nh) || nw <= 0 || nh <= 0) {
        image_cache_request(g_image_cache, s->background_image_url, 0, 0);
        return;
    }
    double bx = box->x, by = box->y, bw = box->width, bh = box->height;
    /* positioning area: background-origin's box, the padding box at first
     * (the viewport when fixed) */
    double ax = bx + s->border_left, ay = by + s->border_top;
    double aw = bw - s->border_left - s->border_right, ah = bh - s->border_top - s->border_bottom;
    if (s->bg_origin == 1) { ax = bx; ay = by; aw = bw; ah = bh; }
    else if (s->bg_origin == 2) {
        ax += box->padding_left; ay += box->padding_top;
        aw -= box->padding_left + box->padding_right; ah -= box->padding_top + box->padding_bottom;
    }
    /* (fixed, but in a transformed box or under one: as scroll, CSS Transforms 1 6) */
    bool fixed_vp = s->bg_fixed == 1;
    for (int p = (int)(box - ctx->boxes); fixed_vp && p >= 0 && p < ctx->tree.count; p = ctx->tree.nodes[p].parent_idx) {
        const LayoutStyle *ps = ctx->boxes[p].s;
        if (ps && (ps->tf_text || ps->tf_rotate || ps->tf_scale)) fixed_vp = false;
    }
    if (fixed_vp) { ax = 0.0; ay = 0.0; aw = ctx->viewport_width; ah = ctx->viewport_height; }
    else if (s->bg_fixed == 2) {
        /* (local, on a scroller: the area its content scrolls over, moved as it is scrolled) */
        int bi = (int)(box - ctx->boxes);
        if (bi >= 0 && bi < ctx->tree.count && css_layout_box_scrolls(ctx, bi)) {
            double right = ax + aw, bottom = ay + ah;
            css_layout_extent(ctx, bi, &right, &bottom);
            double sw = right - ax + (s->bg_origin == 2 ? 0.0 : box->padding_right), sh = bottom - ay + (s->bg_origin == 2 ? 0.0 : box->padding_bottom);
            if (sw > aw) aw = sw;
            if (sh > ah) ah = sh;
            float sx = 0.0f, sy = 0.0f;
            if (css_layout_box_offset(ctx, bi, &sx, &sy)) { ax -= sx; ay -= sy; }
        }
    }
    if (aw <= 0.0 || ah <= 0.0) return;
    /* (an image without a size or ratio of its own, an SVG: CSS
     * Backgrounds 3 section 3.9, the area's for what it lacks) */
    unsigned dims = 7;
    float ratio = 0.0f;
    if (grad) dims = 0;
    else image_cache_intrinsic(g_image_cache, s->background_image_url, &dims, &ratio);
    bool has_r = (dims & 4) && ratio > 0.0f;
    double tw = nw, th = nh;
    if (s->bg_size_kind == 1 || s->bg_size_kind == 2) {
        if (!has_r) { tw = aw; th = ah; }
        else {
            double rw = ratio, rh = 1.0;
            double sx = aw / rw, sy = ah / rh;
            double k = s->bg_size_kind == 1 ? (sx > sy ? sx : sy) : (sx < sy ? sx : sy);
            tw = rw * k;
            th = rh * k;
        }
    } else if (s->bg_size_kind == 3) {
        double w = s->bg_size_w, hgt = s->bg_size_h;
        if (w >= 0.0 && (s->bg_size_pct & 1)) w *= aw;
        if (hgt >= 0.0 && (s->bg_size_pct & 2)) hgt *= ah;
        if (w >= 0.0 && hgt >= 0.0) { tw = w; th = hgt; }
        else if (w >= 0.0) { tw = w; th = has_r ? w / ratio : (dims & 2) ? nh : ah; }
        else if (hgt >= 0.0) { th = hgt; tw = has_r ? hgt * ratio : (dims & 1) ? nw : aw; }
    } else if ((dims & 3) != 3) {
        /* (auto: a size of one's own where it has one, the ratio for the
         * other, else the area's; neither and a ratio: contain) */
        if (dims & 1) { tw = nw; th = has_r ? nw / ratio : ah; }
        else if (dims & 2) { th = nh; tw = has_r ? nh * ratio : aw; }
        else if (has_r) {
            double k = aw / ratio < ah ? aw / ratio : ah;
            th = k;
            tw = k * ratio;
        } else { tw = aw; th = ah; }
    }
    if (tw <= 0.0 || th <= 0.0) return;
    /* (round: as many whole tiles as fit best, sized to fill the area; one
     * axis rounded and the other auto keeps the ratio) */
    if (s->bg_round) {
        double otw = tw, oth = th;
        if (s->bg_round & 1) { double n = floor(aw / tw + 0.5); if (n < 1.0) n = 1.0; tw = aw / n; }
        if (s->bg_round & 2) { double n = floor(ah / th + 0.5); if (n < 1.0) n = 1.0; th = ah / n; }
        bool hauto = s->bg_size_kind == 0 || (s->bg_size_kind == 3 && s->bg_size_h < 0.0);
        bool wauto = s->bg_size_kind == 0 || (s->bg_size_kind == 3 && s->bg_size_w < 0.0);
        if ((s->bg_round & 3) == 1 && hauto && otw > 0.0) th = oth * tw / otw;
        if ((s->bg_round & 3) == 2 && wauto && oth > 0.0) tw = otw * th / oth;
    }
    char gkey[96];
    const char *src = s->background_image_url;
    char gsub[2048];
    bool has_cc = false;
    for (const char *q = grad; q && *q && !has_cc; q++) has_cc = !strncasecmp(q, "currentcolor", 12);
    if (grad && has_cc) {
        /* (currentcolor: the box's color, written in) */
        char col[64];
        snprintf(col, sizeof col, "rgba(%d, %d, %d, %.4f)", (int)lround(s->color_r * 255.0), (int)lround(s->color_g * 255.0),
                 (int)lround(s->color_b * 255.0), s->color_a);
        size_t o = 0;
        for (const char *q = grad; *q && o + 64 < sizeof gsub;) {
            if (!strncasecmp(q, "currentcolor", 12)) { o += (size_t)snprintf(gsub + o, sizeof gsub - o, "%s", col); q += 12; }
            else gsub[o++] = *q++;
        }
        gsub[o] = 0;
        grad = gsub;
    }
    if (grad) {
        int gw = (int)lround(tw), gh = (int)lround(th);
        if (gw < 1) gw = 1;
        if (gh < 1) gh = 1;
        if (!grad_image_key(grad, gw, gh, gkey, sizeof gkey)) return;
        src = gkey;
    }
    int handle = image_cache_request(g_image_cache, src, (int)ceil(tw), (int)ceil(th));
    if (handle < 0) return;
    double ox = ax + s->bg_pos_xf * (aw - tw) + s->bg_pos_xpx;
    double oy = ay + s->bg_pos_yf * (ah - th) + s->bg_pos_ypx;
    /* clip: the border box (and the viewport for a fixed one) */
    float c[4] = { (float)bx, (float)by, (float)(bx + bw), (float)(by + bh) };
    if (area) {
        memcpy(c, area, sizeof(c));
        bx = c[0]; by = c[1]; bw = c[2] - c[0]; bh = c[3] - c[1];
    }
    double x0 = ox, x1 = ox + tw, y0 = oy, y1 = oy + th;
    double stx = tw, sty = th;   /* (the step between tiles) */
    bool rx = !s->bg_no_repeat_x, ry = !s->bg_no_repeat_y;
    if (rx && (s->bg_space & 1)) {
        /* (space: the whole tiles that fit, first and last at the area's edges,
         * the room between them; one: placed as no-repeat; none: none) */
        double n = floor(aw / tw + 1e-6);
        if (n >= 2.0) { stx = tw + (aw - n * tw) / (n - 1.0); ox = ax; }
        else { rx = false; if (n < 1.0) return; }
    }
    if (ry && (s->bg_space & 2)) {
        double n = floor(ah / th + 1e-6);
        if (n >= 2.0) { sty = th + (ah - n * th) / (n - 1.0); oy = ay; }
        else { ry = false; if (n < 1.0) return; }
    }
    x0 = ox; x1 = ox + tw; y0 = oy; y1 = oy + th;
    if (rx) {
        x0 = ox - ceil((ox - bx) / stx) * stx;
        x1 = bx + bw;
    }
    if (ry) {
        y0 = oy - ceil((oy - by) / sty) * sty;
        y1 = by + bh;
    }
    double ntiles = ceil((x1 - x0) / stx) * ceil((y1 - y0) / sty);
    /* (a tiny image over a huge box: tiles too many to draw; repeated both
     * ways, smaller than a pixel, the image stretched over it, as it averages out) */
    bool smear = ntiles > 20000.0 && rx && ry && tw < 1.0 && th < 1.0;
    if (ntiles > 20000.0 && !smear) return;
    int from = dl->count;
    if (smear) emit_image(dl, (float)x0, (float)y0, (float)(x1 - x0), (float)(y1 - y0), handle);
    for (double y = y0; !smear && y < y1 - 0.001; y += sty)
        for (double x = x0; x < x1 - 0.001; x += stx)
            if (x + tw > c[0] && x < c[2] && y + th > c[1] && y < c[3])
                emit_image(dl, (float)x, (float)y, (float)tw, (float)th, handle);
    dl_clip_range(dl, from, c);
}

/* A box's border-image (CSS Backgrounds 3 section 6): the image cut in
 * nine by its slices, the corners into the corners of the border image
 * area (the border box grown by the outsets), the edges along them
 * stretched, repeated, rounded or spaced, the middle when fill; false
 * when it has none to paint (the border as usual then). */
static void dl_clip_range(DisplayList *dl, int from, const float c[4]);
static void bi_piece(DisplayList *dl, int handle, int nw, int nh, double sx0, double sy0, double sx1, double sy1,
                     double dx, double dy, double dw, double dh, int repx, int repy, double tw, double th, float op)
{
    if (dw <= 0.0 || dh <= 0.0 || sx1 <= sx0 || sy1 <= sy0) return;
    if (repx == 0 || tw <= 0.0) tw = dw;
    if (repy == 0 || th <= 0.0) th = dh;
    double gx = 0.0, gy = 0.0, x0 = dx, y0 = dy;
    if (repx == 2) { double n = floor(dw / tw + 0.5); if (n < 1.0) n = 1.0; tw = dw / n; }
    if (repy == 2) { double n = floor(dh / th + 0.5); if (n < 1.0) n = 1.0; th = dh / n; }
    if (repx == 1) x0 = dx + dw / 2.0 - tw / 2.0 - ceil((dw / 2.0 - tw / 2.0) / tw) * tw;
    if (repy == 1) y0 = dy + dh / 2.0 - th / 2.0 - ceil((dh / 2.0 - th / 2.0) / th) * th;
    if (repx == 3) { double n = floor(dw / tw + 1e-6); if (n < 1.0) return; gx = (dw - n * tw) / (n + 1.0); x0 = dx + gx; }
    if (repy == 3) { double n = floor(dh / th + 1e-6); if (n < 1.0) return; gy = (dh - n * th) / (n + 1.0); y0 = dy + gy; }
    double nt = ceil((dw - (x0 - dx)) / (tw + gx) + 1.0) * ceil((dh - (y0 - dy)) / (th + gy) + 1.0);
    if (nt > 4000.0) return;
    int from = dl->count;
    float u0 = (float)(sx0 / nw), v0 = (float)(sy0 / nh), u1 = (float)(sx1 / nw), v1 = (float)(sy1 / nh);
    for (double y = y0; y < dy + dh - 0.001; y += th + gy)
        for (double x = x0; x < dx + dw - 0.001; x += tw + gx) {
            display_list_add_image(dl, (float)x, (float)y, (float)tw, (float)th, handle, u0, v0, u1, v1);
            if (op < 0.999f) dl->cmds[dl->count - 1].a = op;
        }
    float c[4] = { (float)dx, (float)dy, (float)(dx + dw), (float)(dy + dh) };
    dl_clip_range(dl, from, c);
}

static bool emit_border_image(DisplayList *dl, const LayoutBox *box, float op)
{
    const LayoutStyle *s = box->s;
    if ((!s->bi_src || !s->bi_src[0]) && !s->bi_grad) return false;
    t_grad_em = s->font_size > 0.0 ? s->font_size : 16.0;
    if (!g_image_cache) return false;
    const double bwd[4] = { s->border_top, s->border_right, s->border_bottom, s->border_left };
    double out[4], W[4], S[4];
    for (int k = 0; k < 4; k++) out[k] = (s->bi_out_num >> k & 1) ? s->bi_out[k] * bwd[k] : s->bi_out[k];
    double ax = box->x - out[3], ay = box->y - out[0];
    double aw = box->width + out[1] + out[3], ah = box->height + out[0] + out[2];
    int nw = 0, nh = 0, handle = -1;
    if (s->bi_grad) {
        /* (a gradient: an image the border image area's size) */
        char key[96];
        nw = (int)lround(aw); nh = (int)lround(ah);
        if (nw < 1 || nh < 1 || !grad_image_key(s->bi_grad, nw, nh, key, sizeof key)) return true;
        handle = image_cache_request(g_image_cache, key, nw, nh);
    } else {
        if (!image_cache_natural_size(g_image_cache, s->bi_src, &nw, &nh) || nw <= 0 || nh <= 0) {
            image_cache_request(g_image_cache, s->bi_src, 0, 0);
            return true;   /* (loading: no border meanwhile) */
        }
        handle = image_cache_request(g_image_cache, s->bi_src, nw, nh);
    }
    if (handle < 0) return true;
    for (int k = 0; k < 4; k++) {
        double dim = (k & 1) ? nw : nh;
        S[k] = (s->bi_slice_pct >> k & 1) ? s->bi_slice[k] / 100.0 * dim : s->bi_slice[k];
        if (S[k] > dim) S[k] = dim;
        if (S[k] < 0.0) S[k] = 0.0;
    }
    for (int k = 0; k < 4; k++) {
        double adim = (k & 1) ? aw : ah;
        switch (s->bi_w_kind[k]) {
        case 1: W[k] = s->bi_w[k]; break;
        case 2: W[k] = s->bi_w[k] / 100.0 * adim; break;
        case 3: W[k] = S[k]; break;
        default: W[k] = s->bi_w[k] * bwd[k]; break;
        }
        if (W[k] < 0.0) W[k] = 0.0;
    }
    /* (widths that overlap: all scaled down alike) */
    double f = 1.0;
    if (W[1] + W[3] > aw && W[1] + W[3] > 0.0) f = fmin(f, aw / (W[1] + W[3]));
    if (W[0] + W[2] > ah && W[0] + W[2] > 0.0) f = fmin(f, ah / (W[0] + W[2]));
    for (int k = 0; k < 4; k++) W[k] *= f;
    double mx0 = ax + W[3], mx1 = ax + aw - W[1], my0 = ay + W[0], my1 = ay + ah - W[2];
    double sx0 = S[3], sx1 = nw - S[1], sy0 = S[0], sy1 = nh - S[2];
    int rx = s->bi_rep[0], ry = s->bi_rep[1];
    /* corners */
    bi_piece(dl, handle, nw, nh, 0, 0, S[3], S[0], ax, ay, W[3], W[0], 0, 0, 0, 0, op);
    bi_piece(dl, handle, nw, nh, sx1, 0, nw, S[0], mx1, ay, W[1], W[0], 0, 0, 0, 0, op);
    bi_piece(dl, handle, nw, nh, sx1, sy1, nw, nh, mx1, my1, W[1], W[2], 0, 0, 0, 0, op);
    bi_piece(dl, handle, nw, nh, 0, sy1, S[3], nh, ax, my1, W[3], W[2], 0, 0, 0, 0, op);
    /* edges: tiles keep the slice's ratio at the edge's width */
    double tmid = sx1 - sx0, lmid = sy1 - sy0;
    if (S[0] > 0.0) bi_piece(dl, handle, nw, nh, sx0, 0, sx1, S[0], mx0, ay, mx1 - mx0, W[0], rx, 0, tmid * W[0] / S[0], 0, op);
    if (S[2] > 0.0) bi_piece(dl, handle, nw, nh, sx0, sy1, sx1, nh, mx0, my1, mx1 - mx0, W[2], rx, 0, tmid * W[2] / S[2], 0, op);
    if (S[3] > 0.0) bi_piece(dl, handle, nw, nh, 0, sy0, S[3], sy1, ax, my0, W[3], my1 - my0, 0, ry, 0, lmid * W[3] / S[3], op);
    if (S[1] > 0.0) bi_piece(dl, handle, nw, nh, sx1, sy0, nw, sy1, mx1, my0, W[1], my1 - my0, 0, ry, 0, lmid * W[1] / S[1], op);
    if (s->bi_fill) {
        double kx = S[0] > 0.0 ? W[0] / S[0] : (S[2] > 0.0 ? W[2] / S[2] : 1.0);
        double ky = S[3] > 0.0 ? W[3] / S[3] : (S[1] > 0.0 ? W[1] / S[1] : 1.0);
        bi_piece(dl, handle, nw, nh, sx0, sy0, sx1, sy1, mx0, my0, mx1 - mx0, my1 - my0, rx, ry, tmid * kx, lmid * ky, op);
    }
    return true;
}

/* ---- CSS gradients, rasterized (linear, radial, conic, repeating) ---- */
typedef struct { double pos; double c[4]; char txt[96]; } GradStop;   /* (c: premultiplied 0..1; txt the color as written) */

/* (calc() in a stop position: + - * / over numbers, px, em and percents of len) */
static double grad_calc_expr(const char **p, double len);
static double grad_calc_factor(const char **p, double len)
{
    while (isspace((unsigned char)**p)) (*p)++;
    if (!strncasecmp(*p, "calc(", 5)) *p += 4;
    if (**p == '(') {
        (*p)++;
        double v = grad_calc_expr(p, len);
        while (isspace((unsigned char)**p)) (*p)++;
        if (**p == ')') (*p)++;
        return v;
    }
    char *e;
    double v = strtod(*p, &e);
    if (e == *p) { (*p)++; return 0.0; }
    if (*e == '%') { v = v / 100.0 * len; e++; }
    else if (!strncasecmp(e, "px", 2)) e += 2;
    else if (!strncasecmp(e, "em", 2)) { v *= t_grad_em; e += 2; }
    *p = e;
    return v;
}
static double grad_calc_term(const char **p, double len)
{
    double v = grad_calc_factor(p, len);
    for (;;) {
        while (isspace((unsigned char)**p)) (*p)++;
        if (**p == '*') { (*p)++; v *= grad_calc_factor(p, len); }
        else if (**p == '/') { (*p)++; double d = grad_calc_factor(p, len); v = d != 0.0 ? v / d : v; }
        else return v;
    }
}
static double grad_calc_expr(const char **p, double len)
{
    double v = grad_calc_term(p, len);
    for (;;) {
        while (isspace((unsigned char)**p)) (*p)++;
        if (**p == '+') { (*p)++; v += grad_calc_term(p, len); }
        else if (**p == '-') { (*p)++; v -= grad_calc_term(p, len); }
        else return v;
    }
}

/* (a stop list from the arguments after the first, positions in px of a
 * line of length len; deg for conic, of 360 then) */
static int grad_parse_stops(const char *s, double len, bool conic, GradStop *st, int cap)
{
    int n = 0;
    const char *p = s;
    while (*p && n < cap) {
        while (isspace((unsigned char)*p) || *p == ',') p++;
        if (!*p || *p == ')') break;
        const char *a = p;
        int dp = 0;
        while (*p && (dp > 0 || (*p != ',' && *p != ')'))) { if (*p == '(') dp++; else if (*p == ')') dp--; p++; }
        char item[256];
        size_t l = (size_t)(p - a) < sizeof item - 1 ? (size_t)(p - a) : sizeof item - 1;
        memcpy(item, a, l);
        while (l && isspace((unsigned char)item[l - 1])) l--;
        item[l] = 0;
        /* (words: the color, then up to two positions) */
        char words[4][128];
        int nw = 0;
        for (const char *q = item; *q && nw < 4;) {
            while (isspace((unsigned char)*q)) q++;
            if (!*q) break;
            const char *w0 = q;
            int d2 = 0;
            while (*q && (d2 > 0 || !isspace((unsigned char)*q))) { if (*q == '(') d2++; else if (*q == ')') d2--; q++; }
            size_t wl = (size_t)(q - w0) < 127 ? (size_t)(q - w0) : 127;
            memcpy(words[nw], w0, wl);
            words[nw][wl] = 0;
            nw++;
        }
        if (!nw) continue;
        double rgba[4];
        int ci = -1;
        for (int k = 0; k < nw; k++) if (css_color_parse(words[k], rgba)) { ci = k; break; }
        if (ci < 0) continue;   /* (a hint alone: not read) */
        double pp[2];
        int npos = 0;
        for (int k = 0; k < nw && npos < 2; k++) {
            if (k == ci) continue;
            if (!strncasecmp(words[k], "calc(", 5)) {
                const char *q = words[k];
                pp[npos++] = grad_calc_expr(&q, len);
                continue;
            }
            char *e;
            double v = strtod(words[k], &e);
            if (e == words[k]) continue;
            if (*e == '%') v = v / 100.0 * len;
            else if (!strncasecmp(e, "turn", 4)) v *= 360.0;
            else if (!strncasecmp(e, "rad", 3)) v *= 180.0 / 3.14159265358979;
            else if (!strncasecmp(e, "em", 2)) v *= t_grad_em;
            pp[npos++] = v;
        }
        double a8 = rgba[3];
        double c[4] = { rgba[0] / 255.0 * a8, rgba[1] / 255.0 * a8, rgba[2] / 255.0 * a8, a8 };
        if (npos == 0) { st[n].pos = NAN; memcpy(st[n].c, c, sizeof c); snprintf(st[n].txt, sizeof st[n].txt, "%s", words[ci]); n++; }
        for (int k = 0; k < npos && n < cap; k++) { st[n].pos = pp[k]; memcpy(st[n].c, c, sizeof c); snprintf(st[n].txt, sizeof st[n].txt, "%s", words[ci]); n++; }
    }
    (void)conic;
    if (!n) return 0;
    if (isnan(st[0].pos)) st[0].pos = 0.0;
    if (isnan(st[n - 1].pos)) st[n - 1].pos = len;
    for (int i = 1; i < n; i++) if (!isnan(st[i].pos) && st[i].pos < st[i - 1].pos) st[i].pos = st[i - 1].pos;
    for (int i = 1; i < n - 1; i++) {
        if (!isnan(st[i].pos)) continue;
        int j = i;
        while (j < n && isnan(st[j].pos)) j++;
        for (int k = i; k < j; k++) st[k].pos = st[i - 1].pos + (st[j].pos - st[i - 1].pos) * (k - i + 1) / (j - i + 1);
    }
    return n;
}

static void grad_color_at(const GradStop *st, int n, double t, bool rep, double out[4])
{
    if (rep && n > 1 && st[n - 1].pos > st[0].pos) {
        double span = st[n - 1].pos - st[0].pos;
        t = st[0].pos + fmod(t - st[0].pos, span);
        if (t < st[0].pos) t += span;
    }
    if (t <= st[0].pos) { memcpy(out, st[0].c, 4 * sizeof(double)); return; }
    for (int i = 1; i < n; i++)
        if (t < st[i].pos) {
            double f = st[i].pos > st[i - 1].pos ? (t - st[i - 1].pos) / (st[i].pos - st[i - 1].pos) : 1.0;
            for (int k = 0; k < 4; k++) out[k] = st[i - 1].c[k] + (st[i].c[k] - st[i - 1].c[k]) * f;
            return;
        }
    memcpy(out, st[n - 1].c, 4 * sizeof(double));
}

/* (a position's offset on an axis of size sz: keywords, percents, px) */
static double grad_pos_val(const char *w, double sz, bool *ok)
{
    *ok = true;
    if (!strcasecmp(w, "left") || !strcasecmp(w, "top")) return 0.0;
    if (!strcasecmp(w, "right") || !strcasecmp(w, "bottom")) return sz;
    if (!strcasecmp(w, "center")) return sz / 2.0;
    char *e;
    double v = strtod(w, &e);
    if (e == w) { *ok = false; return sz / 2.0; }
    return *e == '%' ? v / 100.0 * sz : v;
}

/* The gradient g rasterized at w x h, RGBA (straight); NULL when not read. */
/* (h holding n, case aside) */
static const char *strcasestr_dl(const char *h, const char *n)
{
    size_t l = strlen(n);
    for (; *h; h++) if (!strncasecmp(h, n, l)) return h;
    return NULL;
}

static uint8_t *grad_raster(const char *g, int w, int h)
{
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return NULL;
    bool rep = !strncasecmp(g, "repeating-", 10);
    int kind = strstr(g, "radial-gradient(") ? 1 : strstr(g, "conic-gradient(") ? 2 : 0;
    const char *args = strchr(g, '(');
    if (!args) return NULL;
    args++;
    /* (the first argument: the direction / shape / angle, when it is one) */
    const char *p = args;
    int dp = 0;
    while (*p && (dp > 0 || (*p != ',' && *p != ')'))) { if (*p == '(') dp++; else if (*p == ')') dp--; p++; }
    char first[256];
    size_t fl = (size_t)(p - args) < sizeof first - 1 ? (size_t)(p - args) : sizeof first - 1;
    memcpy(first, args, fl);
    first[fl] = 0;
    double rgba_probe[4];
    char fw[8][64];
    int nfw = 0;
    for (char *q = first; *q && nfw < 8;) {
        while (isspace((unsigned char)*q)) q++;
        if (!*q) break;
        char *w0 = q;
        int d2 = 0;
        while (*q && (d2 > 0 || !isspace((unsigned char)*q))) { if (*q == '(') d2++; else if (*q == ')') d2--; q++; }
        size_t wl = (size_t)(q - w0) < 63 ? (size_t)(q - w0) : 63;
        memcpy(fw[nfw], w0, wl);
        fw[nfw][wl] = 0;
        nfw++;
    }
    bool has_dir = nfw > 0 && !css_color_parse(fw[0], rgba_probe);
    if (nfw > 0 && has_dir) {
        /* (a color with a position first, as "red 10%": still a stop) */
        char *e;
        strtod(fw[0], &e);
        if (e != fw[0] && kind != 2 && kind != 1 && !strstr(fw[0], "deg") && !strstr(fw[0], "turn") && !strstr(fw[0], "rad")) has_dir = false;
    }
    const char *stops = has_dir ? (*p == ',' ? p + 1 : p) : args;
    double cx = w / 2.0, cy = h / 2.0;
    /* linear */
    double dx = 0.0, dy = 1.0, len = h;
    /* radial */
    double rx = 0.0, ry = 0.0;
    bool circle = false;
    /* conic */
    double from = 0.0;
    if (kind == 0) {
        double ang = 180.0;
        if (has_dir) {
            if (!strcasecmp(fw[0], "to")) {
                int sx = 0, sy = 0;
                for (int k = 1; k < nfw; k++) {
                    if (!strcasecmp(fw[k], "left")) sx = -1; else if (!strcasecmp(fw[k], "right")) sx = 1;
                    else if (!strcasecmp(fw[k], "top")) sy = -1; else if (!strcasecmp(fw[k], "bottom")) sy = 1;
                }
                if (sx && sy) {
                    double vx = sx * (double)h, vy = sy * (double)w;
                    ang = atan2(vx, -vy) * 180.0 / 3.14159265358979;
                } else ang = sx > 0 ? 90.0 : sx < 0 ? 270.0 : sy < 0 ? 0.0 : 180.0;
            } else {
                char *e;
                double v = strtod(fw[0], &e);
                if (!strncasecmp(e, "turn", 4)) v *= 360.0;
                else if (!strncasecmp(e, "grad", 4)) v *= 0.9;
                else if (!strncasecmp(e, "rad", 3)) v *= 180.0 / 3.14159265358979;
                ang = v;
            }
        }
        double th = ang * 3.14159265358979 / 180.0;
        dx = sin(th); dy = -cos(th);
        len = fabs(w * dx) + fabs(h * dy);
    } else {
        /* (radial: shape, size, at position; conic: from angle, at position) */
        int at = -1;
        for (int k = 0; k < nfw; k++) if (!strcasecmp(fw[k], "at")) at = k;
        if (has_dir && at >= 0 && at + 1 < nfw) {
            bool ok;
            const char *a1 = fw[at + 1], *a2 = at + 2 < nfw ? fw[at + 2] : "center";
            bool swap = !strcasecmp(a1, "top") || !strcasecmp(a1, "bottom") || ((!strcasecmp(a2, "left") || !strcasecmp(a2, "right")));
            if (swap) { const char *t = a1; a1 = a2; a2 = t; }
            cx = grad_pos_val(a1, w, &ok);
            cy = grad_pos_val(a2, h, &ok);
        }
        int lim = at >= 0 ? at : nfw;
        if (kind == 2) {
            for (int k = 0; k + 1 < lim; k++)
                if (!strcasecmp(fw[k], "from")) {
                    char *e;
                    double v = strtod(fw[k + 1], &e);
                    if (!strncasecmp(e, "turn", 4)) v *= 360.0;
                    else if (!strncasecmp(e, "rad", 3)) v *= 180.0 / 3.14159265358979;
                    from = v;
                }
            len = 360.0;
        } else {
            int sizek = 3;   /* (farthest-corner) */
            double l1 = -1.0, l2 = -1.0;
            bool ell_said = false;
            for (int k = 0; k < (has_dir ? lim : 0); k++) {
                if (!strcasecmp(fw[k], "circle")) circle = true;
                else if (!strcasecmp(fw[k], "ellipse")) ell_said = true;
                else if (!strcasecmp(fw[k], "closest-side")) sizek = 0;
                else if (!strcasecmp(fw[k], "farthest-side")) sizek = 2;
                else if (!strcasecmp(fw[k], "closest-corner")) sizek = 1;
                else if (!strcasecmp(fw[k], "farthest-corner")) sizek = 3;
                else {
                    char *e;
                    double v = strtod(fw[k], &e);
                    if (e != fw[k]) {
                        double base = l1 < 0.0 ? w : h;
                        if (*e == '%') v = v / 100.0 * base;
                        if (l1 < 0.0) l1 = v; else l2 = v;
                        sizek = -1;
                    }
                }
            }
            if (sizek == -1 && l2 < 0.0 && !ell_said) circle = true;
            double sl = fmin(fabs(cx), fabs(w - cx)), sr = fmax(fabs(cx), fabs(w - cx));
            double st2 = fmin(fabs(cy), fabs(h - cy)), sb = fmax(fabs(cy), fabs(h - cy));
            if (sizek == -1) { rx = l1; ry = l2 >= 0.0 ? l2 : l1; }
            else if (circle) {
                if (sizek == 0) rx = fmin(sl, st2);
                else if (sizek == 2) rx = fmax(sr, sb);
                else if (sizek == 1) rx = sqrt(sl * sl + st2 * st2);
                else rx = sqrt(sr * sr + sb * sb);
                ry = rx;
            } else {
                double ax = sizek <= 1 ? sl : sr, ay = sizek <= 1 ? st2 : sb;
                if (sizek == 0 || sizek == 2) { rx = ax; ry = ay; }
                else { rx = ax * 1.41421356; ry = ay * 1.41421356; }
            }
            if (rx <= 0.0) rx = 1e-6;
            if (ry <= 0.0) ry = 1e-6;
            len = rx;
        }
    }
    GradStop st[64];
    int n = grad_parse_stops(stops, len, kind == 2, st, 64);
    if (!n) return NULL;
    /* (an interpolation method, "in <space> [<hue> hue]": each segment's
     * colors by color-mix, 64 steps) */
    char method[96] = "";
    for (int k = 0; k + 1 < nfw && has_dir; k++)
        if (!strcasecmp(fw[k], "in")) {
            size_t ml = 0;
            ml += (size_t)snprintf(method + ml, sizeof method - ml, "in %s", fw[k + 1]);
            if (k + 3 < nfw && !strcasecmp(fw[k + 3], "hue")) snprintf(method + ml, sizeof method - ml, " %s hue", fw[k + 2]);
            break;
        }
    /* (no method: oklab when a stop is in a color syntax past the legacy ones,
     * CSS Images 4; missing components, none, through color-mix even in sRGB) */
    bool modern = false, has_none = false;
    for (int i = 0; i < n; i++) {
        const char *tx = st[i].txt;
        while (*tx == ' ') tx++;
        if (!strncasecmp(tx, "color(", 6) || !strncasecmp(tx, "lab(", 4) || !strncasecmp(tx, "lch(", 4) ||
            !strncasecmp(tx, "oklab(", 6) || !strncasecmp(tx, "oklch(", 6)) modern = true;
        if (strcasestr_dl(tx, "none")) has_none = true;
    }
    if (!method[0] && modern) snprintf(method, sizeof method, "in oklab");
    enum { LUTN = 256 };
    static __thread double lut[63][LUTN + 1][4];
    bool use_lut = method[0] && n <= 64 && (strcasecmp(method, "in srgb") || has_none);   /* (srgb-linear too) */
    if (use_lut)
        for (int i = 1; i < n && i <= 63; i++)
            for (int k = 0; k <= LUTN; k++) {
                char expr[320];
                double f = (double)k / LUTN, r4[4];
                snprintf(expr, sizeof expr, "color-mix(%s, %s %.4f%%, %s)", method, st[i - 1].txt, (1.0 - f) * 100.0, st[i].txt);
                if (css_color_parse(expr, r4)) {
                    lut[i - 1][k][0] = r4[0] / 255.0 * r4[3]; lut[i - 1][k][1] = r4[1] / 255.0 * r4[3];
                    lut[i - 1][k][2] = r4[2] / 255.0 * r4[3]; lut[i - 1][k][3] = r4[3];
                } else {
                    for (int q = 0; q < 4; q++) lut[i - 1][k][q] = st[i - 1].c[q] + (st[i].c[q] - st[i - 1].c[q]) * f;
                }
            }
    uint8_t *px = (uint8_t *)malloc((size_t)w * h * 4);
    if (!px) return NULL;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            double qx = x + 0.5 - cx, qy = y + 0.5 - cy, t;
            if (kind == 0) t = qx * dx + qy * dy + len / 2.0;
            else if (kind == 1) t = sqrt((qx / rx) * (qx / rx) + (qy / ry) * (qy / ry)) * rx;
            else { t = atan2(qx, -qy) * 180.0 / 3.14159265358979 - from; t = fmod(t, 360.0); if (t < 0.0) t += 360.0; }
            double c[4];
            grad_color_at(st, n, t, rep, c);
            if (use_lut) {
                double tt = t;
                if (rep && n > 1 && st[n - 1].pos > st[0].pos) {
                    double span = st[n - 1].pos - st[0].pos;
                    tt = st[0].pos + fmod(tt - st[0].pos, span);
                    if (tt < st[0].pos) tt += span;
                }
                for (int i = 1; i < n && i <= 63; i++)
                    if (tt < st[i].pos && tt >= st[i - 1].pos) {
                        double f = st[i].pos > st[i - 1].pos ? (tt - st[i - 1].pos) / (st[i].pos - st[i - 1].pos) : 1.0;
                        /* (between the two nearest entries, linearly) */
                        double fk = f * LUTN;
                        int k = (int)floor(fk);
                        if (k < 0) k = 0;
                        if (k >= LUTN) k = LUTN - 1;
                        double u = fk - k;
                        if (u < 0.0) u = 0.0;
                        if (u > 1.0) u = 1.0;
                        for (int q = 0; q < 4; q++) c[q] = lut[i - 1][k][q] * (1.0 - u) + lut[i - 1][k + 1][q] * u;
                        break;
                    }
            }
            uint8_t *o = px + ((size_t)y * w + x) * 4;
            double a = c[3];
            o[0] = (uint8_t)lround(a > 0.0 ? fmin(255.0, c[0] / a * 255.0) : 0.0);
            o[1] = (uint8_t)lround(a > 0.0 ? fmin(255.0, c[1] / a * 255.0) : 0.0);
            o[2] = (uint8_t)lround(a > 0.0 ? fmin(255.0, c[2] / a * 255.0) : 0.0);
            o[3] = (uint8_t)lround(fmin(1.0, a) * 255.0);
        }
    return px;
}

/* (gradient g as an image of w x h in the image cache: its key) */
static bool grad_image_key(const char *g, int w, int h, char *key, size_t kn)
{
    uint64_t hs = 1469598103934665603ull;
    for (const char *q = g; *q; q++) { hs ^= (unsigned char)*q; hs *= 1099511628211ull; }
    snprintf(key, kn, "cyber-grad:%016llx:%dx%d:%g", (unsigned long long)hs, w, h, t_grad_em);
    if (image_cache_status(g_image_cache, key) == 1) return true;
    uint8_t *px = grad_raster(g, w, h);
    if (!px) return false;
    image_cache_put_rgba(g_image_cache, key, w, h, px);
    free(px);
    return true;
}

static void on_async_image_loaded(const char *url, void *user_data);

/* ---- reference filters ---- */
/* (a filter run on the CPU: with a url(), or a blur on a box that isn't huge, no transforms about) */
/* (CYBER_CPU_LAYERS=0: blur and masks on the GPU path only, to compare) */
static bool dl_cpu_layers_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("CYBER_CPU_LAYERS"); on = !(e && e[0] == '0'); }
    return on != 0;
}
static bool dl_box_filter_cpu(const LayoutBox *box, bool any_tf)
{
    if (!dl_cpu_layers_on() && !box->s->filt_url) return false;
    if (box->s->filt_url) return box->s->filt_text != NULL;
    if (any_tf || !box->s->filt_n || !box->s->filt_text || box->width * box->height > 1000000.0) return false;
    for (int k = 0; k < box->s->filt_n && k < 8; k++)
        if (box->s->filt_t[k] == 9 || (box->s->filt_t[k] == 10 && box->s->filt_ds[2] > 0.0f)) return true;
    return false;
}
/* (what clips a CPU filtered box's output, per box; its subtree painted free of that) */
static __thread float (*t_flt_clip)[4];
typedef struct { LayoutContext *ctx; const char *want; char *buf; size_t bufn; bool got; } DlFltCasc;
static const char *node_attribute_value(HtmlNode *node, const char *name);
static void dl_flt_casc(void *ud, const char *prop, const char *value, bool important)
{
    DlFltCasc *c = (DlFltCasc *)ud;
    (void)important;
    if (!strcasecmp(prop, c->want)) { snprintf(c->buf, c->bufn, "%s", value); c->got = true; }
}
static const char *dl_flt_prop(void *ud, HtmlNode *n, const char *name, char *buf, size_t bufn)
{
    LayoutContext *ctx = (LayoutContext *)ud;
    const char *v = node_attribute_value(n, name);
    /* (a presentation attribute may go by style: its declarations, then the sheets') */
    if (!strcmp(name, "flood-color") || !strcmp(name, "flood-opacity") || !strcmp(name, "lighting-color") || !strcmp(name, "color-interpolation-filters")) {
        const char *st = node_attribute_value(n, "style");
        size_t nl = strlen(name);
        for (const char *q = st; q && (q = strcasestr_dl(q, name)) != NULL; q += nl) {
            const char *r = q + nl;
            while (*r == ' ') r++;
            if (*r != ':') continue;
            r++;
            while (*r == ' ') r++;
            size_t l = strcspn(r, ";");
            while (l > 0 && r[l - 1] == ' ') l--;
            if (l >= bufn) l = bufn - 1;
            memcpy(buf, r, l);
            buf[l] = 0;
            return buf;
        }
        if (ctx && ctx->doc) {
            DlFltCasc c = { ctx, name, buf, bufn, false };
            int di = po_array_index_from_payload(&ctx->doc->array, n);
            if (di >= 0) css_layout_dom_cascade(ctx, di, -1, dl_flt_casc, &c);
            if (c.got) return buf;
        }
    }
    return v;
}
static bool dl_flt_color(void *ud, const char *text, double *rgba) { (void)ud; return css_layout_parse_color_text(text, rgba); }
static bool dl_flt_image(void *ud, const char *href, const uint8_t **px, int *w, int *h, int *ch)
{
    (void)ud;
    if (!g_image_cache || !href || href[0] == '#') return false;
    int hd = image_cache_request(g_image_cache, href, 0, 0);
    return hd >= 0 && image_cache_get(g_image_cache, hd, w, h, ch, (uint8_t **)px);
}
/* (feImage of an element: it serialized into an svg of its own) */
static bool dl_flt_element(void *ud, const char *id, int w, int h, const uint8_t **px, int *ch);
static __thread LayoutContext *t_flt_ctx;
static __thread double t_flt_color[4];
static void dl_svg_filter_hook(const char *value, const float box[4], unsigned char *rgba, int w, int h)
{
    LayoutContext *ctx = t_flt_ctx;
    if (!ctx || !ctx->doc) return;
    SvgFilterEnv env = { ctx->doc, dl_flt_prop, dl_flt_color, dl_flt_image, dl_flt_element, (void *)ctx, { t_flt_color[0], t_flt_color[1], t_flt_color[2], t_flt_color[3] }, true };
    svg_filter_apply(&env, value, box, 16.0f, rgba, w, h);
}
/* The commands since `from` (the box and its subtree) become an image of them filtered. */
static int mc_owner(const LayoutContext *ctx, int i, int *child);
static void dl_ref_filter_end(LayoutContext *ctx, DisplayList *dl, int box_i, int from)
{
    const LayoutBox *box = &ctx->boxes[box_i];
    int W = (int)ctx->viewport_width, H = (int)ctx->viewport_height, ncmd = dl->count - from;
    if (W < 1 || H < 1 || W > 8192 || H > 8192 || from < 0 || ncmd < 0 || !g_image_cache) return;
    /* a key: the commands, the list, the filter elements' attributes, the box */
    uint64_t hs = 1469598103934665603ull;
#define DLF_MIX(ptr, len) do { const unsigned char *hp = (const unsigned char *)(ptr); for (size_t hi = 0; hi < (size_t)(len); hi++) { hs ^= hp[hi]; hs *= 1099511628211ull; } } while (0)
    if (ncmd) DLF_MIX(dl->cmds + from, (size_t)ncmd * sizeof(DisplayListCmd));
    DLF_MIX(box->s->filt_text, strlen(box->s->filt_text));
    {
        float bx[4] = { (float)box->x, (float)box->y, (float)box->width, (float)box->height };
        DLF_MIX(bx, sizeof bx);
        int stack[64], sp = 0;
        const char *p = box->s->filt_text;
        while (ctx->doc && (p = strstr(p, "url(")) != NULL) {
            p += 4;
            const char *h = strchr(p, '#');
            const char *e = strchr(p, ')');
            if (!h || !e || h > e) continue;
            char id[128];
            size_t l = (size_t)(e - h - 1);
            if (l >= sizeof id) l = sizeof id - 1;
            memcpy(id, h + 1, l);
            id[l] = 0;
            while (l > 0 && (id[l - 1] == '"' || id[l - 1] == '\'' || id[l - 1] == ' ')) id[--l] = 0;
            HtmlNode *f = html_document_get_element_by_id(ctx->doc, id);
            /* (its descendants' attributes) */
            HtmlNode *walk[64];
            int w = 0;
            if (f) walk[w++] = f;
            while (w > 0) {
                HtmlNode *n = walk[--w];
                for (HtmlAttribute *a = n->attributes; a; a = a->next) { DLF_MIX(a->name, strlen(a->name)); DLF_MIX(a->value, strlen(a->value)); }
                for (HtmlNode *k = html_node_first_child(ctx->doc, n); k; k = html_node_next_sibling(ctx->doc, k))
                    if (k->type == HTML_NODE_ELEMENT && w < 64) walk[w++] = k;
            }
        }
        (void)stack; (void)sp;
    }
#undef DLF_MIX
    /* (with a margin past the viewport: a blur draws in what's beyond it) */
    int M = 64;
    if (!box->s->filt_url || strstr(box->s->filt_text, "blur(")) {
        M = 8;
        for (int k = 0; k < box->s->filt_n && k < 8; k++) if (box->s->filt_t[k] == 9) { int q = (int)ceil(box->s->filt_v[k] * 3.0) + 2; if (q > M) M = q; }
        { int q = (int)ceil(box->s->filt_ds[2] * 3.0 + fabs(box->s->filt_ds[0]) + fabs(box->s->filt_ds[1])) + 2; if (q > M) M = q; }
        if (box->s->filt_url && M < 64) M = 64;
    }
    if (M > 512) M = 512;
    /* the area: all of it for a filter element (its regions go anywhere); else what the
     * box and its subtree paint, as far as the filter reaches */
    int ax = -M, ay = -M, WM = W + 2 * M, HM = H + 2 * M;
    /* (in columns: its strip is cut and moved after this, so what shows is not what is in the viewport now) */
    int px0 = 0, py0 = 0, px1 = W, py1 = H;
    { int mch = -1; if (!box->s->filt_url && mc_owner(ctx, box_i, &mch) >= 0) { px0 = py0 = -4096; px1 = W + 4096; py1 = H + 8192; } }
    if (!box->s->filt_url) {
        double x0 = box->x, y0 = box->y, x1 = box->x + box->width, y1 = box->y + box->height;
        for (int k = from; k < dl->count; k++) {
            const DisplayListCmd *c = &dl->cmds[k];
            if (c->type == DL_XFORM || c->type == DL_MASK_BEGIN || c->type == DL_MASK_END || c->w <= 0.0f || c->h <= 0.0f) continue;
            if (c->x < x0) x0 = c->x;
            if (c->y < y0) y0 = c->y;
            if (c->x + c->w > x1) x1 = c->x + c->w;
            if (c->y + c->h > y1) y1 = c->y + c->h;
        }
        int ix0 = (int)floor(x0) - M, iy0 = (int)floor(y0) - M, ix1 = (int)ceil(x1) + M, iy1 = (int)ceil(y1) + M;
        if (ix0 < px0 - M) ix0 = px0 - M;
        if (iy0 < py0 - M) iy0 = py0 - M;
        if (ix1 > px1 + M) ix1 = px1 + M;
        if (iy1 > py1 + M) iy1 = py1 + M;
        if (ix1 <= ix0 || iy1 <= iy0) { dl->count = from; return; }
        ax = ix0; ay = iy0; WM = ix1 - ix0; HM = iy1 - iy0;
    }
    /* (what of it shows) */
    int vx0 = ax > px0 ? ax : px0, vy0 = ay > py0 ? ay : py0, vx1 = ax + WM < px1 ? ax + WM : px1, vy1 = ay + HM < py1 ? ay + HM : py1;
    if (vx1 <= vx0 || vy1 <= vy0) { dl->count = from; return; }
    int VW = vx1 - vx0, VH = vy1 - vy0;
    char key[64];
    snprintf(key, sizeof key, "cyber-filt:%016llx:%dx%d", (unsigned long long)hs, VW, VH);
    if (image_cache_status(g_image_cache, key) != 1) {
        uint8_t *rgb_w = (uint8_t *)malloc((size_t)WM * HM * 3), *rgb_b = (uint8_t *)malloc((size_t)WM * HM * 3), *rgba = (uint8_t *)malloc((size_t)WM * HM * 4);
        DisplayListCmd *cmds = (DisplayListCmd *)malloc((size_t)(ncmd + 1) * sizeof(DisplayListCmd));
        DisplayMask *mcopy = dl->mask_count > 0 ? (DisplayMask *)malloc((size_t)dl->mask_count * sizeof(DisplayMask)) : NULL;
        if (rgb_w && rgb_b && rgba && cmds && (mcopy || !dl->mask_count)) {
            /* on white and on black: the difference is its alpha */
            memset(&cmds[0], 0, sizeof cmds[0]);
            cmds[0].type = DL_RECT;
            cmds[0].x = 0; cmds[0].y = 0; cmds[0].w = (float)WM; cmds[0].h = (float)HM;
            cmds[0].r = cmds[0].g = cmds[0].b = 0; cmds[0].a = 255;
            if (ncmd) memcpy(cmds + 1, dl->cmds + from, (size_t)ncmd * sizeof(DisplayListCmd));
            for (int k = 1; k <= ncmd; k++) {
                cmds[k].x -= (float)ax; cmds[k].y -= (float)ay;
                if (cmds[k].type == DL_BORDER) { cmds[k].u.border.x -= (float)ax; cmds[k].u.border.y -= (float)ay; }
            }
            if (dl->mask_count) {
                memcpy(mcopy, dl->masks, (size_t)dl->mask_count * sizeof(DisplayMask));
                for (int k = 0; k < dl->mask_count; k++) {
                    mcopy[k].x -= (float)ax; mcopy[k].y -= (float)ay;
                    if (mcopy[k].kind == 1 || mcopy[k].iw > 0.0f) { mcopy[k].ix -= (float)ax; mcopy[k].iy -= (float)ay; }
                }
            }
            DisplayList tmp = *dl;
            tmp.masks = mcopy;
            tmp.cmds = cmds + 1; tmp.count = ncmd; tmp.capacity = ncmd;
            raster_display_list(&tmp, rgb_w, WM, HM);
            tmp.cmds = cmds; tmp.count = ncmd + 1; tmp.capacity = ncmd + 1;
            raster_display_list(&tmp, rgb_b, WM, HM);
            size_t N = (size_t)WM * HM;
            for (size_t k = 0; k < N; k++) {
                int a = 255;
                for (int c = 0; c < 3; c++) { int v = 255 - (int)rgb_w[k * 3 + c] + (int)rgb_b[k * 3 + c]; if (v < a) a = v; }
                if (a < 0) a = 0;
                rgba[k * 4 + 3] = (uint8_t)a;
                for (int c = 0; c < 3; c++) {
                    int v = a > 0 ? ((int)rgb_b[k * 3 + c] * 255 + a / 2) / a : 0;
                    rgba[k * 4 + c] = (uint8_t)(v > 255 ? 255 : v);
                }
            }
            float bx[4] = { (float)box->x - ax, (float)box->y - ay, (float)box->width, (float)box->height };
            SvgFilterEnv env = { ctx->doc, dl_flt_prop, dl_flt_color, dl_flt_image, dl_flt_element, (void *)ctx, { box->s->color_r, box->s->color_g, box->s->color_b, box->s->color_a }, false };
            svg_filter_apply(&env, box->s->filt_text, bx, (float)box->s->font_size, rgba, WM, HM);
            /* (the viewport's part) */
            for (int y = 0; y < VH; y++) memmove(rgba + ((size_t)y * VW) * 4, rgba + ((size_t)(y + vy0 - ay) * WM + (vx0 - ax)) * 4, (size_t)VW * 4);
            image_cache_put_rgba(g_image_cache, key, VW, VH, rgba);
        }
        free(rgb_w); free(rgb_b); free(rgba); free(cmds); free(mcopy);
    }
    /* the last transform set in there stays in force for what follows */
    DisplayListCmd lastx;
    bool has_x = false;
    for (int k = from; k < dl->count; k++) if (dl->cmds[k].type == DL_XFORM) { lastx = dl->cmds[k]; has_x = true; }
    dl->count = from;
    int handle = image_cache_request(g_image_cache, key, VW, VH);
    if (handle >= 0) {
        display_list_add_image(dl, (float)vx0, (float)vy0, (float)VW, (float)VH, handle, 0.0f, 0.0f, 1.0f, 1.0f);
        if (t_flt_clip && t_flt_clip[box_i][2] > t_flt_clip[box_i][0]) dl_clip_range(dl, from, t_flt_clip[box_i]);
    }
    if (has_x && display_list_reserve(dl, 1)) dl->cmds[dl->count++] = lastx;
}

void display_list_set_image_cache(ImageCache *cache)
{
    g_image_cache = cache;
    /* Any image becoming sized (layout) or decoded (paint) re-renders. */
    if (cache) image_cache_set_notify(cache, on_async_image_loaded, NULL);
}

ImageCache *display_list_get_image_cache(void)
{
    return g_image_cache;
}

bool display_list_set_default_font(const char *ttf_path, float size_pixels)
{
    (void)size_pixels;
    return ttf_path && font_registry_set_default_file(ttf_path);
}

struct TextShaper *display_list_get_default_font(void)
{
    return font_registry_slot(font_registry_default_slot());
}

struct TextShaper *display_list_get_font(int slot)
{
    TextShaper *s = font_registry_slot(slot);
    return s ? s : display_list_get_default_font();
}

int display_list_set_font_chain(const char *font_family, int font_weight, int font_italic)
{
    /* (per thread, by the interned list, its weight and style; again when web fonts change) */
    static __thread struct { const char *fam; char name[96]; int w, it; uint32_t gen; int n; TextShaper *f[6]; } cache[32];
    if (!font_family || !strchr(font_family, ',')) { text_shaper_set_chain(NULL, 0); return 0; }
    uint32_t gen = font_registry_face_generation();
    unsigned h = (unsigned)(((uintptr_t)font_family >> 4) ^ (unsigned)font_weight * 7u ^ (unsigned)font_italic) & 31u;
    /* (the pointer and the text both: a string freed with one page may be another's on the next) */
    if (cache[h].fam != font_family || strncmp(cache[h].name, font_family, sizeof cache[h].name) || cache[h].w != font_weight ||
        cache[h].it != font_italic || cache[h].gen != gen) {
        int slots[6];
        int n = font_registry_resolve_chain(font_family, font_weight, font_italic != 0, slots, 6);
        cache[h].n = 0;
        for (int k = 0; k < n; k++) {
            TextShaper *f = font_registry_slot(slots[k]);
            if (f) cache[h].f[cache[h].n++] = f;
        }
        cache[h].fam = font_family; cache[h].w = font_weight; cache[h].it = font_italic; cache[h].gen = gen;
        snprintf(cache[h].name, sizeof cache[h].name, "%s", font_family);
    }
    int n = cache[h].n > 1 ? cache[h].n : 0;   /* (one font: the registry's fallbacks as before) */
    text_shaper_set_chain(cache[h].f, n);
    return n;
}

int display_list_resolve_font_slot(const char *font_family, int font_weight,
                                   int font_italic)
{
    return font_registry_resolve(font_family, font_weight, font_italic != 0);
}

static bool text_is_whitespace(const char *s)
{
    for (; *s; s++) {
        if (!isspace((unsigned char)*s)) return false;
    }
    return true;
}

static bool node_class_contains_any(HtmlNode *node, const char **needles)
{
    if (!node || node->type != HTML_NODE_ELEMENT || !needles) return false;
    for (HtmlAttribute *a = node->attributes; a; a = a->next) {
        if (strcasecmp(a->name, "class") != 0 || !a->value[0]) continue;
        const char *cls = a->value;
        for (const char **n = needles; *n; n++) {
            if (strstr(cls, *n) != NULL) return true;
        }
    }
    return false;
}

/* True when the node or any ancestor is display:none or visibility:hidden.
 * Emissions that bypass the size filters (text glyphs, <img>, background
 * images) must check this so hidden subtrees (scripts, off-screen menus,
 * head content) do not leak onto the page at the origin. */
/* A box's rectangle on whole pixels, as browsers paint backgrounds and
 * borders: each edge rounded (a 1.33 px wide box is a 1 px line, not 2 px
 * with a faint one), and never less than a pixel when it has any size. */
static void snap_rect(float *x, float *y, float *w, float *h)
{
    float x0 = roundf(*x), y0 = roundf(*y), x1 = roundf(*x + *w), y1 = roundf(*y + *h);
    if (*w > 0.0f && x1 <= x0) x1 = x0 + 1.0f;
    if (*h > 0.0f && y1 <= y0) y1 = y0 + 1.0f;
    t_snap_by[0] = *x - x0; t_snap_by[1] = *y - y0; t_snap_by[2] = *x + *w - x1; t_snap_by[3] = *y + *h - y1;
    *x = x0;
    *y = y0;
    *w = x1 - x0;
    *h = y1 - y0;
    t_snap_at[0] = *x; t_snap_at[1] = *y; t_snap_at[2] = *w; t_snap_at[3] = *h;
    t_snap_on = true;
}

/* The backgrounds and borders of the pieces of split inline el (the left
 * edge on its first, the right on its last) that are placed against child
 * anchor (-1: those placed against el itself). */
/* Border side k's color (k: 0 left, 1 right, 2 top, 3 bottom): its own,
 * else currentColor; alpha times op. */
static void border_side_color(const LayoutBox *box, int k, float op, float c[4])
{
    if (box->s->border_color_set & (1 << k)) {
        uint32_t v = box->s->border_rgba[k];
        c[0] = (float)((v >> 24) & 0xFF) / 255.0f;
        c[1] = (float)((v >> 16) & 0xFF) / 255.0f;
        c[2] = (float)((v >> 8) & 0xFF) / 255.0f;
        /* (the alpha as it was written, in hundredths, when the byte is one of those) */
        float al = (float)(v & 0xFF) / 255.0f, ah = roundf(al * 100.0f) / 100.0f;
        if (fabsf(al - ah) < 0.002f) al = ah;
        c[3] = al * op;
    } else {
        c[0] = (float)box->s->color_r;
        c[1] = (float)box->s->color_g;
        c[2] = (float)box->s->color_b;
        c[3] = (float)box->s->color_a * op;
    }
}

static int dl_add_mask(DisplayList *dl, const DisplayMask *m);
static bool dl_add_mask_cmd(DisplayList *dl, DisplayListCmdType type, int mask_index, const DisplayMask *m);

/* Blink's Color::Dark(): the shade of a 3D border style's darker sides
 * (black's is a light gray, so it still shows). */
static void border_dark(const float c[4], float out[4])
{
    if (c[0] == 0.0f && c[1] == 0.0f && c[2] == 0.0f) {
        out[0] = out[1] = out[2] = (float)0xAB / 255.0f;
    } else {
        float v = c[0] > c[1] ? c[0] : c[1];
        if (c[2] > v) v = c[2];
        float k = v > 0.0f ? (v - 0.33f) / v : 0.0f;
        if (k < 0.0f) k = 0.0f;
        out[0] = c[0] * k;
        out[1] = c[1] * k;
        out[2] = c[2] * k;
    }
    out[3] = c[3];
}

/* One band of border side `side` (0 top 1 right 2 bottom 3 left) of the
 * border box (bx, by, bw, bh) with widths bwid (top, right, bottom, left):
 * f0..f1 of its width from the outer edge, in color c, its corners joined
 * to the neighbouring sides along the diagonals (mask kind 6) at the ends
 * cuts names (1 its first going clockwise, 2 its second); at the others it
 * fills the corner. */
static void border_band(DisplayList *dl, float bx, float by, float bw, float bh, const float bwid[4], int side,
                        float f0, float f1, const float c[4], unsigned cuts)
{
    float t = bwid[side];
    if (t <= 0.0f || f1 <= f0 || c[3] <= 0.0f) return;
    DisplayMask m;
    memset(&m, 0, sizeof(m));
    m.kind = 6;
    m.x = side == 1 ? bx + bw - t : bx;
    m.y = side == 2 ? by + bh - t : by;
    m.w = side == 0 || side == 2 ? bw : t;
    m.h = side == 0 || side == 2 ? t : bh;
    for (int k = 0; k < 4; k++) m.border[k] = bwid[k];
    m.angle_deg = (float)side;
    m.nstops = 2;
    m.stop_pos[0] = f0;
    m.stop_pos[1] = f1;
    m.stop_alpha[0] = (cuts & 1) ? 1.0f : 0.0f;
    m.stop_alpha[1] = (cuts & 2) ? 1.0f : 0.0f;
    int mi = dl_add_mask(dl, &m);
    if (mi < 0 || !dl_add_mask_cmd(dl, DL_MASK_BEGIN, mi, &m)) return;
    display_list_add_rect(dl, m.x, m.y, m.w, m.h, c[0], c[1], c[2], c[3]);
    dl_add_mask_cmd(dl, DL_MASK_END, mi, &m);
}

/* A dashed or dotted side (side as border_band's), inside its side mask so
 * its corners join the neighbours: dashes 2 widths long with 1 width gaps
 * (3 and 2 under 3px), dots a width across a width apart, as Blink draws
 * them, the gaps stretched or shrunk so the side starts and ends with one;
 * dots 3px and over round (a rounded-rectangle mask each). */
static void border_dashes(DisplayList *dl, float bx, float by, float bw, float bh, const float bwid[4], int side,
                          const float c[4], bool dotted, unsigned cuts)
{
    float t = bwid[side];
    if (t <= 0.0f || c[3] <= 0.0f) return;
    bool horiz = side == 0 || side == 2;
    float sx = side == 1 ? bx + bw - t : bx, sy = side == 2 ? by + bh - t : by;
    float len = horiz ? bw : bh;
    float dash = dotted ? t : t * (t >= 3.0f ? 2.0f : 3.0f);
    float gap = dotted ? t : t * (t >= 3.0f ? 1.0f : 2.0f);
    int n = (int)floorf((len + gap) / (dash + gap) + 0.5f);
    if (n < 1) n = 1;
    float g = n > 1 ? (len - (float)n * dash) / (float)(n - 1) : 0.0f;
    if (n == 1 || g < 0.0f) { n = 1; dash = len; g = 0.0f; }
    DisplayMask m;
    memset(&m, 0, sizeof(m));
    m.kind = 6;
    m.x = sx;
    m.y = sy;
    m.w = horiz ? bw : t;
    m.h = horiz ? t : bh;
    for (int k = 0; k < 4; k++) m.border[k] = bwid[k];
    m.angle_deg = (float)side;
    m.nstops = 2;
    m.stop_pos[0] = 0.0f;
    m.stop_pos[1] = 1.0f;
    m.stop_alpha[0] = (cuts & 1) ? 1.0f : 0.0f;
    m.stop_alpha[1] = (cuts & 2) ? 1.0f : 0.0f;
    int mi = dl_add_mask(dl, &m);
    if (mi < 0 || !dl_add_mask_cmd(dl, DL_MASK_BEGIN, mi, &m)) return;
    bool round = dotted && t >= 3.0f && n > 1;
    for (int i = 0; i < n; i++) {
        float at = (float)i * (dash + g);
        float x = horiz ? sx + at : sx, y = horiz ? sy : sy + at;
        float w = horiz ? dash : t, h = horiz ? t : dash;
        if (!round) {
            display_list_add_rect(dl, x, y, w, h, c[0], c[1], c[2], c[3]);
            continue;
        }
        DisplayMask d;
        memset(&d, 0, sizeof(d));
        d.kind = 3;
        d.x = x;
        d.y = y;
        d.w = w;
        d.h = h;
        for (int k = 0; k < 4; k++) d.rx[k] = d.ry[k] = t * 0.5f;
        int di = dl_add_mask(dl, &d);
        if (di < 0 || !dl_add_mask_cmd(dl, DL_MASK_BEGIN, di, &d)) break;
        display_list_add_rect(dl, x, y, w, h, c[0], c[1], c[2], c[3]);
        dl_add_mask_cmd(dl, DL_MASK_END, di, &d);
    }
    dl_add_mask_cmd(dl, DL_MASK_END, mi, &m);
}

/* A border whose sides differ (in color or style) or are not solid: each
 * side through its own mask, in its style (CSS 2.1 section 8.5.3; double's
 * thirds and the 3D styles' shades as Blink draws them).  Sides indexed as
 * the style's (0 left, 1 right, 2 top, 3 bottom). */
static void paint_border_sides(DisplayList *dl, const LayoutBox *box, float bx, float by, float bw, float bh,
                               const float col[4][4], const float bwid[4])
{
    static const int style_k[4] = { 2, 1, 3, 0 };   /* top, right, bottom, left -> style index */
    for (int side = 0; side < 4; side++) {
        int k = style_k[side];
        float t = bwid[side];
        if (t <= 0.0f) continue;
        const float *c = col[k];
        int st = box->s->border_style[k];
        /* A solid side drawn first at a corner it shares with another
         * solid side fills it, the later one is cut along the diagonal (no
         * seam of the background between them, as Blink draws them); a
         * corner with another style is cut on both sides. */
        unsigned cuts = 0;
        int ends[2] = { (side + 3) & 3, (side + 1) & 3 };
        for (int e = 0; e < 2; e++) {
            int n = ends[e];
            /* (two solid sides of one color share it; of two colors, cut on the diagonal) */
            bool solid = st == CSS_BORDER_SOLID && box->s->border_style[style_k[n]] == CSS_BORDER_SOLID &&
                         (bwid[n] <= 0.0f || !memcmp(col[k], col[style_k[n]], 4 * sizeof(float)));
            if (!solid || n < side) cuts |= 1u << e;
        }
        bool top_left = side == 0 || side == 3;
        float dark[4];
        border_dark(c, dark);
        if (st == CSS_BORDER_DOUBLE && t >= 3.0f) {
            float third = floorf((t + 1.0f) / 3.0f);
            border_band(dl, bx, by, bw, bh, bwid, side, 0.0f, third / t, c, cuts);
            border_band(dl, bx, by, bw, bh, bwid, side, (t - third) / t, 1.0f, c, cuts);
        } else if (st == CSS_BORDER_DASHED || st == CSS_BORDER_DOTTED) {
            border_dashes(dl, bx, by, bw, bh, bwid, side, c, st == CSS_BORDER_DOTTED, cuts);
        } else if (st == CSS_BORDER_INSET || st == CSS_BORDER_OUTSET) {
            bool darker = top_left == (st == CSS_BORDER_INSET);
            border_band(dl, bx, by, bw, bh, bwid, side, 0.0f, 1.0f, darker ? dark : c, cuts);
        } else if (st == CSS_BORDER_GROOVE || st == CSS_BORDER_RIDGE) {
            /* (the outer half as inset for groove, outset for ridge; the
             * inner half the other) */
            bool outer_dark = top_left == (st == CSS_BORDER_GROOVE);
            border_band(dl, bx, by, bw, bh, bwid, side, 0.0f, 0.5f, outer_dark ? dark : c, cuts);
            border_band(dl, bx, by, bw, bh, bwid, side, 0.5f, 1.0f, outer_dark ? c : dark, cuts);
        } else {
            border_band(dl, bx, by, bw, bh, bwid, side, 0.0f, 1.0f, c, cuts);
        }
    }
}

/* Whether a box's border can be drawn as four plain rectangles: every side
 * with a width solid (or double too thin to show its gap) and one color. */
static bool border_uniform(const LayoutBox *box, const float col[4][4], const float bwid[4])
{
    const float w[4] = { bwid[3], bwid[1], bwid[0], bwid[2] };   /* (as the style's: left, right, top, bottom) */
    int first = -1;
    for (int k = 0; k < 4; k++) {
        if (w[k] <= 0.0f) continue;
        int st = box->s->border_style[k];
        if (st != CSS_BORDER_SOLID && !(st == CSS_BORDER_DOUBLE && w[k] < 3.0f)) return false;
        if (first < 0) first = k;
        else if (memcmp(col[k], col[first], sizeof(col[k])) != 0) return false;
    }
    return true;
}

static void emit_background_image(const LayoutContext *ctx, DisplayList *dl, const LayoutBox *box, const float *area);
static void paint_pieces(LayoutContext *ctx, DisplayList *dl, int el, int anchor, float op)
{
    double run = 0.0, total = 0.0;   /* (the pieces end to end: where each starts, how long they are) */
    for (int q = 0; q < ctx->pieces[el].n; q++) total += ctx->pieces[el].p[q].w;
    const LayoutBox *box = &ctx->boxes[el];
    const LayoutPieceList *pieces = &ctx->pieces[el];
    float cl[4], cr[4], ct[4], cb[4];
    border_side_color(box, 0, op, cl);
    border_side_color(box, 1, op, cr);
    border_side_color(box, 2, op, ct);
    border_side_color(box, 3, op, cb);
    for (int q = 0; q < pieces->n; q++) {
        const LayoutPiece *pc = &pieces->p[q];
        double at = run;
        run += pc->w;
        if (pc->anchor != anchor) continue;
        const LayoutBox *ab = pc->anchor >= 0 ? &ctx->boxes[pc->anchor] : box;
        float px = (float)(ab->x + pc->dx), py = (float)(ab->y + pc->dy);
        float pw = (float)pc->w, ph = (float)pc->h;
        if (pw <= 0.0f || ph <= 0.0f) continue;
        snap_rect(&px, &py, &pw, &ph);
        if (box->s->background_color_a > 0.0f)
            display_list_add_rect(dl, px, py, pw, ph, (float)box->s->background_color_r,
                                  (float)box->s->background_color_g, (float)box->s->background_color_b,
                                  (float)box->s->background_color_a * op);
        if (g_image_cache && (box->s->background_image_url[0] || box->s->bg_grad) && box->s->bg_clip != 3 && pw >= 1.0f && ph >= 1.0f) {
            /* (its image: over the pieces end to end, each clipped to its own; clone: over each) */
            LayoutBox ib = *box;
            float area[4] = { px, py, px + pw, py + ph };
            ib.y = py; ib.height = ph;
            if (box->s->bdb_clone) { ib.x = px; ib.width = pw; }
            else { ib.x = px - at; ib.width = total; }
            emit_background_image(ctx, dl, &ib, area);
        }
        float bt = (float)box->s->border_top, bb = (float)box->s->border_bottom;
        float bl = (pc->sides & 1) ? (float)box->s->border_left : 0.0f;
        float brr = (pc->sides & 2) ? (float)box->s->border_right : 0.0f;
        float mid = ph - bt - bb;
        if (mid < 0.0f) mid = 0.0f;
        if (bt > 0.0f) display_list_add_rect(dl, px, py, pw, bt, ct[0], ct[1], ct[2], ct[3]);
        if (bb > 0.0f) display_list_add_rect(dl, px, py + ph - bb, pw, bb, cb[0], cb[1], cb[2], cb[3]);
        if (bl > 0.0f) display_list_add_rect(dl, px, py + bt, bl, mid, cl[0], cl[1], cl[2], cl[3]);
        if (brr > 0.0f) display_list_add_rect(dl, px + pw - brr, py + bt, brr, mid, cr[0], cr[1], cr[2], cr[3]);
    }
}

/* A table cell empty-cells: hide leaves unpainted (its background and
 * borders, in the separated borders model): one with no content but
 * collapsible white space (CSS 2.1 section 17.6.1.1). */
static bool cell_hidden_empty(LayoutContext *ctx, int i)
{
    const LayoutBox *b = &ctx->boxes[i];
    if (b->s->table_role != 4 || !b->s->empty_cells_hide || b->s->border_collapse) return false;
    for (int c = ctx->tree.nodes[i].first_child_idx; c >= 0; c = ctx->tree.nodes[c].next_sibling_idx) {
        const LayoutBox *cb = &ctx->boxes[c];
        if (cb->s->display == CSS_DISPLAY_NONE) continue;
        HtmlNode *n = css_layout_box_node(ctx, c);
        if (!n) return false;
        if (n->type == HTML_NODE_TEXT) {
            const char *t = css_layout_box_text(ctx, c, n->text_content ? n->text_content : "");
            for (; t && *t; t++)
                if (!(*t == ' ' || *t == '\t' || *t == '\n' || *t == '\r' || *t == '\f')) return false;
            continue;
        }
        return false;
    }
    return true;
}

static bool box_or_ancestor_hidden(LayoutContext *ctx, int node_idx)
{
    int idx = node_idx;
    /* (visibility inherits, and a descendant can be visible again: the
     * nearest box that sets it decides) */
    bool decided = false, hidden = false;
    while (idx >= 0) {
        LayoutBox *b = &ctx->boxes[idx];
        if (b->s->display == CSS_DISPLAY_NONE) return true;
        /* (an object showing its image: its fallback content is not shown) */
        if (idx != node_idx && ctx->doc && display_list_object_image(css_layout_box_node(ctx, idx))) return true;
        if (!decided && (b->s->visibility_set || b->s->visibility != CSS_VISIBILITY_VISIBLE)) {
            decided = true;
            hidden = b->s->visibility != CSS_VISIBILITY_VISIBLE;
        }
        idx = ctx->tree.nodes[idx].parent_idx;
    }
    return hidden;
}

static const char* node_attribute_value(HtmlNode *node, const char *name)
{
    if (!node || node->type != HTML_NODE_ELEMENT) return NULL;
    for (HtmlAttribute *a = node->attributes; a; a = a->next) {
        if (strcasecmp(a->name, name) == 0) return a->value;
    }
    return NULL;
}

/* Pick the best candidate from an <img srcset> for the box's CSS width.
 * Width descriptors (300w): smallest candidate >= css width, else the
 * largest.  Density descriptors (1.5x): smallest >= 1x, else the largest.
 * Returns a malloc'd URL string (caller frees) or NULL. */
static char *srcset_pick(const char *srcset, float css_width)
{
    if (!srcset || !srcset[0]) return NULL;
    char *copy = strdup(srcset);
    if (!copy) return NULL;

    char *best_w_ge = NULL;  double best_w_ge_v = 0.0;  /* smallest w >= target */
    char *best_any = NULL;   double best_any_v = 0.0;   /* largest overall */
    char *best_x_ge = NULL;  double best_x_ge_v = 0.0;  /* smallest x >= 1 */
    char *best_x = NULL;     double best_x_v = 0.0;     /* largest x */

    char *save = NULL;
    for (char *cand = strtok_r(copy, ",", &save); cand; cand = strtok_r(NULL, ",", &save)) {
        while (isspace((unsigned char)*cand)) cand++;
        if (!cand[0]) continue;
        /* Split URL from descriptor at the first space. */
        char *desc = NULL;
        for (char *q = cand; *q; q++) {
            if (isspace((unsigned char)*q)) { *q = '\0'; desc = q + 1; break; }
        }
        double w = -1.0, x = 1.0;
        if (desc) {
            while (isspace((unsigned char)*desc)) desc++;
            char *end = NULL;
            double v = strtod(desc, &end);
            if (end && end != desc) {
                if (*end == 'w') { w = v; x = -1.0; }
                else if (*end == 'x') x = v;
            }
        }
        if (w > 0.0) {
            if (css_width > 0.0f && w >= css_width &&
                (best_w_ge == NULL || w < best_w_ge_v)) {
                free(best_w_ge); best_w_ge = strdup(cand); best_w_ge_v = w;
            }
            if (best_any == NULL || w > best_any_v) {
                free(best_any); best_any = strdup(cand); best_any_v = w;
            }
        } else {
            if (x >= 1.0 && (best_x_ge == NULL || x < best_x_ge_v)) {
                free(best_x_ge); best_x_ge = strdup(cand); best_x_ge_v = x;
            }
            if (best_x == NULL || x > best_x_v) {
                free(best_x); best_x = strdup(cand); best_x_v = x;
            }
        }
    }

    char *out = NULL;
    if (best_w_ge || best_any) out = best_w_ge ? best_w_ge : best_any;
    else out = best_x_ge ? best_x_ge : best_x;
    if (best_w_ge && out != best_w_ge) free(best_w_ge);
    if (best_any && out != best_any) free(best_any);
    if (best_x_ge && out != best_x_ge) free(best_x_ge);
    if (best_x && out != best_x) free(best_x);
    free(copy);
    return out;
}

static char* dl_resolve_url(const char *base_url, const char *href)
{
    if (!href || !href[0]) return NULL;
    if (!strncmp(href, "cbfilter:", 9)) {
        /* (a filter() image: the url after the list is the one to resolve) */
        const char *bar = strchr(href, '|');
        char *in = bar ? dl_resolve_url(base_url, bar + 1) : NULL;
        if (!in) return strdup(href);
        size_t pl = (size_t)(bar + 1 - href), il = strlen(in);
        char *out = (char *)malloc(pl + il + 1);
        if (out) { memcpy(out, href, pl); memcpy(out + pl, in, il + 1); }
        free(in);
        return out;
    }
    if (url_has_scheme(href))
        return strdup(href);
    if (base_url && url_has_scheme(base_url)) {
        char *abs = wurl_resolve(base_url, href);
        if (abs) return abs;
    }
    if (strncmp(href, "//", 2) == 0) {
        char buf[2048];
        snprintf(buf, sizeof(buf), "https:%s", href);
        return strdup(buf);
    }
    if (href[0] == '/')
        return strdup(href);
    if (!base_url || !base_url[0])
        return strdup(href);

    const char *base = base_url;
    if (href[0] == '/') {
        char buf[2048];
        if (base[strlen(base) - 1] == '/') {
            snprintf(buf, sizeof(buf), "%s%s", base, href + 1);
        } else {
            snprintf(buf, sizeof(buf), "%s%s", base, href);
        }
        return strdup(buf);
    }
    char buf[2048];
    if (base[strlen(base) - 1] == '/') {
        snprintf(buf, sizeof(buf), "%s%s", base, href);
    } else {
        const char *last_slash = strrchr(base, '/');
        if (last_slash) {
            size_t base_len = (size_t)(last_slash - base) + 1;
            snprintf(buf, sizeof(buf), "%.*s%s", (int)base_len, base, href);
        } else {
            snprintf(buf, sizeof(buf), "%s/%s", base, href);
        }
    }
    return strdup(buf);
}

static void emit_image(DisplayList *dl, float x, float y, float w, float h, int handle)
{
    if (handle < 0) return;
    int img_w = 0, img_h = 0, ch = 0;
    uint8_t *pix = NULL;
    if (!image_cache_get(g_image_cache, handle, &img_w, &img_h, &ch, &pix)) return;
    if (w <= 0.0f) w = (float)img_w;
    if (h <= 0.0f) h = (float)img_h;
    if (w <= 0.0f || h <= 0.0f) return;
    display_list_add_image(dl, x, y, w, h, handle, 0.0f, 0.0f, 1.0f, 1.0f);
}

static void on_async_image_loaded(const char *url, void *user_data)
{
    (void)url;
    (void)user_data;
    dom_request_relayout();
}

/* Paint an <img> (or other replaced image) into its content box once its
 * pixels are decoded; the decode is requested at the painted size.  While
 * loading nothing is painted (like browsers); the cache notification
 * triggers a re-render when the pixels arrive.  object-fit maps the image
 * into the box: fill (stretch), contain, cover (crop), none, scale-down. */
static void emit_image_async(DisplayList *dl, const LayoutBox *box, const char *url, float op)
{
    if (!g_image_cache || !url || !url[0]) return;
    float cx = (float)(box->x + box->s->border_left + box->padding_left);
    float cy = (float)(box->y + box->s->border_top + box->padding_top);
    float cw = (float)(box->width - box->s->border_left - box->s->border_right - box->padding_left - box->padding_right);
    float ch = (float)(box->height - box->s->border_top - box->s->border_bottom - box->padding_top - box->padding_bottom);
    if (cw <= 0.0f || ch <= 0.0f) return;
    snap_rect(&cx, &cy, &cw, &ch);      /* (on whole pixels, as a background is) */
    int handle = image_cache_request(g_image_cache, url, (int)ceilf(cw), (int)ceilf(ch));
    if (handle < 0) return;
    int img_w = 0, img_h = 0, chn = 0;
    uint8_t *pix = NULL;
    if (!image_cache_get(g_image_cache, handle, &img_w, &img_h, &chn, &pix)) return;
    if (img_w <= 0 || img_h <= 0) return;
    int nw = img_w, nh = img_h;
    image_cache_get_natural(g_image_cache, handle, &nw, &nh);
    if (nw <= 0 || nh <= 0) { nw = img_w; nh = img_h; }
    /* (object-view-box: the view box stands for the image, fitted and placed;
     * the whole image around it, cut to the content box) */
    double vbx = 0.0, vby = 0.0, vbw = nw, vbh = nh;
    int fullw = nw, fullh = nh;
    unsigned vdims = 7;
    float vr = 0.0f;
    if (box->s->ovb_kind) image_cache_intrinsic(g_image_cache, url, &vdims, &vr);
    if ((vdims & 3) == 3 && css_layout_view_box(box->s, nw, nh, &vbx, &vby, &vbw, &vbh)) {
        nw = (int)lround(vbw); nh = (int)lround(vbh);
        if (nw <= 0) nw = 1;
        if (nh <= 0) nh = 1;
    } else { vbx = vby = 0.0; vbw = nw; vbh = nh; }

    float dw = cw, dh = ch;
    float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
    int fit = box->s->object_fit;
    /* (none or scale-down without a natural width and height: the concrete size by the
     * default sizing algorithm, the content box the default object size, CSS Images 3 5.5) */
    if ((fit == 3 || fit == 4) && !box->s->ovb_kind) {
        unsigned idims = 7;
        float ir = 0.0f;
        image_cache_intrinsic(g_image_cache, url, &idims, &ir);
        if ((idims & 3) != 3) {
            float r = ir > 0.0f ? ir : (float)nw / (float)nh;   /* (width over height) */
            if (!(idims & 4) && !(idims & 3)) { nw = (int)lroundf(cw); nh = (int)lroundf(ch); }   /* (no ratio either: the box) */
            else if (idims & 1) { nh = (int)lroundf((float)nw / r); }
            else if (idims & 2) { nw = (int)lroundf((float)nh * r); }
            else if (cw / ch > r) { nh = (int)lroundf(ch); nw = (int)lroundf(ch * r); }
            else { nw = (int)lroundf(cw); nh = (int)lroundf(cw / r); }
            if (nw <= 0) nw = 1;
            if (nh <= 0) nh = 1;
        }
    }
    if (fit == 4) {   /* scale-down: none or contain, whichever is smaller */
        fit = ((float)nw <= cw && (float)nh <= ch) ? 3 : 1;
    }
    if (fit == 1 || fit == 2 || fit == 3) {
        float s = fit == 1 ? fminf(cw / (float)nw, ch / (float)nh) : fit == 2 ? fmaxf(cw / (float)nw, ch / (float)nh) : 1.0f;
        dw = (float)nw * s;
        dh = (float)nh * s;
    }
    /* (object-position: in the room left over, then cut to the content box) */
    const LayoutStyle *ps = box->s;
    float xf = ps->obj_pos_set ? ps->obj_xf : 0.5f, yf = ps->obj_pos_set ? ps->obj_yf : 0.5f;
    float xpx = ps->obj_pos_set ? ps->obj_xpx : 0.0f, ypx = ps->obj_pos_set ? ps->obj_ypx : 0.0f;
    float dx = cx + (cw - dw) * xf + xpx, dy = cy + (ch - dh) * yf + ypx;
    if (vbw != fullw || vbh != fullh || vbx != 0.0 || vby != 0.0) {
        /* (from the view box's place to the whole image's) */
        float sx = dw / (float)vbw, sy = dh / (float)vbh;
        dx -= (float)vbx * sx; dy -= (float)vby * sy;
        dw = (float)fullw * sx; dh = (float)fullh * sy;
    }
    if (dw > 0.0f && dh > 0.0f && (dx < cx - 0.01f || dy < cy - 0.01f || dx + dw > cx + cw + 0.01f || dy + dh > cy + ch + 0.01f)) {
        float vx0 = fmaxf(dx, cx), vy0 = fmaxf(dy, cy);
        float vx1 = fminf(dx + dw, cx + cw), vy1 = fminf(dy + dh, cy + ch);
        if (vx1 <= vx0 || vy1 <= vy0) return;
        u0 = (vx0 - dx) / dw; u1 = (vx1 - dx) / dw;
        v0 = (vy0 - dy) / dh; v1 = (vy1 - dy) / dh;
        dx = vx0; dy = vy0; dw = vx1 - vx0; dh = vy1 - vy0;
    }
    if (dw <= 0.0f || dh <= 0.0f) return;
    int from = dl->count;
    display_list_add_image(dl, dx, dy, dw, dh, handle, u0, v0, u1, v1);
    if (op < 0.999f && dl->count > from) dl->cmds[from].a = op;
}

/* ---- inline <svg> ---- */

typedef struct { char *s; size_t n, cap; } SvgBuf;

static void sb_put(SvgBuf *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 1024;
        while (nc < b->n + n + 1) nc *= 2;
        char *t = (char *)realloc(b->s, nc);
        if (!t) return;
        b->s = t;
        b->cap = nc;
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = '\0';
}
static void sb_puts(SvgBuf *b, const char *s) { sb_put(b, s, strlen(s)); }

static void sb_escaped(SvgBuf *b, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        switch (s[i]) {
        case '&': sb_puts(b, "&amp;"); break;
        case '<': sb_puts(b, "&lt;"); break;
        case '>': sb_puts(b, "&gt;"); break;
        case '"': sb_puts(b, "&quot;"); break;
        default: sb_put(b, s + i, 1);
        }
    }
}

/* SVG names are case-sensitive; the HTML parser lower-cased them. */
static const char *svg_case(const char *lower)
{
    static const char *const names[] = {
        "viewBox", "preserveAspectRatio", "gradientUnits", "gradientTransform", "spreadMethod",
        "patternUnits", "patternContentUnits", "patternTransform", "clipPathUnits", "maskUnits",
        "maskContentUnits", "markerUnits", "markerWidth", "markerHeight", "refX", "refY",
        "stdDeviation", "textLength", "lengthAdjust", "startOffset", "pathLength",
        "linearGradient", "radialGradient", "clipPath", "foreignObject", "textPath",
        "feGaussianBlur", "feOffset", "feBlend", "feColorMatrix", "feComposite", "feFlood",
        "feMerge", "feMergeNode", NULL
    };
    for (int i = 0; names[i]; i++)
        if (strcasecmp(lower, names[i]) == 0) return names[i];
    return lower;
}

/* (the layout the inline <svg> being serialized is in: its elements' CSS
 * transform-origin and transform-box go along as attributes) */
static __thread LayoutContext *t_svg_ctx;

static bool svg_clip_shape(HtmlNode *n, const char *v, char *out, size_t cap);
static __thread int t_svg_clip_n;

/* (an element's clip-path: its attribute, else its style's) */
static const char *svg_clip_value(HtmlNode *n, char *buf, size_t cap)
{
    const char *v = node_attribute_value(n, "clip-path");
    if (v) return v;
    const char *st = node_attribute_value(n, "style");
    const char *p = st ? strstr(st, "clip-path") : NULL;
    if (!p) return NULL;
    p += 9;
    while (*p == ' ') p++;
    if (*p != ':') return NULL;
    p++;
    size_t k = 0;
    while (*p && *p != ';' && k + 1 < cap) buf[k++] = *p++;
    buf[k] = 0;
    return buf;
}

/* (the page's style rules for an SVG element: its presentation properties as
 * the cascade gives them, collected as a style text, later ones last) */
/* (inh_only: the properties that inherit only; geometry ones (x, y, width, ...) apart, as attributes) */
typedef struct { char *t; size_t n, cap; bool inh_only; int ng; char gname[10][8]; char gval[10][64]; } SvgCasc;
static void svg_casc_decl(void *ud, const char *prop, const char *value, bool important)
{
    (void)important;
    static const char *const keep[] = { "filter", "fill", "fill-opacity", "fill-rule", "stroke", "stroke-width", "stroke-opacity",
        "stroke-dasharray", "stroke-dashoffset", "stroke-linecap", "stroke-linejoin", "stroke-miterlimit", "opacity",
        "stop-color", "stop-opacity", "visibility", "color", "font-family", "font-size", "font-weight", "font-style",
        "text-anchor", "clip-rule", "paint-order", "vector-effect", "x", "y", "width", "height", "r", "cx", "cy", "rx",
        "ry", "d", "dominant-baseline", "letter-spacing", "word-spacing", "flood-color", "flood-opacity", "lighting-color",
        "marker-start", "marker-mid", "marker-end", "mix-blend-mode", "display" };
    SvgCasc *c = (SvgCasc *)ud;
    if (!prop || !value || prop[0] == 1) return;
    bool ok = false;
    for (size_t k = 0; k < sizeof keep / sizeof keep[0] && !ok; k++) ok = !strcasecmp(prop, keep[k]);
    if (ok && c->inh_only) {
        static const char *const inh[] = { "fill", "fill-opacity", "fill-rule", "stroke", "stroke-width", "stroke-opacity",
            "stroke-dasharray", "stroke-dashoffset", "stroke-linecap", "stroke-linejoin", "stroke-miterlimit", "visibility",
            "color", "font-family", "font-size", "font-weight", "font-style", "text-anchor", "clip-rule", "paint-order",
            "dominant-baseline", "letter-spacing", "word-spacing", "marker-start", "marker-mid", "marker-end" };
        ok = false;
        for (size_t k = 0; k < sizeof inh / sizeof inh[0] && !ok; k++) ok = !strcasecmp(prop, inh[k]);
    }
    if (!ok || strchr(value, '"') || strstr(value, "var(")) return;
    static const char *const geo[] = { "x", "y", "width", "height", "r", "cx", "cy", "rx", "ry" };
    for (size_t k = 0; k < sizeof geo / sizeof geo[0]; k++) {
        if (strcasecmp(prop, geo[k])) continue;
        if (c->inh_only || !strcasecmp(value, "auto")) return;
        int at = -1;
        for (int q = 0; q < c->ng; q++) if (!strcmp(c->gname[q], geo[k])) at = q;
        if (at < 0 && c->ng < 10) at = c->ng++;
        if (at >= 0) { snprintf(c->gname[at], sizeof c->gname[at], "%s", geo[k]); snprintf(c->gval[at], sizeof c->gval[at], "%s", value); }
        return;
    }
    size_t need = strlen(prop) + strlen(value) + 4;
    if (c->n + need + 1 > c->cap) {
        size_t nc = c->cap ? c->cap * 2 : 256;
        while (nc < c->n + need + 1) nc *= 2;
        char *t = (char *)realloc(c->t, nc);
        if (!t) return;
        c->t = t;
        c->cap = nc;
    }
    c->n += (size_t)snprintf(c->t + c->n, c->cap - c->n, "%s:%s;", prop, value);
}

static void svg_serialize(PreOrderCompactionArray *arr, int idx, SvgBuf *b, bool root,
                          int w, int h, const char *root_style)
{
    HtmlNode *n = (HtmlNode *)po_array_payload(arr, idx);
    if (!n) return;
    if (n->type == HTML_NODE_TEXT) {
        if (n->text_content) sb_escaped(b, n->text_content, n->text_len);
        return;
    }
    if (n->type != HTML_NODE_ELEMENT || !n->tag_name) return;
    const char *tag = svg_case(n->tag_name);
    /* (a CSS basic shape for clip-path on a shape: a <clipPath> of it made first) */
    char cpv[160], cps[256], cpid[32];
    cpid[0] = 0;
    const char *cv = root ? NULL : svg_clip_value(n, cpv, sizeof cpv);
    if (cv && svg_clip_shape(n, cv, cps, sizeof cps)) {
        snprintf(cpid, sizeof cpid, "cbclip%d", t_svg_clip_n++);
        sb_puts(b, "<clipPath id=\"");
        sb_puts(b, cpid);
        sb_puts(b, "\">");
        sb_puts(b, cps);
        sb_puts(b, "</clipPath>");
    }
    sb_puts(b, "<");
    sb_puts(b, tag);
    if (cpid[0]) {
        sb_puts(b, " clip-path=\"url(#");
        sb_puts(b, cpid);
        sb_puts(b, ")\"");
    }
    bool has_xmlns = false;
    /* (the style rules' presentation properties, then its own style attribute over them) */
    SvgCasc casc = { NULL, 0, 0, false, 0, { { 0 } }, { { 0 } } };
    if (!root && t_svg_ctx && !cpid[0]) css_layout_dom_cascade(t_svg_ctx, idx, -1, svg_casc_decl, &casc);
    if (casc.n) {
        const char *own = node_attribute_value(n, "style");
        sb_puts(b, " style=\"");
        sb_escaped(b, casc.t, casc.n);
        if (own && *own) { sb_puts(b, ";"); sb_escaped(b, own, strlen(own)); }
        sb_puts(b, "\"");
    }
    free(casc.t);
    /* (geometry the style rules give: attributes, over the element's own) */
    for (int q = 0; q < casc.ng; q++) {
        sb_puts(b, " ");
        sb_puts(b, casc.gname[q]);
        sb_puts(b, "=\"");
        sb_escaped(b, casc.gval[q], strlen(casc.gval[q]));
        sb_puts(b, "\"");
    }
    for (HtmlAttribute *a = n->attributes; a; a = a->next) {
        if (!a->name) continue;
        if (casc.n && !strcasecmp(a->name, "style")) continue;
        bool over = false;
        for (int q = 0; q < casc.ng && !over; q++) over = !strcasecmp(a->name, casc.gname[q]);
        if (over) continue;
        if (cpid[0] && (!strcasecmp(a->name, "clip-path") || !strcasecmp(a->name, "style"))) continue;
        if (root && (strcasecmp(a->name, "width") == 0 || strcasecmp(a->name, "height") == 0 ||
                     strcasecmp(a->name, "style") == 0)) continue;
        if (strcasecmp(a->name, "xmlns") == 0) has_xmlns = true;
        sb_puts(b, " ");
        sb_puts(b, svg_case(a->name));
        sb_puts(b, "=\"");
        if (a->value) sb_escaped(b, a->value, strlen(a->value));
        sb_puts(b, "\"");
    }
    if (!root && t_svg_ctx) {
        LayoutBox *eb = css_layout_box_for_node(t_svg_ctx, idx);
        if (eb && eb->s) {
            bool own = false;
            for (HtmlAttribute *a = n->attributes; a; a = a->next)
                if (a->name && !strcasecmp(a->name, "transform-origin")) own = true;
            if (!own && eb->s->tf_origin) {
                sb_puts(b, " transform-origin=\"");
                sb_escaped(b, eb->s->tf_origin, strlen(eb->s->tf_origin));
                sb_puts(b, "\"");
            }
            /* (the translate property first, then transform: as CSS composes them) */
            /* (only with no transform text: a transform's own translation shares those fields) */
            bool tr = !(eb->s->tf_raw && strstr(eb->s->tf_raw, "translate")) && eb->s->has_translate && (eb->s->translate_x != 0.0 || eb->s->translate_y != 0.0);
            if (eb->s->tf_raw || tr) {
                sb_puts(b, " css-transform=\"");
                if (tr) {
                    char tt[96];
                    snprintf(tt, sizeof tt, "translate(%gpx, %gpx) ", eb->s->translate_x, eb->s->translate_y);
                    sb_puts(b, tt);
                }
                if (eb->s->tf_raw) sb_escaped(b, eb->s->tf_raw, strlen(eb->s->tf_raw));
                sb_puts(b, "\"");
            }
            static const char *const boxes[] = { NULL, "fill-box", "stroke-box", "content-box", "border-box" };
            if (eb->s->tf_box > 0 && eb->s->tf_box <= 4) {
                sb_puts(b, " transform-box=\"");
                sb_puts(b, boxes[eb->s->tf_box]);
                sb_puts(b, "\"");
            }
        }
    }
    if (root) {
        char t[64];
        if (!has_xmlns) sb_puts(b, " xmlns=\"http://www.w3.org/2000/svg\"");
        snprintf(t, sizeof(t), " width=\"%d\" height=\"%d\"", w, h);
        sb_puts(b, t);
        /* (what it inherits from the page around it: the style rules' inherited
         * presentation properties of its ancestors, the outermost first, then its own) */
        SvgCasc rc = { NULL, 0, 0, true, 0, { { 0 } }, { { 0 } } };
        if (t_svg_ctx) {
            int chain[64], nch = 0;
            for (int p = po_array_parent(arr, idx); p >= 0 && nch < 64; p = po_array_parent(arr, p)) {
                HtmlNode *pn = (HtmlNode *)po_array_payload(arr, p);
                if (pn && pn->type == HTML_NODE_ELEMENT) chain[nch++] = p;
            }
            for (int k = nch - 1; k >= 0; k--) css_layout_dom_cascade(t_svg_ctx, chain[k], -1, svg_casc_decl, &rc);
            css_layout_dom_cascade(t_svg_ctx, idx, -1, svg_casc_decl, &rc);
        }
        if ((root_style && root_style[0]) || rc.n) {
            sb_puts(b, " style=\"");
            if (rc.n) sb_escaped(b, rc.t, rc.n);
            if (root_style && root_style[0]) sb_escaped(b, root_style, strlen(root_style));
            sb_puts(b, "\"");
        }
        free(rc.t);
    }
    sb_puts(b, ">");
    for (int c = po_array_first_child(arr, idx); c >= 0; c = po_array_next_sibling(arr, c))
        svg_serialize(arr, c, b, false, 0, 0, NULL);
    sb_puts(b, "</");
    sb_puts(b, tag);
    sb_puts(b, ">");
}

static uint64_t svg_hash(const char *s, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 1099511628211ull; }
    return h;
}

static uint8_t to_byte(double v)
{
    v = v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v;
    return (uint8_t)lround(v * 255.0);
}

/* Paint an outermost inline <svg> into its content box: its subtree is
 * serialized as a standalone document (with the CSS fill it inherits, and
 * the text color for currentColor), rasterized at the painted size, and
 * kept in the image cache under a key naming the markup, size and colors. */
static void emit_inline_svg(LayoutContext *ctx, DisplayList *dl, const LayoutBox *box, int dom_idx, float op)
{
    int cw = (int)lround(box->width - box->s->border_left - box->s->border_right - box->padding_left - box->padding_right);
    int ch = (int)lround(box->height - box->s->border_top - box->s->border_bottom - box->padding_top - box->padding_bottom);
    if (cw <= 0 || ch <= 0 || cw > 4096 || ch > 4096) return;
    char style[96] = "";
    if (box->s->fill_kind == 1) snprintf(style, sizeof(style), "fill:currentColor");
    else if (box->s->fill_kind == 3) snprintf(style, sizeof(style), "fill:none");
    else if (box->s->fill_kind == 2)
        snprintf(style, sizeof(style), "fill:#%02x%02x%02x;fill-opacity:%.3f", to_byte(box->s->fill_r),
                 to_byte(box->s->fill_g), to_byte(box->s->fill_b), box->s->fill_a);
    uint32_t color = (uint32_t)to_byte(box->s->color_r) << 24 | (uint32_t)to_byte(box->s->color_g) << 16 |
                     (uint32_t)to_byte(box->s->color_b) << 8 | to_byte(box->s->color_a);

    SvgBuf b = { 0 };
    t_svg_ctx = ctx;
    svg_serialize(&ctx->doc->array, dom_idx, &b, true, cw, ch, style);
    t_svg_ctx = NULL;
    if (!b.s) return;
    char key[96];
    snprintf(key, sizeof(key), "cyber-svg:%016llx:%08x", (unsigned long long)svg_hash(b.s, b.n), color);
    if (image_cache_status(g_image_cache, key) != 1) {
        uint8_t *px = NULL;
        int pw = 0, ph = 0;
        t_flt_ctx = ctx;
        t_flt_color[0] = box->s->color_r; t_flt_color[1] = box->s->color_g; t_flt_color[2] = box->s->color_b; t_flt_color[3] = box->s->color_a;
        plutosvg_filter_hook = dl_svg_filter_hook;
        bool svg_ok = svg_render_rgba(b.s, b.n, cw, ch, color, &px, &pw, &ph);
        plutosvg_filter_hook = NULL;
        t_flt_ctx = NULL;
        if (svg_ok) {
            image_cache_put_rgba(g_image_cache, key, pw, ph, px);
            free(px);
        }
    }
    free(b.s);
    LayoutBox fill_box = *box;
    LayoutStyle fill_style = *box->s;
    fill_box.s = &fill_style;
    fill_style.object_fit = 0;
    emit_image_async(dl, &fill_box, key, op);
}

static bool dl_flt_element(void *ud, const char *id, int w, int h, const uint8_t **px, int *ch)
{
    LayoutContext *ctx = (LayoutContext *)ud;
    HtmlNode *n = ctx && ctx->doc ? html_document_get_element_by_id(ctx->doc, id) : NULL;
    if (!n) return false;
    int di = po_array_index_from_payload(&ctx->doc->array, n);
    if (di < 0) return false;
    SvgBuf b = { 0 };
    char head[160];
    snprintf(head, sizeof head, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%d\" height=\"%d\">", w, h);
    sb_puts(&b, head);
    LayoutContext *keep = t_svg_ctx;
    t_svg_ctx = ctx;
    svg_serialize(&ctx->doc->array, di, &b, false, (double)w, (double)h, NULL);
    t_svg_ctx = keep;
    sb_puts(&b, "</svg>");
    if (!b.s) return false;
    uint8_t *out = NULL;
    int pw = 0, ph = 0;
    plutosvg_filter_hook_t hk = plutosvg_filter_hook;
    plutosvg_filter_hook = NULL;
    bool ok = svg_render_rgba(b.s, b.n, w, h, 0x000000ff, &out, &pw, &ph);
    plutosvg_filter_hook = hk;
    free(b.s);
    if (!ok) return false;
    if (pw != w || ph != h) { free(out); return false; }
    *px = out;
    *ch = 4;
    return true;
}

/* ---- CSS masks of several layers (composites, modes, mask elements) ----
 * A mask of kind 12 heads them: nstops its layers, each a mask of kind 13 right
 * after it, the top one first.  The rasterizers fold them per pixel. */
#define MK_LAYERS 8
typedef struct {
    char image[1024], mode[24], repeat[32], position[96], clip[24], origin[24], size[64], composite[16];
} MkLayer;
/* (the top level pieces of a comma list) */
static int mk_split(const char *v, char out[][1024], int cap)
{
    int n = 0, d = 0;
    size_t l = 0;
    char q = 0;
    for (const char *p = v; p && n < cap; p++) {
        char c = *p;
        if (q) { if (c == q) q = 0; }
        else if (c == '"' || c == '\'') q = c;
        else if (c == '(') d++;
        else if (c == ')') d--;
        if (c == 0 || (c == ',' && d == 0 && !q)) {
            out[n][l] = 0;
            n++;
            l = 0;
            if (!c) break;
            continue;
        }
        if (l + 1 < 1024) out[n][l++] = c;
    }
    return n;
}
static bool mk_is_box(const char *w)
{
    static const char *const b[] = { "border-box", "padding-box", "content-box", "fill-box", "stroke-box", "view-box", "margin-box", "border", "padding", "content" };
    for (size_t k = 0; k < sizeof b / sizeof b[0]; k++) if (!strcasecmp(w, b[k])) return true;
    return false;
}
/* (a layer's piece of the shorthand) */
static void mk_shorthand(const char *t, MkLayer *L)
{
    char word[1024];
    bool slash = false;
    int nbox = 0;
    for (const char *p = t; *p;) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        size_t l = 0;
        int d = 0;
        if (*p == '/') { slash = true; p++; continue; }
        while (*p && (d > 0 || (!isspace((unsigned char)*p) && *p != '/'))) {
            if (*p == '(') d++;
            else if (*p == ')') d--;
            if (l + 1 < sizeof word) word[l++] = *p;
            p++;
        }
        word[l] = 0;
        if (!strcasecmp(word, "none") || strchr(word, '(')) {
            /* (a position's calc() is rare in this shorthand: a function is the image) */
            snprintf(L->image, sizeof L->image, "%s", word);
        }
        else if (!strcasecmp(word, "alpha") || !strcasecmp(word, "luminance") || !strcasecmp(word, "match-source")) snprintf(L->mode, sizeof L->mode, "%s", word);
        else if (!strcasecmp(word, "add") || !strcasecmp(word, "subtract") || !strcasecmp(word, "intersect") || !strcasecmp(word, "exclude")) snprintf(L->composite, sizeof L->composite, "%s", word);
        else if (!strcasecmp(word, "repeat") || !strcasecmp(word, "no-repeat") || !strcasecmp(word, "repeat-x") || !strcasecmp(word, "repeat-y") || !strcasecmp(word, "space") || !strcasecmp(word, "round")) {
            size_t rl = strlen(L->repeat);
            snprintf(L->repeat + rl, sizeof L->repeat - rl, "%s%s", rl ? " " : "", word);
        }
        else if (!strcasecmp(word, "no-clip")) snprintf(L->clip, sizeof L->clip, "no-clip");
        else if (mk_is_box(word)) {
            if (nbox == 0) { snprintf(L->origin, sizeof L->origin, "%s", word); if (!L->clip[0]) snprintf(L->clip, sizeof L->clip, "%s", word); }
            else snprintf(L->clip, sizeof L->clip, "%s", word);
            nbox++;
        }
        else if (slash) { size_t rl = strlen(L->size); snprintf(L->size + rl, sizeof L->size - rl, "%s%s", rl ? " " : "", word); }
        else { size_t rl = strlen(L->position); snprintf(L->position + rl, sizeof L->position - rl, "%s%s", rl ? " " : "", word); }
    }
}
static int mk_layers(const LayoutStyle *s, MkLayer *out)
{
    static __thread char parts[MK_LAYERS][1024];
    memset(out, 0, sizeof(MkLayer) * MK_LAYERS);
    int n = 0;
    if (s->mk_text[8]) {
        n = mk_split(s->mk_text[8], parts, MK_LAYERS);
        for (int k = 0; k < n; k++) mk_shorthand(parts[k], &out[k]);
    }
    if (s->mk_text[0]) {
        int ni = mk_split(s->mk_text[0], parts, MK_LAYERS);
        n = ni;
        for (int k = 0; k < ni; k++) { char *q = parts[k]; while (isspace((unsigned char)*q)) q++; snprintf(out[k].image, sizeof out[k].image, "%s", q); }
    }
    /* the longhands, cycled over the images */
#define MK_LONG(idx, field)                                                                                 \
    if (s->mk_text[idx]) {                                                                                  \
        int nv = mk_split(s->mk_text[idx], parts, MK_LAYERS);                                               \
        for (int k = 0; nv > 0 && k < n; k++) { char *q = parts[k % nv]; while (isspace((unsigned char)*q)) q++; snprintf(out[k].field, sizeof out[k].field, "%s", q); } \
    }
    MK_LONG(1, mode) MK_LONG(2, repeat) MK_LONG(3, position) MK_LONG(4, clip) MK_LONG(5, origin) MK_LONG(6, size) MK_LONG(7, composite)
#undef MK_LONG
    return n;
}
/* (a layered mask for this box: an image of its own that isn't none) */
static bool dl_mask_is_layered(const LayoutBox *box)
{
    const LayoutStyle *s = box->s;
    /* (one plain layer is a mask of kind 1 or 2, far cheaper on real pages: YouTube has
     * them in numbers.  Several layers, a composite or mode, a mask element: only here) */
    {
        const char *im = s->mk_text[0] ? s->mk_text[0] : s->mk_text[8];
        bool hard = !im || strchr(im, ',') != NULL || strstr(im, "url(#") != NULL || s->mk_text[7] || s->mk_text[1] || s->mk_text[4] || s->mk_text[5];
        if (im && !s->mk_text[0] && (strstr(im, "luminance") || strstr(im, "subtract") || strstr(im, "intersect") || strstr(im, "exclude"))) hard = true;
        /* (a gradient's own commas are not layers) */
        if (im && strstr(im, "gradient(") && !strstr(im, "), ") && !strstr(im, "),") && !s->mk_text[7] && !s->mk_text[1] && !s->mk_text[4] && !s->mk_text[5]) hard = false;
        if (!hard) return false;
    }
    return (s->mk_text[0] || s->mk_text[8]) && (s->mask_kind || (s->mk_text[0] && strchr(s->mk_text[0], '(')) || (s->mk_text[8] && strchr(s->mk_text[8], '(')));
}
static void mk_box_rect(const LayoutBox *b, const char *name, double dflt_border, double r[4])
{
    (void)dflt_border;
    double bl = b->s->border_left, bt = b->s->border_top, br = b->s->border_right, bb = b->s->border_bottom;
    r[0] = b->x; r[1] = b->y; r[2] = b->width; r[3] = b->height;
    if (!name || !name[0] || !strcasecmp(name, "border-box") || !strcasecmp(name, "stroke-box") || !strcasecmp(name, "view-box") || !strcasecmp(name, "border")) return;
    if (!strcasecmp(name, "margin-box")) { r[0] -= b->margin_left; r[1] -= b->margin_top; r[2] += b->margin_left + b->margin_right; r[3] += b->margin_top + b->margin_bottom; return; }
    r[0] += bl; r[1] += bt; r[2] -= bl + br; r[3] -= bt + bb;
    if (!strcasecmp(name, "padding-box") || !strcasecmp(name, "padding")) return;
    r[0] += b->padding_left; r[1] += b->padding_top; r[2] -= b->padding_left + b->padding_right; r[3] -= b->padding_top + b->padding_bottom;
}
static double mk_len(const char *w, double base, double fs)
{
    char *e;
    double d = strtod(w, &e);
    if (e == w) return 0.0;
    if (*e == '%') return d / 100.0 * base;
    if (!strncasecmp(e, "em", 2)) return d * fs;
    return d;
}
/* (background-position's grammar: one to four words: where the image's corner goes) */
static void mk_position(const char *t, double aw, double ah, double tw, double th, double fs, double *ox, double *oy)
{
    char w[4][32];
    int n = 0;
    const char *p = t;
    while (n < 4) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        size_t l = 0;
        while (*p && !isspace((unsigned char)*p)) { if (l < 31) w[n][l++] = *p; p++; }
        w[n++][l] = 0;
    }
    double x = 0.0, y = 0.0;
    double fx = aw - tw, fy = ah - th;
    #define ISX(s) (!strcasecmp(s, "left") || !strcasecmp(s, "right"))
    #define ISY(s) (!strcasecmp(s, "top") || !strcasecmp(s, "bottom"))
    #define ISC(s) (!strcasecmp(s, "center"))
    #define KWV(s, f) (!strcasecmp(s, "left") || !strcasecmp(s, "top") ? 0.0 : !strcasecmp(s, "right") || !strcasecmp(s, "bottom") ? (f) : (f) / 2.0)
    #define LENV(s, f) (s[strlen(s) - 1] == '%' ? strtod(s, NULL) / 100.0 * (f) : mk_len(s, 0.0, fs))
    if (n == 1) {
        if (ISY(w[0])) { y = KWV(w[0], fy); x = fx / 2.0; }
        else if (ISX(w[0]) || ISC(w[0])) { x = KWV(w[0], fx); y = fy / 2.0; }
        else { x = LENV(w[0], fx); y = fy / 2.0; }
    } else if (n == 2) {
        const char *a = w[0], *b = w[1];
        if (ISY(a) || ISX(b)) { const char *t2 = a; a = b; b = t2; }
        x = ISX(a) || ISC(a) ? KWV(a, fx) : LENV(a, fx);
        y = ISY(b) || ISC(b) ? KWV(b, fy) : LENV(b, fy);
    } else if (n >= 3) {
        /* (edge offset pairs) */
        int k = 0;
        for (int side = 0; side < 2 && k < n; side++) {
            const char *kw = w[k++];
            const char *off = k < n && !ISX(w[k]) && !ISY(w[k]) && !ISC(w[k]) ? w[k++] : NULL;
            bool horiz = ISX(kw) || (ISC(kw) && side == 0);
            double f = horiz ? fx : fy, v = KWV(kw, f);
            if (off) {
                double o = LENV(off, f);
                v += !strcasecmp(kw, "right") || !strcasecmp(kw, "bottom") ? -o : o;
            }
            if (horiz) x = v; else y = v;
        }
    }
    *ox = x; *oy = y;
}
/* A mask element's content over w x h (user space from the box's corner), straight RGBA. */
static uint8_t *mk_mask_element(LayoutContext *ctx, HtmlNode *mn, const LayoutBox *box, int w, int h)
{
    SvgBuf b = { 0 };
    char head[256];
    const char *cu = node_attribute_value(mn, "maskContentUnits");
    bool obb = cu && !strcasecmp(cu, "objectBoundingBox");
    {
        double kx = obb ? 1.0 / (box->width > 0 ? box->width : 1.0) : 1.0, ky = obb ? 1.0 / (box->height > 0 ? box->height : 1.0) : 1.0;
        snprintf(head, sizeof head, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%d\" height=\"%d\" viewBox=\"%f %f %f %f\">", w, h, -256.0 * kx, -256.0 * ky, w * kx, h * ky);
    }
    sb_puts(&b, head);
    LayoutContext *keep = t_svg_ctx;
    t_svg_ctx = ctx;
    for (HtmlNode *k = html_node_first_child(ctx->doc, mn); k; k = html_node_next_sibling(ctx->doc, k)) {
        if (k->type != HTML_NODE_ELEMENT) continue;
        svg_serialize(&ctx->doc->array, po_array_index_from_payload(&ctx->doc->array, k), &b, false, w, h, NULL);
    }
    t_svg_ctx = keep;
    sb_puts(&b, "</svg>");
    if (!b.s) return NULL;
    uint8_t *out = NULL;
    int pw = 0, ph = 0;
    plutosvg_filter_hook_t hk = plutosvg_filter_hook;
    plutosvg_filter_hook = NULL;
    bool ok = svg_render_rgba(b.s, b.n, w, h, 0x000000ff, &out, &pw, &ph);
    plutosvg_filter_hook = hk;
    free(b.s);
    if (!ok || pw != w || ph != h) { free(out); return NULL; }
    return out;
}
#define MK_FAR 400000.0f
static uint64_t mk_mix(uint64_t h, const void *p, size_t n)
{
    const unsigned char *b = (const unsigned char *)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}
/* (the image cache's pixels under key: their handle, -1 while there are none) */
static int mk_texture(const char *key, int w, int h)
{
    int iw = 0, ih = 0, ich = 0;
    uint8_t *px = NULL;
    int hd = image_cache_request(g_image_cache, key, w, h);
    return hd >= 0 && image_cache_get(g_image_cache, hd, &iw, &ih, &ich, &px) && px ? hd : -1;
}
/* One layer as a mask of kind 13: its image's handle (-1: transparent), the first
 * tile's place in ix.., the step from tile to tile in rx[0], rx[1] (repeat: bit 1
 * across, bit 2 down), what it is clipped to in x.., angle_deg 1 for luminance,
 * nstops its operator (1 add, 2 subtract, 3 intersect, 4 exclude). */
static void mk_layer_mask(LayoutContext *ctx, const LayoutBox *box, const MkLayer *L, DisplayMask *m)
{
    memset(m, 0, sizeof *m);
    m->kind = 13;
    m->image_handle = -1;
    m->nstops = !strcasecmp(L->composite, "subtract") ? 2 : !strcasecmp(L->composite, "intersect") ? 3 : !strcasecmp(L->composite, "exclude") ? 4 : 1;
    const char *img = L->image;
    while (isspace((unsigned char)*img)) img++;
    if (!*img || !strcasecmp(img, "none")) return;   /* (transparent) */
    double org[4], clp[4];
    mk_box_rect(box, L->origin, 1, org);
    bool no_clip = !strcasecmp(L->clip, "no-clip");
    mk_box_rect(box, L->clip[0] && !no_clip ? L->clip : "border-box", 1, clp);
    double fs = box->s->font_size > 0 ? box->s->font_size : 16.0;
    /* url(#id): a mask element, drawn once to a texture over the box and 256 px around */
    if (!strncasecmp(img, "url(", 4)) {
        const char *q = img + 4;
        while (*q == ' ' || *q == '"' || *q == '\'') q++;
        if (*q == '#') {
            char id[128];
            size_t l = 0;
            q++;
            while (*q && *q != ')' && *q != '"' && *q != '\'' && *q != ' ' && l + 1 < sizeof id) id[l++] = *q++;
            id[l] = 0;
            HtmlNode *mn = ctx->doc ? html_document_get_element_by_id(ctx->doc, id) : NULL;
            if (!mn || !mn->tag_name || strcasecmp(mn->tag_name, "mask")) return;
            int bw = (int)ceil(box->width), bh = (int)ceil(box->height);
            /* (its region: -10%, 120% of the box unless it says, in the box's terms or user space) */
            const char *mu = node_attribute_value(mn, "maskUnits");
            bool user = mu && !strcasecmp(mu, "userSpaceOnUse");
            double rx = -0.1 * box->width, ry = -0.1 * box->height, rw = 1.2 * box->width, rh = 1.2 * box->height;
            const char *v;
            #define MKR(name, var, base2) if ((v = node_attribute_value(mn, name)) != NULL) { char *e; double d = strtod(v, &e); if (e != v) var = *e == '%' ? d / 100.0 * (base2) : user ? d : d * (base2); }
            MKR("x", rx, box->width) MKR("y", ry, box->height) MKR("width", rw, box->width) MKR("height", rh, box->height)
            #undef MKR
            int mw = bw + 512, mh = bh + 512;
            if (mw > 8192 || mh > 8192 || rw <= 0.0 || rh <= 0.0) return;
            /* (the texture's name: the element's subtree and the box's size) */
            uint64_t hs = 1469598103934665603ull;
            {
                float bx[2] = { (float)box->width, (float)box->height };
                hs = mk_mix(hs, bx, sizeof bx);
                HtmlNode *walk[64];
                int w = 0;
                walk[w++] = mn;
                while (w > 0) {
                    HtmlNode *e = walk[--w];
                    if (e->tag_name) hs = mk_mix(hs, e->tag_name, strlen(e->tag_name));
                    for (HtmlAttribute *a = e->attributes; a; a = a->next) {
                        if (a->name) hs = mk_mix(hs, a->name, strlen(a->name));
                        if (a->value) hs = mk_mix(hs, a->value, strlen(a->value));
                    }
                    for (HtmlNode *c = html_node_first_child(ctx->doc, e); c; c = html_node_next_sibling(ctx->doc, c))
                        if (c->type == HTML_NODE_ELEMENT && w < 64) walk[w++] = c;
                }
            }
            char key[80];
            snprintf(key, sizeof key, "cyber-mke:%016llx:%dx%d", (unsigned long long)hs, mw, mh);
            /* (images on their way: this frame's, not to be kept; the build after they're in keeps it) */
            bool busy = image_cache_has_pending(g_image_cache);
            if (busy) { static unsigned mk_seq; size_t kl = strlen(key); snprintf(key + kl, sizeof key - kl, "p%u", ++mk_seq); }
            if (busy || image_cache_status(g_image_cache, key) != 1) {
                uint8_t *px = mk_mask_element(ctx, mn, box, mw, mh);
                if (!px) return;
                image_cache_put_rgba(g_image_cache, key, mw, mh, px);
                free(px);
            }
            const char *mt = node_attribute_value(mn, "mask-type");
            bool alpha = (mt && !strcasecmp(mt, "alpha")) || !strcasecmp(L->mode, "alpha");
            if (!strcasecmp(L->mode, "luminance")) alpha = false;
            m->image_handle = mk_texture(key, mw, mh);
            m->ix = (float)(box->x - 256.0); m->iy = (float)(box->y - 256.0); m->iw = (float)mw; m->ih = (float)mh;
            m->x = (float)(box->x + rx); m->y = (float)(box->y + ry); m->w = (float)rw; m->h = (float)rh;
            m->angle_deg = alpha ? 0.0f : 1.0f;
            return;
        }
    }
    /* the size: natural, or by mask-size */
    double aw0 = org[2], ah0 = org[3];
    double nw = 0.0, nh = 0.0, ratio = 0.0;
    bool is_grad = !strncasecmp(img, "linear-gradient", 15) || !strncasecmp(img, "radial-gradient", 15) || !strncasecmp(img, "conic-gradient", 14) || !strncasecmp(img, "repeating-", 10);
    char url[1024] = "";
    if (!is_grad) {
        if (strncasecmp(img, "url(", 4)) return;
        const char *q = img + 4;
        while (*q == ' ' || *q == '"' || *q == '\'') q++;
        size_t l = 0;
        while (*q && *q != ')' && *q != '"' && *q != '\'' && l + 1 < sizeof url) url[l++] = *q++;
        url[l] = 0;
        unsigned dims = 0;
        float rt = 0.0f;
        int iw = 0, ih = 0;
        if (image_cache_natural_size(g_image_cache, url, &iw, &ih) && iw > 0 && ih > 0) { nw = iw; nh = ih; }
        if (image_cache_intrinsic(g_image_cache, url, &dims, &rt)) {
            if ((dims & 4) && rt > 0.0f) ratio = rt;
            if (!(dims & 1)) nw = 0.0;
            if (!(dims & 2)) nh = 0.0;
        }
        if (ratio <= 0.0 && nw > 0.0 && nh > 0.0) ratio = nw / nh;
    }
    /* mask-size */
    double tw = -1.0, th = -1.0;
    {
        char a[32] = "", b[32] = "";
        sscanf(L->size, "%31s %31s", a, b);
        if (!strcasecmp(a, "cover") || !strcasecmp(a, "contain")) {
            bool cover = !strcasecmp(a, "cover");
            if (ratio <= 0.0) { tw = aw0; th = ah0; }
            else if ((aw0 / ratio > ah0) == cover) { tw = aw0; th = aw0 / ratio; }
            else { th = ah0; tw = ah0 * ratio; }
        } else {
            if (a[0] && strcasecmp(a, "auto")) tw = mk_len(a, aw0, fs);
            if (b[0] && strcasecmp(b, "auto")) th = mk_len(b, ah0, fs);
            if (tw < 0.0 && th < 0.0) {
                tw = nw > 0.0 ? nw : (nh > 0.0 && ratio > 0.0 ? nh * ratio : -1.0);
                th = nh > 0.0 ? nh : (nw > 0.0 && ratio > 0.0 ? nw / ratio : -1.0);
                if (tw < 0.0 && th < 0.0) {
                    if (ratio > 0.0) { if (aw0 / ratio > ah0) { th = ah0; tw = ah0 * ratio; } else { tw = aw0; th = aw0 / ratio; } }
                    else { tw = aw0; th = ah0; }
                }
            } else if (tw < 0.0) tw = ratio > 0.0 ? th * ratio : nw > 0.0 ? nw : aw0;
            else if (th < 0.0) th = ratio > 0.0 ? tw / ratio : nh > 0.0 ? nh : ah0;
        }
    }
    if (tw <= 0.0 || th <= 0.0) return;
    /* mask-repeat */
    int rx = 1, ry = 1;   /* 0 no, 1 repeat, 2 space, 3 round */
    {
        char a[16] = "", b[16] = "";
        sscanf(L->repeat, "%15s %15s", a, b);
        if (!strcasecmp(a, "repeat-x")) { rx = 1; ry = 0; }
        else if (!strcasecmp(a, "repeat-y")) { rx = 0; ry = 1; }
        else if (a[0]) {
            rx = !strcasecmp(a, "no-repeat") ? 0 : !strcasecmp(a, "space") ? 2 : !strcasecmp(a, "round") ? 3 : 1;
            const char *bb = b[0] ? b : a;
            ry = !strcasecmp(bb, "no-repeat") ? 0 : !strcasecmp(bb, "space") ? 2 : !strcasecmp(bb, "round") ? 3 : 1;
        }
    }
    if (rx == 3) { double c = fmax(1.0, round(aw0 / tw)); double was = tw; tw = aw0 / c; if (ry != 3 && !L->size[0] && ratio > 0.0) th = th * tw / was; }
    if (ry == 3) { double c = fmax(1.0, round(ah0 / th)); th = ah0 / c; }
    double ox = 0.0, oy = 0.0;
    mk_position(L->position[0] ? L->position : "0 0", aw0, ah0, tw, th, fs, &ox, &oy);
    double sx = tw, sy = th;
    if (rx == 2) { int c = (int)floor(aw0 / tw); if (c >= 2) { sx = (aw0 - c * tw) / (c - 1) + tw; ox = 0.0; } else rx = 0; }
    if (ry == 2) { int c = (int)floor(ah0 / th); if (c >= 2) { sy = (ah0 - c * th) / (c - 1) + th; oy = 0.0; } else ry = 0; }
    ox += org[0]; oy += org[1];
    int pw = (int)ceil(tw), ph = (int)ceil(th);
    if (pw < 1) pw = 1;
    if (ph < 1) ph = 1;
    if (pw > 8192 || ph > 8192) return;
    if (is_grad) {
        /* (a gradient: drawn once at the tile's size, kept under its text's name) */
        char key[80];
        snprintf(key, sizeof key, "cyber-mkg:%016llx:%dx%d", (unsigned long long)mk_mix(1469598103934665603ull, img, strlen(img)), pw, ph);
        if (image_cache_status(g_image_cache, key) != 1) {
            uint8_t *grad = grad_raster(img, pw, ph);
            if (!grad) return;
            image_cache_put_rgba(g_image_cache, key, pw, ph, grad);
            free(grad);
        }
        m->image_handle = mk_texture(key, pw, ph);
    } else {
        m->image_handle = mk_texture(url, pw, ph);
    }
    m->ix = (float)ox; m->iy = (float)oy; m->iw = (float)tw; m->ih = (float)th;
    m->repeat = (rx ? 1 : 0) | (ry ? 2 : 0);
    m->rx[0] = (float)sx; m->rx[1] = (float)sy;
    if (no_clip) { m->x = -MK_FAR; m->y = -MK_FAR; m->w = 2.0f * MK_FAR; m->h = 2.0f * MK_FAR; }
    else { m->x = (float)clp[0]; m->y = (float)clp[1]; m->w = (float)(clp[2] > 0.0 ? clp[2] : 0.0); m->h = (float)(clp[3] > 0.0 ? clp[3] : 0.0); }
    m->angle_deg = !strcasecmp(L->mode, "luminance") ? 1.0f : 0.0f;
}
/* The box's mask as a header and its layers: the header's index, -1 none.  (Its
 * region is everything: what the subtree paints outside every layer's clip is gone.) */
static int dl_layered_mask(LayoutContext *ctx, DisplayList *dl, const LayoutBox *box)
{
    static __thread MkLayer L[MK_LAYERS];
    if (!g_image_cache) return -1;
    int nl = mk_layers(box->s, L);
    if (nl <= 0) return -1;
    DisplayMask h;
    memset(&h, 0, sizeof h);
    h.kind = 12;
    h.image_handle = -1;
    h.x = -MK_FAR; h.y = -MK_FAR; h.w = 2.0f * MK_FAR; h.h = 2.0f * MK_FAR;
    h.nstops = nl;
    int hi = dl_add_mask(dl, &h);
    for (int k = 0; hi >= 0 && k < nl; k++) {
        DisplayMask m;
        mk_layer_mask(ctx, box, &L[k], &m);
        if (dl_add_mask(dl, &m) < 0) { dl->mask_count = hi; hi = -1; }
    }
    return hi;
}

/* ---- frames' display lists, by their <iframe> ---- */
typedef struct { uint32_t node; DisplayList dl; } DlFrameEntry;
static DlFrameEntry g_dl_frames[64];
static int g_dl_nframes;
void display_list_frames_clear(void)
{
    for (int k = 0; k < g_dl_nframes; k++) display_list_free(&g_dl_frames[k].dl);
    g_dl_nframes = 0;
}
void display_list_frame_set(uint32_t node, const DisplayList *src)
{
    if (!src || g_dl_nframes >= 64) return;
    DlFrameEntry *e = &g_dl_frames[g_dl_nframes];
    display_list_init(&e->dl);
    e->node = node;
    if (src->count > 0 && !display_list_reserve(&e->dl, src->count)) return;
    if (src->count > 0) memcpy(e->dl.cmds, src->cmds, (size_t)src->count * sizeof(DisplayListCmd));
    e->dl.count = src->count;
    for (int k = 0; k < src->mask_count; k++) dl_add_mask(&e->dl, &src->masks[k]);
    g_dl_nframes++;
}
static bool dl_frame_has(uint32_t node)
{
    for (int k = 0; k < g_dl_nframes; k++) if (g_dl_frames[k].node == node) return true;
    return false;
}
/* (the frame's commands at the iframe's content box, within it) */
static void dl_emit_frame(DisplayList *dl, const LayoutBox *box, uint32_t node)
{
    const DlFrameEntry *e = NULL;
    for (int k = 0; k < g_dl_nframes && !e; k++) if (g_dl_frames[k].node == node) e = &g_dl_frames[k];
    if (!e || !e->dl.count) return;
    float ox = (float)(box->x + box->s->border_left + box->padding_left), oy = (float)(box->y + box->s->border_top + box->padding_top);
    int from = dl->count, mbase = dl->mask_count;
    for (int k = 0; k < e->dl.mask_count; k++) {
        DisplayMask m = e->dl.masks[k];
        m.x += ox; m.y += oy;
        if (m.kind == 1 || m.iw > 0.0f) { m.ix += ox; m.iy += oy; }
        dl_add_mask(dl, &m);
    }
    if (!display_list_reserve(dl, e->dl.count)) return;
    for (int k = 0; k < e->dl.count; k++) {
        DisplayListCmd c = e->dl.cmds[k];
        if (c.type == DL_MASK_BEGIN || c.type == DL_MASK_END) c.u.mask_index += mbase;
        if (c.type == DL_XFORM) {
            /* (about the frame's origin: p -> M(p - o) + o) */
            float *m = c.u.xf;
            m[4] += ox - (m[0] * ox + m[2] * oy);
            m[5] += oy - (m[1] * ox + m[3] * oy);
        } else {
            c.x += ox; c.y += oy;
            if (c.type == DL_BORDER) { c.u.border.x += ox; c.u.border.y += oy; }
        }
        dl->cmds[dl->count++] = c;
    }
    float clip[4] = { ox, oy, ox + (float)box->content_width, oy + (float)box->content_height };
    dl_clip_range(dl, from, clip);
}

static bool dl_cv_applies(const LayoutContext *ctx, int i);
static bool inside_svg(PreOrderCompactionArray *arr, int dom_idx)
{
    for (int p = po_array_parent(arr, dom_idx); p >= 0; p = po_array_parent(arr, p)) {
        HtmlNode *n = (HtmlNode *)po_array_payload(arr, p);
        if (n && n->type == HTML_NODE_ELEMENT && n->tag_name && strcasecmp(n->tag_name, "svg") == 0) return true;
    }
    return false;
}

/* (the attribute an image-showing element takes its image from: <object>'s
 * data, <embed>'s src, a source-less <video>'s poster; NULL none) */
static const char *dl_image_attr(const HtmlNode *node)
{
    if (!node || node->type != HTML_NODE_ELEMENT || !node->tag_name) return NULL;
    if (!strcasecmp(node->tag_name, "object")) return "data";
    if (!strcasecmp(node->tag_name, "embed")) return "src";
    if (!strcasecmp(node->tag_name, "video")) {
        const char *src = node_attribute_value((HtmlNode *)node, "src");
        if (src && src[0]) return NULL;
        for (NodeId c = node->store_id ? dom_first_child(node->store_id) : 0; c; c = dom_next_sibling(c))
            if (dom_kind(c) == DOM_K_ELEMENT && !strcmp(css_atom_str(dom_tag(c)), "source")) return NULL;
        return "poster";
    }
    return NULL;
}

bool display_list_object_image(const HtmlNode *node)
{
    if (node && node->type == HTML_NODE_ELEMENT && node->content_img) return true;   /* (replaced by content: url()) */
    const char *attr = dl_image_attr(node);
    if (!attr) return false;
    const char *data = node_attribute_value((HtmlNode *)node, attr);
    if (data && data[0] && attr[0] == 'p') return true;   /* (a poster is an image) */
    if (!data || !data[0]) return false;
    const char *type = node_attribute_value((HtmlNode *)node, "type");
    if (type && type[0]) return strncasecmp(type, "image/", 6) == 0;
    const char *dot = strrchr(data, '.');
    static const char *const ext[] = { ".png", ".jpg", ".jpeg", ".gif", ".svg", ".webp", ".bmp", NULL };
    for (int i = 0; dot && ext[i]; i++)
        if (!strcasecmp(dot, ext[i])) return true;
    return false;
}

static double g_media_vw = 1024.0, g_media_vh = 768.0;

void display_list_set_media_viewport(double vw, double vh)
{
    g_media_vw = vw;
    g_media_vh = vh;
}

const char *display_list_picture_srcset(uint32_t img)
{
    NodeId p = dom_parent(img);
    const char *pt = p && dom_kind(p) == DOM_K_ELEMENT ? css_atom_str(dom_tag(p)) : NULL;
    if (!pt || strcmp(pt, "picture")) return NULL;
    for (NodeId c = dom_first_child(p); c && c != img; c = dom_next_sibling(c)) {
        if (dom_kind(c) != DOM_K_ELEMENT || strcmp(css_atom_str(dom_tag(c)), "source")) continue;
        const char *ss = dom_get_attr_str(c, "srcset");
        if (!ss || !ss[0]) continue;
        const char *media = dom_get_attr_str(c, "media");
        if (media && media[0] && !css_media_query_matches(media, g_media_vw, g_media_vh)) continue;
        const char *type = dom_get_attr_str(c, "type");
        if (type && type[0] && strncasecmp(type, "image/", 6)) continue;
        return ss;
    }
    return NULL;
}

char *display_list_noexif(char *url, const char *base_url)
{
    if (!url) return NULL;
    /* (another origin's image keeps its orientation) */
    if (!strncmp(url, "http", 4) && base_url) {
        const char *h = strstr(base_url, "://");
        const char *e = h ? strchr(h + 3, '/') : NULL;
        size_t ol = e ? (size_t)(e - base_url) : (base_url ? strlen(base_url) : 0);
        if (!ol || strncmp(url, base_url, ol)) return url;
    }
    size_t n = strlen(url);
    char *o = (char *)malloc(n + 8);
    if (!o) return url;
    memcpy(o, "noexif:", 7);
    memcpy(o + 7, url, n + 1);
    free(url);
    return o;
}

char *display_list_image_source(const char *base_url, HtmlNode *node, float css_width)
{
    if (!node || node->type != HTML_NODE_ELEMENT) return NULL;
    if (node->content_img) return strdup(node->content_img);
    const char *src = node_attribute_value(node, display_list_object_image(node) ? dl_image_attr(node) : "src");
    /* srcset, when present, overrides src with the candidate that best
     * matches the laid-out width (or the 1x candidate); a <picture>'s
     * <source> overrides both. */
    const char *pic = node->store_id ? display_list_picture_srcset(node->store_id) : NULL;
    const char *srcset = pic ? pic : node_attribute_value(node, "srcset");
    char *picked = srcset ? srcset_pick(srcset, css_width) : NULL;
    if (picked) src = picked;
    char *url = NULL;
    if (src && src[0]) {
        /* Resolve against the document URL (relative paths need its
         * directory, not just the origin layout carries). */
        extern const char *g_cyber_start_url;
        const char *doc_url = (g_cyber_start_url && g_cyber_start_url[0]) ? g_cyber_start_url : base_url;
        url = (doc_url && doc_url[0]) ? wurl_resolve(doc_url, src) : NULL;
        if (!url) url = dl_resolve_url(base_url, src);
    }
    free(picked);
    return url;
}

char *display_list_pick_srcset(const char *srcset, float css_width)
{
    return srcset_pick(srcset, css_width);
}

/* Emit underline / line-through rects for the glyph commands emitted since
 * `from_cmd`.  Glyphs are grouped into lines by y proximity so wrapped runs
 * get one decoration segment per line. */
static void emit_text_decorations(DisplayList *dl, int from_cmd, double fs,
                                  unsigned char deco, float r, float g, float b, float a)
{
    if (!deco || from_cmd >= dl->count) return;
    float tol = (float)(fs * 0.5);
    if (tol < 1.0f) tol = 1.0f;
    /* Collect line extents first (add_rect may realloc dl->cmds). */
    typedef struct { float y, x0, x1; } DecoLine;
    int cap = 16, n = 0;
    DecoLine *lines = (DecoLine *)malloc((size_t)cap * sizeof(DecoLine));
    if (!lines) return;
    for (int i = from_cmd; i < dl->count; i++) {
        DisplayListCmd *c = &dl->cmds[i];
        if (c->type != DL_GLYPH) continue;
        float ly = c->y;
        int found = -1;
        for (int k = 0; k < n; k++) {
            if (fabsf(lines[k].y - ly) <= tol) { found = k; break; }
        }
        if (found < 0) {
            if (n >= cap) {
                cap *= 2;
                DecoLine *nl = (DecoLine *)realloc(lines, (size_t)cap * sizeof(DecoLine));
                if (!nl) { free(lines); return; }
                lines = nl;
            }
            found = n++;
            lines[found].y = ly;
            lines[found].x0 = c->x;
            lines[found].x1 = c->x + c->w;
        } else {
            if (c->x < lines[found].x0) lines[found].x0 = c->x;
            if (c->x + c->w > lines[found].x1) lines[found].x1 = c->x + c->w;
        }
    }
    for (int pass = 0; pass < 2; pass++) {
        float yoff, thick;
        if (pass == 0) {
            if (!(deco & 1)) continue; /* underline */
            yoff = (float)(fs * 0.92);
        } else {
            if (!(deco & 2)) continue; /* line-through */
            yoff = (float)(fs * 0.32);
        }
        thick = (float)(fs / 14.0);
        if (thick < 1.0f) thick = 1.0f;
        for (int k = 0; k < n; k++) {
            float w = lines[k].x1 - lines[k].x0;
            if (w <= 0.0f) continue;
            display_list_add_rect(dl, lines[k].x0, lines[k].y + yoff, w, thick,
                                  r, g, b, a);
        }
    }
    free(lines);
}

/* (one decoration line from x0 to x1, its top at y, t thick, in its style) */
static void dl_deco_line(DisplayList *dl, float x0, float x1, float y, float t, int style, const float c[4])
{
    if (x1 <= x0 || c[3] <= 0.0f) return;
    if (style == 1) {   /* (double: two lines a thickness apart) */
        display_list_add_rect(dl, x0, y, x1 - x0, t, c[0], c[1], c[2], c[3]);
        display_list_add_rect(dl, x0, y + 2.0f * t, x1 - x0, t, c[0], c[1], c[2], c[3]);
    } else if (style == 2 || style == 3) {   /* (dotted: squares; dashed: three thicknesses long) */
        float on = style == 2 ? t : 3.0f * t, step = on + (style == 2 ? t : 2.0f * t);
        if (step < 1.0f) step = 1.0f;
        /* (in phase across the page, so pieces of one line join up) */
        for (float x = floorf(x0 / step) * step; x < x1; x += step) {
            float l = x < x0 ? x0 : x, r = x + on < x1 ? x + on : x1;
            if (r > l) display_list_add_rect(dl, l, y, r - l, t, c[0], c[1], c[2], c[3]);
        }
    } else if (style == 4) {   /* (wavy: a sine a thickness and a half high, eight thicknesses long) */
        float amp = 1.5f * t, len = 8.0f * t;
        if (len < 4.0f) len = 4.0f;
        for (float x = x0; x < x1; x += 1.0f) {
            float yy = y + amp + amp * sinf(x * 6.2831853f / len);
            display_list_add_rect(dl, x, yy, (x + 1.0f < x1 ? 1.0f : x1 - x), t, c[0], c[1], c[2], c[3]);
        }
    } else {
        display_list_add_rect(dl, x0, y, x1 - x0, t, c[0], c[1], c[2], c[3]);
    }
}

/* (no text box in [a, b) with fragments: nothing of a box painted there) */
static bool dl_no_text_between(const LayoutContext *ctx, int a, int b)
{
    for (int j = a; j < b; j++)
        if (ctx->text_frags && j < ctx->text_frags_n && ctx->text_frags[j].n > 0) return false;
    return true;
}

/* Text decorations (CSS Text Decoration) on fragment q of nq of text box i,
 * [x, x + w) with its baseline at by: the lines its decorating ancestors
 * propagate to it (not out of floats, absolutely positioned or atomic
 * inline boxes), each in that box's color, style and thickness; the text's
 * own font places them.  The innermost box's last; pass 0 the underlines
 * and overlines (under the text), 1 the line-throughs (over it). */
/* (text-decoration-skip-ink: the under- and overline rects [a, b) cut where
 * the glyphs [ga, gb) drawn after them reach into them, a gap either side) */
static void dl_skip_ink(DisplayList *dl, int a, int b, int ga, int gb)
{
    for (int k = a; k < b; k++) {
        if (dl->cmds[k].type != DL_RECT) continue;
        float rx = dl->cmds[k].x, ry = dl->cmds[k].y, rw = dl->cmds[k].w, rh = dl->cmds[k].h;
        if (rw <= 0.0f || rh <= 0.0f) continue;
        float pad = rh > 1.0f ? rh : 1.0f;
        float cut[64][2];
        int nc = 0;
        for (int g = ga; g < gb && nc < 64; g++) {
            const DisplayListCmd *gc = &dl->cmds[g];
            if (gc->type != DL_GLYPH) continue;
            if (gc->y >= ry + rh - 0.5f || gc->y + gc->h <= ry + 0.5f) continue;
            if (gc->x >= rx + rw || gc->x + gc->w <= rx) continue;
            cut[nc][0] = gc->x - pad;
            cut[nc][1] = gc->x + gc->w + pad;
            nc++;
        }
        if (!nc) continue;
        /* (the cuts in order, overlaps merged) */
        for (int u = 1; u < nc; u++)
            for (int v = u; v > 0 && cut[v][0] < cut[v - 1][0]; v--) { float t0 = cut[v][0], t1 = cut[v][1]; cut[v][0] = cut[v - 1][0]; cut[v][1] = cut[v - 1][1]; cut[v - 1][0] = t0; cut[v - 1][1] = t1; }
        float cr = dl->cmds[k].r, cg = dl->cmds[k].g, cbl = dl->cmds[k].b, ca = dl->cmds[k].a;
        float x0 = rx, end = rx + rw;
        bool first = true;
        for (int u = 0; u <= nc; u++) {
            float x1 = u < nc ? cut[u][0] : end;
            if (x1 > end) x1 = end;
            if (x1 > x0 + 0.01f) {
                if (first) { dl->cmds[k].x = x0; dl->cmds[k].w = x1 - x0; first = false; }
                else display_list_add_rect(dl, x0, ry, x1 - x0, rh, cr, cg, cbl, ca);
            }
            if (u < nc && cut[u][1] > x0) x0 = cut[u][1];
        }
        if (first) dl->cmds[k].w = 0.0f;
    }
}

static void dl_text_decorations(DisplayList *dl, const LayoutContext *ctx, int i, int q, int nq, const TextShaper *font,
                                double fs, float x, float w, float by, float op, int pass)
{
    if (fs < 0.5) return;
    int recs[8], nr = 0;
    const LayoutStyle *last = NULL;
    for (int c = i, p = ctx->tree.nodes[i].parent_idx; p >= 0; c = p, p = ctx->tree.nodes[p].parent_idx) {
        const LayoutStyle *cs = ctx->boxes[c].s;
        if (c != i && (cs->float_side || cs->position == CSS_POSITION_ABSOLUTE || cs->position == CSS_POSITION_FIXED ||
                       cs->display == CSS_DISPLAY_INLINE_BLOCK || cs->inline_level)) break;
        const LayoutStyle *ps = ctx->boxes[p].s;
        if (ps->text_decoration_set && ps->text_decoration && ps != last && nr < 8) { recs[nr++] = p; last = ps; }
    }
    if (!nr) return;
    float a0 = 0.8f, d0 = 0.2f, g0 = 0.0f;
    text_shaper_vmetrics(font, &a0, &d0, &g0);
    float ul_pos = 0.1f, ul_th = 0.05f, st_pos = 0.3f, st_th = 0.05f;
    text_shaper_deco_metrics(font, &ul_pos, &ul_th, &st_pos, &st_th);
    const LayoutStyle *ts = ctx->boxes[i].s;
    for (int k = nr - 1; k >= 0; k--) {
        int o = recs[k];
        const LayoutStyle *rs = ctx->boxes[o].s;
        float c[4];
        if (rs->td_cset) {
            c[0] = (float)(rs->td_rgba >> 24) / 255.0f; c[1] = (float)((rs->td_rgba >> 16) & 255) / 255.0f;
            c[2] = (float)((rs->td_rgba >> 8) & 255) / 255.0f; c[3] = (float)(rs->td_rgba & 255) / 255.0f;
        } else {
            c[0] = (float)rs->color_r; c[1] = (float)rs->color_g; c[2] = (float)rs->color_b; c[3] = (float)rs->color_a;
        }
        c[3] *= op;
        /* (em and % thickness of the font size before font-size-adjust) */
        double rfs = rs->fs_unadj > 0.0 ? rs->fs_unadj : rs->font_size > 0.0 ? rs->font_size : fs;
        float t = rs->td_tk >= 2 ? rs->td_t + (float)(rs->td_te * rfs) : (float)(ul_th * fs);
        t = roundf(t);
        if (t < 1.0f) t = 1.0f;
        float x0 = x, x1 = x + w;
        /* (the inset at the decorating box's own start and end) */
        if ((rs->td_in[0] != 0.0f || rs->td_in[1] != 0.0f || rs->td_in_auto || rs->td_in_cpct[0] != 0.0f || rs->td_in_cpct[1] != 0.0f) && o < i) {
            float in0 = rs->td_in_auto ? (float)(rfs / 16.0) : rs->td_in[0], in1 = rs->td_in_auto ? (float)(rfs / 16.0) : rs->td_in[1];
            int end = o;
            while (end >= 0 && ctx->tree.nodes[end].next_sibling_idx < 0) end = ctx->tree.nodes[end].parent_idx;
            end = end >= 0 ? ctx->tree.nodes[end].next_sibling_idx : ctx->tree.count;
            /* (a percentage of the decorating box's length: sliced, all its lines' together; cloned, this line's) */
            float base = w;
            if (((rs->td_in_pct & 3) || rs->td_in_cpct[0] != 0.0f || rs->td_in_cpct[1] != 0.0f) && !rs->bdb_clone && ctx->text_frags) {
                double tot = 0.0;
                for (int j = o + 1; j < end && j < ctx->text_frags_n; j++)
                    for (int fq = 0; fq < ctx->text_frags[j].n; fq++) tot += ctx->text_frags[j].f[fq].w;
                if (tot > 0.0) base = (float)tot;
            }
            if (rs->td_in_pct & 1) in0 *= base;
            if (rs->td_in_pct & 2) in1 *= base;
            in0 += rs->td_in_cpct[0] * base;   /* (a calc()'s percentage part) */
            in1 += rs->td_in_cpct[1] * base;
            bool at0 = q == 0 && dl_no_text_between(ctx, o + 1, i), at1 = q == nq - 1 && end > i && dl_no_text_between(ctx, i + 1, end);
            if (rs->bdb_clone && ctx->text_frags && i < ctx->text_frags_n && ctx->text_frags[i].n == nq) {
                /* (cloned: at each line's start and end) */
                const LayoutTextFrag *f = ctx->text_frags[i].f;
                if (q > 0 && fabs(f[q].dy - f[q - 1].dy) > 0.5) at0 = true;
                if (q < nq - 1 && fabs(f[q + 1].dy - f[q].dy) > 0.5) at1 = true;
            }
            if (rs->direction == 1) {   /* (rtl: the start is the right end) */
                if (at0) x1 -= in0;
                if (at1) x0 += in1;
            } else {
                if (at0) x0 += in0;
                if (at1) x1 -= in1;
            }
        }
        unsigned char lines = rs->text_decoration & (pass ? 2 : 5);
        /* (the decorating box's baseline: the text's, less the shifts of the inline boxes between) */
        float by0 = by;
        for (int c = ctx->tree.nodes[i].parent_idx; c >= 0 && c != o; c = ctx->tree.nodes[c].parent_idx) {
            const LayoutStyle *cs = ctx->boxes[c].s;
            if (cs->display != CSS_DISPLAY_INLINE || cs->vertical_align != 8) continue;
            double lh = cs->line_height > 0.0 ? cs->line_height : cs->font_size * 1.2;
            by += (float)(cs->va_len + cs->va_pct * lh);
        }
        if (lines & 1) {
            float off = ts->ul_k ? ts->ul_v + (float)(ts->ul_e * fs) : 0.0f;
            float y = ts->ul_under ? by + d0 * (float)fs + off : by + (ts->ul_k ? off : ul_pos * (float)fs);
            dl_deco_line(dl, x0, x1, roundf(y), t, rs->td_style, c);
        }
        if (lines & 4) dl_deco_line(dl, x0, x1, roundf(by - a0 * (float)fs), t, rs->td_style, c);
        if (lines & 2) dl_deco_line(dl, x0, x1, roundf(by - st_pos * (float)fs), t, rs->td_style, c);
        by = by0;
    }
}

/* Return true if the document is in a dark theme.
 * This is used to make default-black text visible on dark backgrounds. */
static bool document_is_dark_mode(LayoutContext *ctx)
{
    if (!ctx || !ctx->doc) return false;
    for (size_t i = 0; i < ctx->doc->array.count; i++) {
        HtmlNode *node = (HtmlNode *)po_array_payload(&ctx->doc->array, i);
        if (!node || node->type != HTML_NODE_ELEMENT) continue;
        if (strcasecmp(node->tag_name, "html") == 0 ||
            strcasecmp(node->tag_name, "body") == 0) {
            /* Only the exact `dark` attribute marks YouTube's dark theme.  The
             * legacy `darker-dark-theme` attribute is present on the light
             * theme as well (deprecated), so it must not be used here. */
            if (node_attribute_value(node, "dark") != NULL) return true;
            static const char *dark_needles[] = {"dark-mode", "theme-dark", NULL};
            if (node_class_contains_any(node, dark_needles)) return true;
        }
    }
    return false;
}

/* Clip the commands [from, dl->count) to rect c (x0,y0,x1,y1): quads are
 * cropped (texture coordinates follow), fully clipped ones collapse. */
/* (a glyph or image cut from (x0, y0, w, h) to (nx0, ny0)-(nx1, ny1): its part of the texture, turned or not) */
static void dl_uv_cut(DisplayListCmd *cmd, float *u0, float *v0, float *u1, float *v1, float x0, float y0, float nx0, float ny0, float nx1, float ny1)
{
    float du = *u1 - *u0, dv = *v1 - *v0;
    float fx0 = (nx0 - x0) / cmd->w, fx1 = (nx1 - x0) / cmd->w, fy0 = (ny0 - y0) / cmd->h, fy1 = (ny1 - y0) / cmd->h;
    int rot = cmd->type == DL_GLYPH ? cmd->u.glyph.rot : 0;
    if (rot == 1) {   /* (u down, v from the right) */
        float nu0 = *u0 + du * fy0, nu1 = *u0 + du * fy1, nv0 = *v0 + dv * (1.0f - fx1), nv1 = *v0 + dv * (1.0f - fx0);
        *u0 = nu0; *u1 = nu1; *v0 = nv0; *v1 = nv1;
    } else if (rot == 2) {   /* (u up, v from the left) */
        float nu0 = *u0 + du * (1.0f - fy1), nu1 = *u0 + du * (1.0f - fy0), nv0 = *v0 + dv * fx0, nv1 = *v0 + dv * fx1;
        *u0 = nu0; *u1 = nu1; *v0 = nv0; *v1 = nv1;
    } else {
        float nu0 = *u0 + du * fx0, nu1 = *u0 + du * fx1, nv0 = *v0 + dv * fy0, nv1 = *v0 + dv * fy1;
        *u0 = nu0; *u1 = nu1; *v0 = nv0; *v1 = nv1;
    }
}

static void dl_clip_range(DisplayList *dl, int from, const float c[4])
{
    for (int k = from; k < dl->count; k++) {
        DisplayListCmd *cmd = &dl->cmds[k];
        /* (a filter's output is clipped too, not only what goes into it: in ex_rx) */
        if (cmd->type == DL_MASK_BEGIN) {
            DisplayMask *fm = &dl->masks[cmd->u.mask_index];
            if (fm->kind == 9 || fm->kind == 10) {
                bool had = fm->ex_rx[2] > fm->ex_rx[0] || fm->ex_rx[3] > fm->ex_rx[1];
                fm->ex_rx[0] = had && fm->ex_rx[0] > c[0] ? fm->ex_rx[0] : c[0];
                fm->ex_rx[1] = had && fm->ex_rx[1] > c[1] ? fm->ex_rx[1] : c[1];
                fm->ex_rx[2] = had && fm->ex_rx[2] < c[2] ? fm->ex_rx[2] : c[2];
                fm->ex_rx[3] = had && fm->ex_rx[3] < c[3] ? fm->ex_rx[3] : c[3];
                if (fm->ex_rx[2] <= fm->ex_rx[0]) fm->ex_rx[2] = fm->ex_rx[0] + 0.001f;
                if (fm->ex_rx[3] <= fm->ex_rx[1]) fm->ex_rx[3] = fm->ex_rx[1] + 0.001f;
            }
            continue;
        }
        if (cmd->type == DL_MASK_END) continue;
        float x0 = cmd->x, y0 = cmd->y, x1 = cmd->x + cmd->w, y1 = cmd->y + cmd->h;
        if (x0 >= c[0] && y0 >= c[1] && x1 <= c[2] && y1 <= c[3]) continue;
        float nx0 = x0 > c[0] ? x0 : c[0], ny0 = y0 > c[1] ? y0 : c[1];
        float nx1 = x1 < c[2] ? x1 : c[2], ny1 = y1 < c[3] ? y1 : c[3];
        if (nx1 <= nx0 || ny1 <= ny0) {
            cmd->w = 0.0f; cmd->h = 0.0f; cmd->a = 0.0f;
            if (cmd->type == DL_BORDER) { cmd->u.border.w = 0.0f; cmd->u.border.h = 0.0f; }
            continue;
        }
        float *u0 = NULL, *v0 = NULL, *u1 = NULL, *v1 = NULL;
        if (cmd->type == DL_GLYPH) { u0 = &cmd->u.glyph.u0; v0 = &cmd->u.glyph.v0; u1 = &cmd->u.glyph.u1; v1 = &cmd->u.glyph.v1; }
        else if (cmd->type == DL_IMAGE) { u0 = &cmd->u.image.u0; v0 = &cmd->u.image.v0; u1 = &cmd->u.image.u1; v1 = &cmd->u.image.v1; }
        else if (cmd->type == DL_BORDER) continue;   /* outlines: keep whole */
        if (u0 && cmd->w > 0.0f && cmd->h > 0.0f) dl_uv_cut(cmd, u0, v0, u1, v1, x0, y0, nx0, ny0, nx1, ny1);
        cmd->x = nx0; cmd->y = ny0; cmd->w = nx1 - nx0; cmd->h = ny1 - ny0;
    }
}

/* ---- Columns ----
 * A multi-column container's content is laid out as one column (css_layout.c
 * layout_multicol): what each box in it paints is cut into column-high
 * strips, each moved into its column (the column's slice of what crosses
 * from one to the next, as box-decoration-break: slice does). */

/* (the multi-column container a box is in, -1: none; *child: its child the box is in) */
static int mc_owner(const LayoutContext *ctx, int i, int *child)
{
    int below = i, spanner = -1;
    bool fixed_seen = false;
    /* (an out-of-flow box whose containing block is the container or above it is
     * outside its fragmentation: not cut into columns) */
    bool oof = false;
    for (int p = ctx->tree.nodes[i].parent_idx, q = i, d = 0; p >= 0 && d < 1024; q = p, p = ctx->tree.nodes[p].parent_idx, d++) {
        const LayoutStyle *qs = ctx->boxes[q].s;
        if (qs->position == CSS_POSITION_ABSOLUTE || qs->position == CSS_POSITION_FIXED) oof = true;
        else if (oof && qs->position != CSS_POSITION_STATIC) oof = false;   /* (its containing block inside) */
        if (qs->position == CSS_POSITION_FIXED) fixed_seen = true;
        if (ctx->boxes[q].mc_span && spanner < 0) spanner = q;   /* (in a spanner, however deep: where it is) */
        if (ctx->boxes[p].mc_n > 0) {
            /* (rows of columns: their markers over the whole; print: only what is fixed stays out of the pages) */
            if (oof && ctx->boxes[p].mc_rgap >= 0.0f && (!g_css_print.on || fixed_seen)) return -1;
            *child = spanner >= 0 ? spanner : below;
            return p;
        }
        below = p;
    }
    return -1;
}

/* (one command cut to c (x0, y0, x1, y1): false when nothing is left) */
static bool dl_clip_cmd(DisplayListCmd *cmd, const float c[4])
{
    float x0 = cmd->x, y0 = cmd->y, x1 = cmd->x + cmd->w, y1 = cmd->y + cmd->h;
    if (x0 >= c[0] && y0 >= c[1] && x1 <= c[2] && y1 <= c[3]) return true;
    float nx0 = x0 > c[0] ? x0 : c[0], ny0 = y0 > c[1] ? y0 : c[1];
    float nx1 = x1 < c[2] ? x1 : c[2], ny1 = y1 < c[3] ? y1 : c[3];
    if (nx1 <= nx0 || ny1 <= ny0) return false;
    float *u0 = NULL, *v0 = NULL, *u1 = NULL, *v1 = NULL;
    if (cmd->type == DL_GLYPH) { u0 = &cmd->u.glyph.u0; v0 = &cmd->u.glyph.v0; u1 = &cmd->u.glyph.u1; v1 = &cmd->u.glyph.v1; }
    else if (cmd->type == DL_IMAGE) { u0 = &cmd->u.image.u0; v0 = &cmd->u.image.v0; u1 = &cmd->u.image.u1; v1 = &cmd->u.image.v1; }
    else if (cmd->type == DL_BORDER) return true;
    if (u0 && cmd->w > 0.0f && cmd->h > 0.0f) dl_uv_cut(cmd, u0, v0, u1, v1, x0, y0, nx0, ny0, nx1, ny1);
    cmd->x = nx0; cmd->y = ny0; cmd->w = nx1 - nx0; cmd->h = ny1 - ny0;
    return true;
}

/* Gap decorations (CSS Gaps 1): the value of a rule list for gap gi of N
 * (n values, a repeat(auto, ...) run at [as, as + an): the values before it
 * the first gaps', those after it the last gaps', it the ones between;
 * none: they cycle).  -1: the list is the initial one. */
static int gap_pick(int n, int as, int an, int gi, int N)
{
    if (n <= 0) return -1;
    if (an <= 0) return gi % n;
    int lead = as, trail = n - as - an;
    if (gi < lead) return gi;
    if (gi >= N - trail && trail > 0) {
        int k = as + an + (gi - (N - trail));
        return k < n ? k : n - 1;
    }
    return as + (gi - lead) % an;
}

/* (a rule's style, width and color for gap gi of N in axis a; false: none painted) */
static bool gap_rule_of(const LayoutStyle *s, int a, int gi, int N, float op, int *style, float *w, float c[4])
{
    int k = gap_pick(s->gr[a].ns, s->gr[a].sas, s->gr[a].san, gi, N);
    *style = k < 0 ? CSS_BORDER_NONE : s->gr[a].style[k];
    k = gap_pick(s->gr[a].nw, s->gr[a].was, s->gr[a].wan, gi, N);
    *w = k < 0 ? (s->zoom > 0.0f ? 3.0f * s->zoom : 3.0f) : s->gr[a].w[k];
    k = gap_pick(s->gr[a].nc, s->gr[a].cas, s->gr[a].can, gi, N);
    if (k >= 0 && s->gr[a].cset[k]) {
        uint32_t v = s->gr[a].rgba[k];
        c[0] = (float)((v >> 24) & 0xFF) / 255.0f;
        c[1] = (float)((v >> 16) & 0xFF) / 255.0f;
        c[2] = (float)((v >> 8) & 0xFF) / 255.0f;
        float al = (float)(v & 0xFF) / 255.0f, ah = roundf(al * 100.0f) / 100.0f;
        if (fabsf(al - ah) < 0.0021f) al = ah;   /* (a byte of alpha back to the hundredth it was written as) */
        c[3] = al * op;
    } else {
        c[0] = (float)s->color_r; c[1] = (float)s->color_g; c[2] = (float)s->color_b; c[3] = (float)s->color_a * op;
    }
    return *style != CSS_BORDER_NONE && *style != CSS_BORDER_HIDDEN && *w > 0.0f;
}

/* One rule's piece: across x, y, w, h in its style (as a box's one border side). */
static void gap_rule_piece(DisplayList *dl, const LayoutBox *box, int axis, int style, float x, float y, float w, float h,
                           const float c[4])
{
    /* (on whole pixels, as a border's edges are) */
    float x1 = roundf(x + w), y1 = roundf(y + h);
    x = roundf(x); y = roundf(y);
    w = x1 - x; h = y1 - y;
    if (w <= 0.0f || h <= 0.0f) return;
    /* (as Blink paints rules: inset as ridge, outset as groove; a column
     * rule a left border, a row rule a bottom one) */
    if (style == CSS_BORDER_INSET) style = CSS_BORDER_RIDGE;
    else if (style == CSS_BORDER_OUTSET) style = CSS_BORDER_GROOVE;
    if (style == CSS_BORDER_SOLID) { display_list_add_rect(dl, x, y, w, h, c[0], c[1], c[2], c[3]); return; }
    LayoutStyle rs = *box->s;
    LayoutBox rb = *box;
    rb.s = &rs;
    for (int k = 0; k < 4; k++) rs.border_style[k] = CSS_BORDER_NONE;
    float col[4][4] = { { 0 } };
    float bwid[4] = { 0.0f, 0.0f, 0.0f, 0.0f };   /* (top, right, bottom, left) */
    if (axis == 0) { rs.border_style[0] = (unsigned char)style; memcpy(col[0], c, 4 * sizeof(float)); bwid[3] = w; }
    else { rs.border_style[3] = (unsigned char)style; memcpy(col[3], c, 4 * sizeof(float)); bwid[2] = h; }
    paint_border_sides(dl, &rb, x, y, w, h, (const float (*)[4])col, bwid);
}

/* A grid's or flex container's gap decorations, column rules then row rules
 * (or the other way, column-over-row): each gap's segments joined as its
 * break says (none: all of it; normal: through crossings, ending at Ts;
 * intersection: through spanning items only; never over an item across
 * it), the painted ones as visibility-items says, each end moved by its
 * cap or junction inset. */
/* (whether the crossing gap ci of axis ca has its rule painted next to pos
 * along it: a segment of it that ends or starts there, shown by its
 * visibility; true when that can't be told) */
static bool gap_cross_on(const CssGapLine *lines, int nl, const CssGapSeg *segs, int ca, int ci, float pos, int vis)
{
    if (ci == -2) return true;   /* (a crossing gap of no index: a lane's own) */
    if (ci < 0) return false;
    if (vis == 1) return true;
    for (int i = 0; i < nl; i++) {
        const CssGapLine *L = &lines[i];
        if (L->axis != ca || L->gi != ci) continue;
        const CssGapSeg *G = segs + L->at;
        bool any = false;
        for (int k = 0; k < L->n; k++) {
            /* (the segments on either side of the junction at pos) */
            float lo = G[k].s - 1.0f, hi = G[k].e + 1.0f;
            float gapw = G[k].jw > 0.0f ? G[k].jw : 0.0f;
            if (pos < lo - gapw - 1.0f || pos > hi + gapw + 1.0f) continue;
            any = true;
            if (G[k].disc) continue;
            if ((vis == 2 && G[k].occ) || (vis == 3 && G[k].occ == 3)) return true;
        }
        if (any) return false;   /* (else: another stretch of that gap) */
    }
    return true;
}

static int mc_owner(const LayoutContext *ctx, int i, int *child);
static void mc_rule_piece(DisplayList *dl, const LayoutBox *mb, int axis, int style, float x, float y, float w, float h, const float c[4]);
/* (kind 2: a multi-column container's rows of columns, ox and oy its columns' origin along their axes) */
static void paint_gap_lines(const LayoutContext *ctx, DisplayList *dl, const LayoutBox *box, int idx, float op, unsigned char kind,
                            const CssGapLine *lines, int nl, const CssGapSeg *segs, float ox, float oy)
{
    const LayoutStyle *s = box->s;
    /* (a vertical flow's: its geometry along the flow's axes from the content corner, turned as painted) */
    int wv = kind & 0x10 ? (s->writing_mode == 1 || s->writing_mode == 3 ? 2 : 1) : 0;
    kind &= 0xF;
    int N[2] = { 0, 0 };
    for (int i = 0; i < nl; i++) N[lines[i].axis]++;
    for (int pass = 0; pass < 2; pass++) {
        int a = s->gr_overlap ? 1 - pass : pass;
        for (int i = 0; i < nl; i++) {
            const CssGapLine *L = &lines[i];
            if (L->axis != a) continue;
            int style;
            float rw, c[4];
            if (!gap_rule_of(s, a, L->gi, L->nn ? L->nn : N[a], op, &style, &rw, c)) continue;
            /* (a row gap split by a column break, or the last thing before one: no rule, CSS Gaps 1) */
            if (a == 1 && kind != 2 && !wv) {
                int mch_ = -1, mo_ = mc_owner(ctx, idx, &mch_);
                if (mo_ >= 0 && !ctx->boxes[mo_].mc_vert) {
                    const LayoutBox *mb_ = &ctx->boxes[mo_], *cb_ = &ctx->boxes[mch_];
                    double H_ = cb_->mc_seg_h > 0.0f ? cb_->mc_seg_h : mb_->mc_h;
                    double top_ = cb_->mc_seg_h > 0.0f ? cb_->mc_src : mb_->y + mb_->padding_top + mb_->s->border_top;
                    double g0 = oy + L->pos - L->w * 0.5 - top_, g1 = oy + L->pos + L->w * 0.5 - top_;
                    if (H_ > 1.0) {
                        double f1 = fmod(g1, H_);
                        if (floor((g0 + 0.01) / H_) != floor((g1 - 0.01) / H_) || (g1 > 0.5 && (f1 < 0.5 || f1 > H_ - 0.5))) continue;
                    }
                }
            }
            int brk = s->gr[a].brk;
            if (kind == 1 && brk == 0) brk = 1;            /* (flex: normal is none) */
            if (kind == 2 && brk == 0) brk = a == 0 ? 2 : 1;   /* (columns: each row its own rules, a row gap's all the way) */
            int vis = kind == 1 ? 1 : s->gr[a].vis;
            if (vis == 0) vis = kind == 2 && a == 0 ? 3 : 1;   /* (column rules: between columns with content) */
            const CssGapSeg *G = segs + L->at;
            int n = L->n;
            /* (the crossing rule's width, for overlap-join) */
            #define CROSS_W(ci) ({ int st_; float w_, c_[4]; \
                (ci) >= 0 ? (gap_rule_of(s, 1 - a, (ci), N[1 - a], op, &st_, &w_, c_), w_) : 0.0f; })   /* (its width, painted or not) */
            float done = -1e30f;   /* (how far this gap's rule is painted: pieces joined over a crossing cover it once) */
            for (int d0 = 0; d0 < n;) {
                int d1 = d0;
                if (brk == 1) d1 = n - 1;
                else {
                    if (G[d0].disc) { d0++; continue; }
                    while (d1 + 1 < n && !G[d1 + 1].disc &&
                           !(brk == 2 && G[d1].jt != 0))   /* (normal: broken only where an item spans it) */
                        d1++;
                }
                /* The painted runs of [d0, d1]. */
                for (int p = d0; p <= d1;) {
                    bool on = vis == 1 || (vis == 2 && G[p].occ) || (vis == 3 && G[p].occ == 3);
                    if (!on) { p++; continue; }
                    int q = p;
                    while (q + 1 <= d1 && (vis == 1 || (vis == 2 && G[q + 1].occ) || (vis == 3 && G[q + 1].occ == 3))) q++;
                    float st = G[p].s, en = G[q].e;
                    /* (start: a cap at the gap's start, else the junction before) */
                    float jw = p == 0 ? L->jw0 : G[p - 1].jw;
                    int ci = p == 0 ? L->ci0 : G[p - 1].ci;
                    bool junc = p > 0 || L->jw0 > 0.0f;
                    {
                        int cvis0 = kind == 1 ? 1 : s->gr[1 - a].vis;
                        if (cvis0 == 0) cvis0 = 1;
                        if (junc && !gap_cross_on(lines, nl, segs, 1 - a, ci, L->pos, cvis0)) junc = false;   /* (a cap: its percentage still of the crossing gap) */
                    }
                    /* (right to left: a row gap's start is at its far end as laid out) */
                    int rv = a == 1 && s->direction == 1;
                    const __typeof__(s->gr[a].inset[0]) *in = &s->gr[a].inset[(junc ? 2 : 0) + rv];
                    bool ojs = in->oj && junc;
                    if (in->oj) st -= junc ? jw * 0.5f + CROSS_W(ci) * 0.5f : 0.0f;
                    else st += in->px + in->pct * jw;
                    jw = q == n - 1 ? L->jw1 : G[q].jw;
                    ci = q == n - 1 ? L->ci1 : G[q].ci;
                    junc = q < n - 1 || L->jw1 > 0.0f;
                    {
                        /* (a junction whose crossing rule isn't painted there ends it as a cap) */
                        int cvis = kind == 1 ? 1 : s->gr[1 - a].vis;
                        if (cvis == 0) cvis = 1;
                        if (junc && !gap_cross_on(lines, nl, segs, 1 - a, ci, L->pos, cvis)) junc = false;
                    }
                    in = &s->gr[a].inset[(junc ? 3 : 1) - rv];
                    if (in->oj) en += junc ? jw * 0.5f + CROSS_W(ci) * 0.5f : 0.0f;
                    else en -= in->px + in->pct * jw;
                    /* (a flex or multicol row gap's pieces joined over a crossing: it's covered once) */
                    if (ojs && a == 1 && kind != 0 && st < done) st = done;
                    done = en;
                    if (en > st) {
                        float across = L->pos - rw * 0.5f;
                        if (wv) {
                            float x_ = a == 0 ? across : st, y_ = a == 0 ? st : across, w_ = a == 0 ? rw : en - st, h_ = a == 0 ? en - st : rw;
                            float cl_ = (float)(box->x + box->padding_left + s->border_left), ct_ = (float)(box->y + box->padding_top + s->border_top);
                            gap_rule_piece(dl, box, 1 - a, style, wv == 1 ? cl_ + y_ : cl_ + (float)box->content_width - y_ - h_,
                                           s->writing_mode == 4 ? ct_ + (float)box->content_height - x_ - w_ : ct_ + x_, h_, w_, c);
                        } else if (kind == 2) {
                            if (a == 0) mc_rule_piece(dl, box, 0, style, ox + across, oy + st, rw, en - st, c);
                            else mc_rule_piece(dl, box, 1, style, ox + st, oy + across, en - st, rw, c);
                        } else if (a == 0) gap_rule_piece(dl, box, 0, style, ox + across, oy + st, rw, en - st, c);
                        else gap_rule_piece(dl, box, 1, style, ox + st, oy + across, en - st, rw, c);
                    }
                    p = q + 1;
                }
                d0 = d1 + 1;
            }
            #undef CROSS_W
        }
    }
}

static void paint_gap_rules(const LayoutContext *ctx, DisplayList *dl, const LayoutBox *box, int idx, float op)
{
    unsigned char kind = 0;
    const CssGapLine *lines;
    const CssGapSeg *segs;
    int nl = css_layout_gap_lines(idx, &kind, &lines, &segs);
    if (nl <= 0) return;
    if (box->s->writing_mode != 0) { paint_gap_lines(ctx, dl, box, idx, op, kind | 0x10, lines, nl, segs, 0.0f, 0.0f); return; }
    paint_gap_lines(ctx, dl, box, idx, op, kind, lines, nl, segs, (float)(box->x + box->padding_left + box->s->border_left),
                    (float)(box->y + box->padding_top + box->s->border_top));
}

/* A multi-column container's column rules (CSS Multicol 1 4): in the middle
 * of each gap between columns with content, as high as the columns, in the
 * rule's style (a left border of that width, painted as one). */
/* (a piece of a rule given along the column's axes: in a vertical flow, x
 * along the inline axis and y along the block axis, from the content start) */
static void mc_rule_piece(DisplayList *dl, const LayoutBox *mb, int axis, int style, float x, float y, float w, float h, const float c[4])
{
    if (!mb->mc_vert) { gap_rule_piece(dl, mb, axis, style, x, y, w, h, c); return; }
    float cl = (float)(mb->x + mb->padding_left + mb->s->border_left), ct = (float)(mb->y + mb->padding_top + mb->s->border_top);
    float px = mb->mc_vert == 1 ? cl + y : cl + (float)mb->content_width - y - h;
    gap_rule_piece(dl, mb, 1 - axis, style, px, ct + x, h, w, c);
}

static void paint_column_rules(const LayoutContext *ctx, int idx, DisplayList *dl, const LayoutBox *mb, float op)
{
    const LayoutStyle *s = mb->s;
    if (mb->mc_n <= 0 || mb->mc_used < 1) return;
    int vm = mb->mc_vert;
    float cl0 = (float)(mb->x + mb->padding_left + s->border_left);
    float left = vm ? 0.0f : cl0;
    float top = vm ? 0.0f : (float)(mb->y + mb->padding_top + s->border_top);
    bool rtl = (s->direction == 1) != (vm && s->writing_mode == 4);
    float U = vm ? (float)mb->content_height : (float)mb->content_width, W = mb->mc_w, G = mb->mc_gap, H = mb->mc_h;
    bool wrap = mb->mc_rgap >= 0.0f;
    /* (runs by place in the one column) */
    if (mb->mc_nseg > 0) {
        int Nr = mb->mc_n - 1 > 0 ? mb->mc_n - 1 : 0;
        /* (rows of columns between spanners: each run of rows a grid of gaps) */
        const LayoutMcSeg *SG = &ctx->mc_segs[mb->mc_seg0];
        int ns_ = mb->mc_nseg, per_ = mb->mc_n, nrow = 0;
        for (int sg = 1; sg < ns_; sg++) if (SG[sg].rgap >= 0.0f) nrow++;
        if (nrow > 0 && per_ <= 32 && ns_ <= 256 && !vm) {
            CssGapLine *L = (CssGapLine *)calloc((size_t)ns_ * (size_t)per_ + 1, sizeof *L);
            CssGapSeg *Sg = (CssGapSeg *)calloc((size_t)ns_ * (size_t)per_ * 2 + 1, sizeof *Sg);
            int nl = 0, ns = 0, ri = 0;
            for (int a = 0; L && Sg && a < ns_;) {
                int b = a;
                while (b + 1 < ns_ && SG[b + 1].rgap >= 0.0f) b++;
                /* (column-rule-break: none: one rule through the runs, behind the spanners) */
                int ca = a, cb = b;
                if (s->gr[0].brk == 1) { ca = 0; cb = a == 0 ? ns_ - 1 : -1; }
                for (int g = 0; g + 1 < per_ && cb >= ca; g++) {
                    CssGapLine *l = &L[nl++];
                    float mid = (g + 1) * (W + G) - G * 0.5f;
                    l->axis = 0; l->gi = g; l->nn = Nr; l->pos = rtl ? U - mid : mid; l->w = G; l->ci0 = l->ci1 = -1; l->at = ns; l->n = cb - ca + 1;
                    for (int r = ca, rj = ri; r <= cb; r++) {
                        CssGapSeg *q = &Sg[ns++];
                        bool jn = r < cb && SG[r + 1].rgap >= 0.0f;
                        q->s = SG[r].dst - top; q->e = q->s + SG[r].h; q->jw = jn ? SG[r + 1].rgap : 0.0f; q->ci = jn ? rj++ : -1; q->jt = 2;
                        q->occ = (unsigned char)((g < SG[r].used ? 1 : 0) | (g + 1 < SG[r].used ? 2 : 0));
                    }
                }
                for (int r = a; r < b; r++) {
                    CssGapLine *l = &L[nl++];
                    float rgp = SG[r + 1].rgap;
                    l->axis = 1; l->gi = ri + (r - a); l->nn = nrow; l->pos = SG[r].dst + SG[r].h + rgp * 0.5f - top; l->w = rgp; l->ci0 = l->ci1 = -1; l->at = ns; l->n = per_;
                    for (int k = 0; k < per_; k++) {
                        int col = rtl ? per_ - 1 - k : k;
                        CssGapSeg *q = &Sg[ns++];
                        q->s = rtl ? U - (col + 1) * (W + G) + G : col * (W + G); q->e = q->s + W;
                        q->jw = k < per_ - 1 ? G : 0.0f; q->ci = k < per_ - 1 ? (rtl ? per_ - 2 - k : k) : -1; q->jt = 2;
                        q->occ = (unsigned char)((col < SG[r].used ? 1 : 0) | (col < SG[r + 1].used ? 2 : 0));
                    }
                }
                ri += b - a;
                a = b + 1;
            }
            if (L && Sg) paint_gap_lines(ctx, dl, mb, idx, op, 2, L, nl, Sg, left, top);
            free(L);
            free(Sg);
            return;
        }
        for (int sg = 0; sg < mb->mc_nseg; sg++) {
            const LayoutMcSeg *S = &ctx->mc_segs[mb->mc_seg0 + sg];
            float base = vm ? S->dst - (float)(mb->y + mb->padding_top + s->border_top) * 0.0f : S->dst;
            for (int g = 0; g + 1 < S->used; g++) {
                int style;
                float rw, c[4];
                if (!gap_rule_of(s, 0, g, Nr, op, &style, &rw, c)) continue;
                float mid = (g + 1) * (W + G) - G * 0.5f;
                if (rtl) mid = U - mid;
                mc_rule_piece(dl, mb, 0, style, left + mid - rw * 0.5f, base, rw, S->h, c);
            }
        }
        return;
    }
    /* (spanners: each run of columns between them its rules, as tall as it is,
     * between the columns it has content in) */
    if (!wrap) {
        bool any_span = false;
        for (int c = ctx->tree.nodes[idx].first_child_idx; c >= 0 && !any_span; c = ctx->tree.nodes[c].next_sibling_idx)
            any_span = ctx->boxes[c].mc_span && ctx->boxes[c].s->display != CSS_DISPLAY_NONE;
        if (any_span) {
            int k = ctx->tree.nodes[idx].first_child_idx;
            /* (the runs: where each is painted, how tall, its columns with content, those overflowing too) */
            enum { MR = 64, MG = 63 };
            float rd[MR], rh[MR];
            int rc[MR], nr = 0, maxc = 0;
            while (k >= 0 && nr < MR) {
                if (ctx->boxes[k].mc_span) { k = ctx->tree.nodes[k].next_sibling_idx; continue; }
                float dst = ctx->boxes[k].mc_dst, src = ctx->boxes[k].mc_src, h = ctx->boxes[k].mc_seg_h, most = src;
                for (; k >= 0 && !ctx->boxes[k].mc_span; k = ctx->tree.nodes[k].next_sibling_idx) {
                    const LayoutBox *kb = &ctx->boxes[k];
                    if (kb->s->display == CSS_DISPLAY_NONE || kb->s->position == CSS_POSITION_ABSOLUTE || kb->s->position == CSS_POSITION_FIXED) continue;
                    float e = vm == 1 ? (float)(kb->x + kb->width) - cl0 : vm == 2 ? cl0 + (float)mb->content_width - (float)kb->x : (float)(kb->y + kb->height);
                    if (e > most) most = e;
                }
                if (h <= 0.5f || most <= src + 0.01f) continue;
                int used = (int)ceilf((most - src - 0.01f) / h);
                /* (the last run of a container of a height, balanced: its columns to the content edge, every one) */
                if (k < 0 && s->height_set && s->column_fill == 0 && used < mb->mc_n && s->gr[0].vis == 0) used = mb->mc_n;
                if (k < 0 && s->height_set && s->column_fill == 0 && !vm && top + (float)mb->content_height - dst > h) h = top + (float)mb->content_height - dst;
                if (used > maxc) maxc = used;
                rd[nr] = dst; rh[nr] = h; rc[nr] = used; nr++;
            }
            /* (each gap a line through the runs: broken where a spanner crosses it, the overflowing columns' not) */
            if (s->gr[0].vis == 1 && s->column_count > 0 && maxc < mb->mc_n) maxc = mb->mc_n;   /* (all: a count's every gap) */
            int ng = maxc - 1 < MG ? maxc - 1 : MG;
            if (ng > 0 && nr > 0) {
                CssGapLine *L = (CssGapLine *)calloc((size_t)ng, sizeof *L);
                CssGapSeg *Sg = (CssGapSeg *)calloc((size_t)ng * (size_t)nr * 2, sizeof *Sg);
                int ns = 0;
                for (int g = 0; L && Sg && g < ng; g++) {
                    CssGapLine *l = &L[g];
                    float mid = (g + 1) * (W + G) - G * 0.5f;
                    l->axis = 0; l->gi = g; l->nn = mb->mc_n - 1 > 0 ? mb->mc_n - 1 : 0; l->pos = rtl ? U - mid : mid; l->w = G; l->ci0 = l->ci1 = -1; l->at = ns; l->n = 0;
                    for (int r = 0; r < nr; r++) {
                        CssGapSeg *q = &Sg[ns++];
                        /* (a rule through the spanners: over each it's wanted, the spanner on both its sides) */
                        if (r > 0 && s->gr[0].brk == 1 && g + 1 < mb->mc_n && rd[r] > rd[r - 1] + rh[r - 1]) {
                            q->s = rd[r - 1] + rh[r - 1] - top; q->e = rd[r] - top; q->ci = -1; q->jt = 2; q->occ = 3;
                            q = &Sg[ns++];
                            l->n++;
                        }
                        l->n++;
                        q->s = rd[r] - top; q->e = q->s + rh[r]; q->jw = 0.0f; q->ci = -1; q->jt = g + 1 < mb->mc_n ? 2 : 0;
                        q->occ = (unsigned char)((g < rc[r] ? 1 : 0) | (g + 1 < rc[r] ? 2 : 0));
                    }
                }
                if (L && Sg) paint_gap_lines(ctx, dl, mb, idx, op, 2, L, ng, Sg, left, top);
                free(L);
                free(Sg);
            }
            return;
        }
    }
    int per = wrap ? mb->mc_n : mb->mc_used;      /* (columns in a row) */
    int used = mb->mc_used;
    if (!wrap && s->gr[0].vis == 1 && per < mb->mc_n) per = used = mb->mc_n;   /* (rule-visibility-items: all, the empty columns' gaps too) */
    int rows = wrap ? (used + per - 1) / per : 1;
    float RG = wrap ? mb->mc_rgap : 0.0f;
    int Nc = per - 1 > 0 ? per - 1 : 0;
    /* (rows of columns: a grid of them, its gaps' segments by cell) */
    if (wrap && rows > 1 && per <= 32 && rows <= 64) {
        enum { ML = 96, MS = 32 * 64 * 2 };
        CssGapLine *L = (CssGapLine *)calloc(ML, sizeof *L);
        CssGapSeg *Sg = (CssGapSeg *)calloc(MS, sizeof *Sg);
        int nl = 0, ns = 0;
        if (L && Sg) {
            for (int g = 0; g + 1 < per; g++) {
                CssGapLine *l = &L[nl++];
                float mid = (g + 1) * (W + G) - G * 0.5f;
                l->axis = 0; l->gi = g; l->pos = rtl ? U - mid : mid; l->w = G; l->ci0 = l->ci1 = -1; l->at = ns; l->n = rows;
                for (int r = 0; r < rows; r++) {
                    CssGapSeg *q = &Sg[ns++];
                    q->s = r * (H + RG); q->e = q->s + H; q->jw = r < rows - 1 ? RG : 0.0f; q->ci = r < rows - 1 ? r : -1; q->jt = 2;
                    q->occ = (unsigned char)((r * per + g < used ? 1 : 0) | (r * per + g + 1 < used ? 2 : 0));
                }
            }
            for (int r = 0; r + 1 < rows; r++) {
                CssGapLine *l = &L[nl++];
                l->axis = 1; l->gi = r; l->pos = (r + 1) * (H + RG) - RG * 0.5f; l->w = RG; l->ci0 = l->ci1 = -1; l->at = ns; l->n = per;
                for (int k = 0; k < per; k++) {
                    int col = rtl ? per - 1 - k : k;
                    CssGapSeg *q = &Sg[ns++];
                    q->s = rtl ? U - (col + 1) * (W + G) + G : col * (W + G); q->e = q->s + W;
                    q->jw = k < per - 1 ? G : 0.0f; q->ci = k < per - 1 ? (rtl ? per - 2 - k : k) : -1; q->jt = 2;
                    q->occ = (unsigned char)((r * per + col < used ? 1 : 0) | ((r + 1) * per + col < used ? 2 : 0));
                }
            }
            paint_gap_lines(ctx, dl, mb, idx, op, 2, L, nl, Sg, left, top);
        }
        free(L);
        free(Sg);
        return;
    }
    /* (rules between columns with content: in each row, each its row's height) */
    for (int pass = 0; pass < 2; pass++) {
        int a = s->gr_overlap ? 1 - pass : pass;
        if (a == 0) {
            for (int r = 0; r < rows; r++) {
                int incol = r == rows - 1 ? used - r * per : per;
                for (int g = 0; g + 1 < incol; g++) {
                    int style;
                    float rw, c[4];
                    if (!gap_rule_of(s, 0, g, Nc, op, &style, &rw, c)) continue;
                    float mid = (g + 1) * (W + G) - G * 0.5f;
                    if (rtl) mid = U - mid;
                    float st = r * (H + RG), en = st + H;
                    /* (ends: caps at the container's edges, junctions at row gaps) */
                    bool j0 = r > 0, j1 = r < rows - 1;
                    const __typeof__(s->gr[0].inset[0]) *in = &s->gr[0].inset[j0 ? 2 : 0];
                    st += in->oj ? (j0 ? -RG * 0.5f : 0.0f) : in->px + in->pct * (j0 ? RG : 0.0f);
                    in = &s->gr[0].inset[j1 ? 3 : 1];
                    en -= in->oj ? (j1 ? -RG * 0.5f : 0.0f) : in->px + in->pct * (j1 ? RG : 0.0f);
                    if (en > st) mc_rule_piece(dl, mb, 0, style, left + mid - rw * 0.5f, top + st, rw, en - st, c);
                }
            }
        } else {
            for (int r = 0; r + 1 < rows; r++) {
                int style;
                float rw, c[4];
                if (!gap_rule_of(s, 1, r, rows - 1, op, &style, &rw, c)) continue;
                float mid = (r + 1) * (H + RG) - RG * 0.5f;
                float st = 0.0f, en = U;
                const __typeof__(s->gr[1].inset[0]) *in = &s->gr[1].inset[0];
                if (!in->oj) st += in->px;
                in = &s->gr[1].inset[1];
                if (!in->oj) en -= in->px;
                if (en > st) mc_rule_piece(dl, mb, 1, style, left + st, top + mid - rw * 0.5f, en - st, rw, c);
            }
        }
    }
}

/* box-decoration-break: clone in columns: what [from, count) painted, of box
 * i, its decorations again at each break when i is the box (a border band
 * each side of it), its content moved into the fragments past them when i is
 * in it. */
static void dl_bdb_clone(const LayoutContext *ctx, DisplayList *dl, int from, int i, float op)
{
    int a = -1;
    for (int p = i, d = 0; p >= 0 && d < 256; p = ctx->tree.nodes[p].parent_idx, d++) {
        if (p != i && ctx->boxes[p].mc_n > 0) break;
        if (ctx->boxes[p].bdb_n > 0 && ctx->boxes[p].s->bdb_clone) { a = p; break; }
    }
    if (a < 0 || dl->count <= from) return;
    int mch = -1, mo = mc_owner(ctx, a, &mch);
    if (mo < 0) return;
    const LayoutBox *mb = &ctx->boxes[mo], *chb = &ctx->boxes[mch], *A = &ctx->boxes[a];
    float top = (float)(mb->y + mb->padding_top + mb->s->border_top), H = mb->mc_h;
    if (chb->mc_seg_h > 0.0f) { top = chb->mc_src; H = chb->mc_seg_h; }
    if (H <= 1.0f) return;
    float bt = (float)A->s->border_top, bb = (float)A->s->border_bottom;
    float dt = bt + (float)A->padding_top, db = (float)A->padding_bottom + bb;
    float B1 = top + (floorf(((float)A->y - top) / H + 1e-4f) + 1.0f) * H;
    int n = A->bdb_n;
    if (a == i) {
        for (int j = 0; j < n; j++) {
            float Bj = B1 + j * H, c[4];
            if (bb > 0.0f && A->s->border_style[3]) { border_side_color(A, 3, op, c); display_list_add_rect(dl, (float)A->x, Bj - bb, (float)A->width, bb, c[0], c[1], c[2], c[3]); }
            if (bt > 0.0f && A->s->border_style[2]) { border_side_color(A, 2, op, c); display_list_add_rect(dl, (float)A->x, Bj, (float)A->width, bt, c[0], c[1], c[2], c[3]); }
        }
        return;
    }
    float S0 = (float)A->y + dt, c1 = B1 - db - S0, ck = H - dt - db;
    if (ck <= 0.5f) return;
    int n0 = dl->count - from;
    DisplayListCmd *src = (DisplayListCmd *)malloc((size_t)n0 * sizeof(DisplayListCmd));
    if (!src) return;
    memcpy(src, dl->cmds + from, (size_t)n0 * sizeof(DisplayListCmd));
    dl->count = from;
    for (int j = 0; j <= n; j++) {
        float lo = j == 0 ? -1e9f : S0 + c1 + (j - 1) * ck, hi = j == n ? 1e9f : S0 + c1 + j * ck;
        float clipr[4] = { -1e9f, lo, 1e9f, hi }, dy = j * (dt + db);
        if (!display_list_reserve(dl, n0)) break;
        for (int k = 0; k < n0; k++) {
            DisplayListCmd cmd = src[k];
            if (!dl_clip_cmd(&cmd, clipr)) continue;
            cmd.y += dy;
            if (cmd.type == DL_BORDER) cmd.u.border.y += dy;
            dl->cmds[dl->count++] = cmd;
        }
    }
    free(src);
}

/* What [from, count) painted, in container m's columns (the run of child's). */
/* (the transform xf with what it applies to moved by (dx, dy)) */
static void xf_moved(float out[6], const float xf[6], float dx, float dy)
{
    memcpy(out, xf, 6 * sizeof(float));
    out[4] = xf[4] + dx - (xf[0] * dx + xf[2] * dy);
    out[5] = xf[5] + dy - (xf[1] * dx + xf[3] * dy);
}

static bool dl_put_xform(DisplayList *dl, const float xf[6])
{
    if (!display_list_reserve(dl, 1)) return false;
    DisplayListCmd *xc = &dl->cmds[dl->count++];
    memset(xc, 0, sizeof *xc);
    xc->type = DL_XFORM;
    memcpy(xc->u.xf, xf, 6 * sizeof(float));
    return true;
}

static int dl_add_mask(DisplayList *dl, const DisplayMask *m);

/* (a copy of mask mi moved by (dx, dy): its index) */
static int dl_mask_moved(DisplayList *dl, int mi, float dx, float dy)
{
    /* (a layered mask's layers, a polygon's further corners: after it there, and here) */
    int n = 1, first = -1;
    if (dl->masks[mi].kind == 12) n += dl->masks[mi].nstops;
    else if (dl->masks[mi].kind == 7) while (mi + n < dl->mask_count && dl->masks[mi + n].kind == 11) n++;
    if (mi + n > dl->mask_count) n = dl->mask_count - mi;
    for (int k = 0; k < n; k++) {
        DisplayMask m = dl->masks[mi + k];
        m.x += dx;
        m.y += dy;
        if (m.kind != 9) { m.ix += dx; m.iy += dy; }   /* (a filter's: a drop-shadow's offsets) */
        int ni = dl_add_mask(dl, &m);
        if (ni < 0) return -1;
        if (!k) first = ni;
    }
    return first;
}

static __thread int t_seg_pick = -1;
static __thread int t_mc_box = -1;   /* (the box whose painting is being put in columns) */
static __thread bool t_mc_flat;   /* (no transform of the page is in three dimensions) */
static void dl_columns_range(const LayoutContext *ctx, DisplayList *dl, int from, int m, int child, const float *xf0)
{
    const LayoutBox *chb = &ctx->boxes[child];
    if (chb->mc_span) return;
    int n0 = dl->count - from;
    if (n0 <= 0) return;
    /* (masks, shadows and transforms move with the column they are in; a mask
     * that opens or closes outside the range: left whole) */
    bool has_x = false;
    float xf_end[6] = { 1, 0, 0, 1, 0, 0 };
    if (xf0) memcpy(xf_end, xf0, sizeof xf_end);
    {
        int bal = 0;
        for (int k = from; k < dl->count; k++) {
            DisplayListCmdType t = dl->cmds[k].type;
            if (t == DL_MASK_BEGIN) bal++;
            else if (t == DL_MASK_END) { if (--bal < 0) return; }
            else if (t == DL_XFORM) { has_x = true; memcpy(xf_end, dl->cmds[k].u.xf, sizeof xf_end); }
        }
        if (bal) return;
    }
    bool xf_linear = xf0 && (xf0[0] != 1.0f || xf0[1] != 0.0f || xf0[2] != 0.0f || xf0[3] != 1.0f);
    bool need_x = has_x || xf_linear;
    const LayoutBox *mb = &ctx->boxes[m];
    float left = (float)(mb->x + mb->padding_left + mb->s->border_left);
    float top = (float)(mb->y + mb->padding_top + mb->s->border_top);
    float W = mb->mc_w, G = mb->mc_gap, H = mb->mc_h, dst = top;
    float seg_end = 1e9f;
    /* (runs by place in the one column: each has the part of the range in it) */
    if (mb->mc_nseg > 0 && t_seg_pick < 0) {
        DisplayListCmd *all = (DisplayListCmd *)malloc((size_t)n0 * sizeof(DisplayListCmd));
        if (!all) return;
        memcpy(all, dl->cmds + from, (size_t)n0 * sizeof(DisplayListCmd));
        dl->count = from;
        for (int sg = 0; sg < mb->mc_nseg; sg++) {
            int f2 = dl->count;
            const LayoutMcSeg *S = &ctx->mc_segs[mb->mc_seg0 + sg];
            if (S->h <= 0.5f) continue;
            if (!display_list_reserve(dl, n0)) break;
            for (int k = 0; k < n0; k++) {
                DisplayListCmd cmd = all[k];
                if (cmd.type != DL_MASK_BEGIN && cmd.type != DL_MASK_END && cmd.type != DL_XFORM) {
                    float cl_[4] = { -1e9f, sg == 0 ? -1e9f : S->src, 1e9f, sg == mb->mc_nseg - 1 ? 1e9f : S->end };
                    if (cmd.type == DL_SHADOW ? (cmd.y >= cl_[3] || cmd.y + cmd.h <= cl_[1]) : !dl_clip_cmd(&cmd, cl_)) continue;
                }
                dl->cmds[dl->count++] = cmd;
            }
            t_seg_pick = sg;
            dl_columns_range(ctx, dl, f2, m, child, xf0);
            t_seg_pick = -1;
        }
        free(all);
        return;
    }
    /* (a vertical flow: the block axis along x, the inline axis along y; below,
     * top, dst and H are block offsets from the content start) */
    int vm = mb->mc_vert;
    float cl = left, cr = left + (float)mb->content_width, ctop = top;
    if (vm) { dst = top = 0.0f; }
    bool first_run = true, last_run = true;
    if (mb->mc_nseg > 0 && t_seg_pick >= 0) {
        const LayoutMcSeg *S = &ctx->mc_segs[mb->mc_seg0 + t_seg_pick];
        first_run = t_seg_pick == 0;
        last_run = t_seg_pick == mb->mc_nseg - 1;
        dst = S->dst;
        top = S->src;
        H = S->h;
        seg_end = S->end;
    } else if (chb->mc_seg_h > 0.0f) {
        first_run = chb->mc_src <= top + 0.01f;
        last_run = chb->mc_seg_last;
        dst = chb->mc_dst;
        top = chb->mc_src;
        H = chb->mc_seg_h;
    }
    (void)seg_end;
    if (H <= 0.5f) return;
    /* (in what can't break and hangs over its column's end: whole in that
     * column, in no other; a box around such: longer there by what went with it) */
    int hang_col = -1, next = 0;
    int ext_col[16];
    float ext_v[16];
    if (!vm && t_mc_box >= 0 && t_mc_box != m) {
        int hb = -1;
        bool ours = false;
        for (int p = t_mc_box, d = 0; p >= 0 && d < 1024; p = ctx->tree.nodes[p].parent_idx, d++) {
            if (p == m) { ours = true; break; }
            if (ctx->boxes[p].mc_n > 0) break;
            if (ctx->boxes[p].mc_hang == 1 || ctx->boxes[p].mc_hang == 3) hb = p;
        }
        if (ours && hb >= 0) {
            hang_col = (int)floorf(((float)ctx->boxes[hb].y - top + 0.01f) / H);
            /* (nothing of it, at a column's end: the last thing in that column) */
            if (ctx->boxes[hb].height <= 0.0 && hang_col > 0 && (float)ctx->boxes[hb].y - top - hang_col * H < 0.02f) hang_col--;
        }
        else if (ours && ctx->boxes[t_mc_box].mc_hang == 2) {
            float rem = ctx->boxes[t_mc_box].mc_cut;
            int q = ctx->tree.nodes[t_mc_box].first_child_idx;
            for (int guard = 0; q >= 0 && guard < 100000 && next < 16 && rem > 0.0f; guard++) {
                const LayoutBox *qb = &ctx->boxes[q];
                bool is = qb->mc_hang == 1;
                if (is) {
                    int hc = (int)floorf(((float)qb->y - top + 0.01f) / H);
                    float o = (float)(qb->y + qb->height) - (top + (hc + 1) * H);
                    if (o > rem) o = rem;
                    if (o > 0.0f) { ext_col[next] = hc; ext_v[next] = o; next++; rem -= o; }
                }
                if (!is && qb->mc_hang == 2 && ctx->tree.nodes[q].first_child_idx >= 0) { q = ctx->tree.nodes[q].first_child_idx; continue; }
                while (q >= 0 && q != t_mc_box && ctx->tree.nodes[q].next_sibling_idx < 0) q = ctx->tree.nodes[q].parent_idx;
                if (q < 0 || q == t_mc_box) break;
                q = ctx->tree.nodes[q].next_sibling_idx;
            }
        }
    }
    float bottom = top;
    for (int k = from; k < dl->count; k++) {
        const DisplayListCmd *q = &dl->cmds[k];
        if (!(q->w > 0.0f && q->h > 0.0f) || q->type == DL_MASK_BEGIN || q->type == DL_MASK_END || q->type == DL_XFORM) continue;
        float e = vm == 1 ? q->x + q->w - cl : vm == 2 ? cr - q->x : q->y + q->h;
        if (e > bottom) bottom = e;
    }
    /* (more columns than the count when its height holds the content short; a
     * hair over a column's end is that column's) */
    int ncols = (int)ceilf((bottom - top - 0.01f) / H);
    if (ncols < 1) ncols = 1;
    if (ncols > 1000) ncols = 1000;
    if (ncols == 1 && mb->mc_n <= 1 && dst == top && hang_col < 0 && !next) return;
    bool rtl = (mb->s->direction == 1) != (vm && mb->s->writing_mode == 4);
    float U = vm ? (float)mb->content_height : (float)mb->content_width;
    DisplayListCmd *src = (DisplayListCmd *)malloc((size_t)n0 * sizeof(DisplayListCmd));
    if (!src) return;
    memcpy(src, dl->cmds + from, (size_t)n0 * sizeof(DisplayListCmd));
    dl->count = from;
    bool wrap = mb->mc_rgap >= 0.0f && mb->mc_n > 0;
    (void)ctop;
    float pclip[4] = { (float)(mb->x + mb->s->border_left), (float)(mb->y + mb->s->border_top),
                       (float)(mb->x + mb->width - mb->s->border_right), (float)(mb->y + mb->height - mb->s->border_bottom) };
    for (int c = 0; c < ncols; c++) {
        /* (a column cuts only along the block axis: what overflows it sideways stays visible) */
        float clipr[4] = { -1e9f, c == 0 && first_run ? -1e9f : top + c * H,
                           1e9f, c == ncols - 1 && last_run ? 1e9f : top + (c + 1) * H };
        if (vm) {
            float a = clipr[1], b = clipr[3];
            clipr[1] = -1e9f; clipr[3] = 1e9f;
            if (vm == 1) { clipr[0] = a <= -1e8f ? -1e9f : cl + a; clipr[2] = b >= 1e8f ? 1e9f : cl + b; }
            else { clipr[0] = b >= 1e8f ? -1e9f : cr - b; clipr[2] = a <= -1e8f ? 1e9f : cr - a; }
        }
        /* (right to left: the first column at the right; wrapping, rows of
         * mc_n columns, each overflowing only at the row's ends) */
        float ext = 0.0f, real_bd = top + (c + 1) * H;
        if (hang_col >= 0) {
            if (c != hang_col) continue;
            clipr[3] = 1e9f;
        }
        for (int q = 0; q < next; q++) if (ext_col[q] == c) ext += ext_v[q];
        int row = wrap ? c / mb->mc_n : 0, col = wrap ? c % mb->mc_n : c;
        float dx = rtl ? (U - W) - col * (W + G) : col * (W + G), dy = dst - top - c * H + row * (H + mb->mc_rgap);
        if (vm) { float across = vm == 1 ? dy : -dy; dy = dx; dx = across; }
        if (!display_list_reserve(dl, n0 + 4)) break;
        /* (nothing of it in this column: no masks for nothing) */
        {
            bool any = false;
            for (int k = 0; k < n0 && !any; k++) {
                const DisplayListCmd *q = &src[k];
                if (q->type == DL_MASK_BEGIN || q->type == DL_MASK_END || q->type == DL_XFORM) continue;
                any = q->x < clipr[2] && q->x + q->w > clipr[0] && q->y < clipr[3] && q->y + q->h > clipr[1];
            }
            if (!any) continue;
        }
        if (need_x && xf0) {
            float mv[6], px[6];
            const float *bx = xf0;
            double pm[6];
            if (!has_x && !vm && !wrap && t_mc_flat && t_mc_box >= 0 && css_layout_box_map_col(ctx, t_mc_box, m, top, H, c, pm)) {
                for (int q = 0; q < 6; q++) px[q] = (float)pm[q];
                bx = px;
            }
            xf_moved(mv, bx, dx, dy);
            dl_put_xform(dl, mv);
        }
        int mstack[32], msp = 0;
        for (int k = 0; k < n0; k++) {
            DisplayListCmd cmd = src[k];
            if (!display_list_reserve(dl, 2)) break;
            if (cmd.type == DL_XFORM) {
                float mv[6];
                xf_moved(mv, cmd.u.xf, dx, dy);
                memcpy(cmd.u.xf, mv, sizeof mv);
                dl->cmds[dl->count++] = cmd;
                continue;
            }
            if (cmd.type == DL_MASK_BEGIN || cmd.type == DL_SHADOW) {
                /* (a shadow: the part in this column of the rectangle it is drawn over, its shape whole) */
                if (cmd.type == DL_SHADOW && !dl_clip_cmd(&cmd, clipr)) continue;
                int nm = dl_mask_moved(dl, cmd.u.mask_index, dx, dy);
                if (nm < 0) continue;
                if (cmd.type == DL_MASK_BEGIN && msp < 32) mstack[msp++] = nm;
                cmd.u.mask_index = nm;
                cmd.x += dx;
                cmd.y += dy;
                dl->cmds[dl->count++] = cmd;
                continue;
            }
            if (cmd.type == DL_MASK_END) {
                if (msp > 0) cmd.u.mask_index = mstack[--msp];
                cmd.x += dx;
                cmd.y += dy;
                dl->cmds[dl->count++] = cmd;
                continue;
            }
            if (!dl_clip_cmd(&cmd, clipr)) continue;
            if (ext > 0.0f && cmd.type == DL_RECT && cmd.y + cmd.h >= real_bd - 0.01f) cmd.h += ext;
            cmd.x += dx;
            cmd.y += dy;
            if (cmd.type == DL_BORDER) { cmd.u.border.x += dx; cmd.u.border.y += dy; }
            /* (a fill on whole pixels, moved by a fraction: its true edges moved, on whole pixels again; an edge the strip cut is where the cut is) */
            if (cmd.type == DL_RECT && cmd.u.xf[5] == DL_SNAP_MARK && !need_x) {
                const DisplayListCmd *o = &src[k];
                float e0 = cmd.x, e1 = cmd.x + cmd.w, f0 = cmd.y, f1 = cmd.y + cmd.h;
                if (cmd.x - dx == o->x) e0 += cmd.u.xf[0];
                if (cmd.x - dx + cmd.w == o->x + o->w) e1 += cmd.u.xf[2];
                if (cmd.y - dy == o->y) f0 += cmd.u.xf[1];
                if (cmd.y - dy + cmd.h == o->y + o->h) f1 += cmd.u.xf[3];
                float x0 = roundf(e0), x1 = roundf(e1), y0 = roundf(f0), y1 = roundf(f1);
                if (e1 > e0 && x1 <= x0) x1 = x0 + 1.0f;
                if (f1 > f0 && y1 <= y0) y1 = y0 + 1.0f;
                cmd.x = x0; cmd.y = y0; cmd.w = x1 - x0; cmd.h = y1 - y0;
                cmd.u.xf[0] = e0 - x0; cmd.u.xf[1] = f0 - y0; cmd.u.xf[2] = e1 - x1; cmd.u.xf[3] = f1 - y1;
                if (cmd.w <= 0.0f || cmd.h <= 0.0f) continue;
            }
            /* (overflow clipped: no overflow columns past its padding box) */
            if (mb->s->overflow != 0 && !wrap && c >= mb->mc_n && !dl_clip_cmd(&cmd, pclip)) continue;
            dl->cmds[dl->count++] = cmd;
        }
    }
    if (need_x) dl_put_xform(dl, xf_end);
    free(src);
}

/* (what [from, ...) painted moved by (dx, dy), masks and transforms with it) */
static void dl_shift_range(DisplayList *dl, int from, float dx, float dy)
{
    int st[32], sp = 0;
    for (int k = from; k < dl->count; k++) {
        DisplayListCmd *c = &dl->cmds[k];
        if (c->type == DL_XFORM) { float mv[6]; xf_moved(mv, c->u.xf, dx, dy); memcpy(c->u.xf, mv, sizeof mv); continue; }
        if (c->type == DL_MASK_BEGIN || c->type == DL_SHADOW) {
            int nm = dl_mask_moved(dl, c->u.mask_index, dx, dy);
            c = &dl->cmds[k];
            if (nm >= 0) {
                if (c->type == DL_MASK_BEGIN && sp < 32) st[sp++] = nm;
                c->u.mask_index = nm;
            }
        } else if (c->type == DL_MASK_END) { if (sp > 0) c->u.mask_index = st[--sp]; }
        else if (c->type == DL_BORDER) { c->u.border.x += dx; c->u.border.y += dy; }
        c->x += dx;
        c->y += dy;
    }
}

/* (what box painted, [from, ...): through the columns of the multicol
 * containers it is in, up to those around stop (-1: all)) */
static void dl_mc_chain(const LayoutContext *ctx, DisplayList *dl, int from, int box, int stop, const float *xf)
{
    /* (print: what is fixed is on every page) */
    if (g_css_print.on && ctx->tree.root_idx >= 0 && dl->count > from) {
        bool fx = false;
        for (int p = box, d = 0; p >= 0 && d < 1024; p = ctx->tree.nodes[p].parent_idx, d++) {
            const LayoutStyle *ps = ctx->boxes[p].s;
            if (ps->position == CSS_POSITION_FIXED) { fx = ps->z_index != 999999000 && ps->z_index != -999999000 && ps->z_index != -999999001; if (!fx) break; }
        }
        const LayoutBox *rb = &ctx->boxes[ctx->tree.root_idx];
        if (fx && rb->mc_n > 0 && rb->mc_used > 1) {
            int n0 = dl->count - from, bal = 0;
            float step = rb->mc_h + (rb->mc_rgap > 0.0f ? rb->mc_rgap : 0.0f);
            DisplayListCmd *src = (DisplayListCmd *)malloc((size_t)n0 * sizeof(DisplayListCmd));
            if (src) {
                memcpy(src, dl->cmds + from, (size_t)n0 * sizeof(DisplayListCmd));
                for (int k = 0; k < n0; k++) { if (src[k].type == DL_MASK_BEGIN) bal++; else if (src[k].type == DL_MASK_END) bal--; }
                for (int pg = 1; pg < rb->mc_used && pg < 40 && bal == 0; pg++) {
                    int st[32], sp = 0;
                    float dy = pg * step;
                    for (int k = 0; k < n0; k++) {
                        if (!display_list_reserve(dl, 1)) break;
                        DisplayListCmd c = src[k];
                        if (c.type == DL_XFORM) { dl->cmds[dl->count++] = c; continue; }
                        if (c.type == DL_MASK_BEGIN || c.type == DL_SHADOW) {
                            int nm = dl_mask_moved(dl, c.u.mask_index, 0.0f, dy);
                            if (nm < 0) continue;
                            if (c.type == DL_MASK_BEGIN && sp < 32) st[sp++] = nm;
                            c.u.mask_index = nm;
                        } else if (c.type == DL_MASK_END) { if (sp > 0) c.u.mask_index = st[--sp]; }
                        else if (c.type == DL_BORDER) c.u.border.y += dy;
                        c.y += dy;
                        dl->cmds[dl->count++] = c;
                    }
                }
                free(src);
            }
        }
    }
    int mch = -1, mo = mc_owner(ctx, box, &mch);
    t_mc_box = box;
    for (int lv = 0; mo >= 0 && lv < 8; lv++) {
        if (stop >= 0) {
            bool inside = false;
            for (int p = mo, d = 0; p >= 0 && d < 1024; p = ctx->tree.nodes[p].parent_idx, d++) if (p == stop) { inside = true; break; }
            if (!inside) break;
        }
        /* (relative offsets inside these columns: the cut is where the boxes were, then they move) */
        float sx = 0.0f, sy = 0.0f;
        for (int p = box, d = 0; p >= 0 && p != mo && d < 1024; p = ctx->tree.nodes[p].parent_idx, d++) { sx += ctx->boxes[p].rel_dx; sy += ctx->boxes[p].rel_dy; }
        bool sh = (sx != 0.0f || sy != 0.0f) && dl->count > from;
        if (sh) dl_shift_range(dl, from, -sx, -sy);
        dl_columns_range(ctx, dl, from, mo, mch, xf);
        if (sh) dl_shift_range(dl, from, sx, sy);
        mo = mc_owner(ctx, mo, &mch);
    }
}

/* ---- CSS masks ---- */

static int dl_add_mask(DisplayList *dl, const DisplayMask *m)
{
    if (dl->mask_count == dl->mask_capacity) {
        int nc = dl->mask_capacity ? dl->mask_capacity * 2 : 16;
        DisplayMask *nm = (DisplayMask *)realloc(dl->masks, (size_t)nc * sizeof(*nm));
        if (!nm) return -1;
        dl->masks = nm;
        dl->mask_capacity = nc;
    }
    dl->masks[dl->mask_count] = *m;
    return dl->mask_count++;
}

static bool dl_add_mask_cmd(DisplayList *dl, DisplayListCmdType type, int mask_index, const DisplayMask *m)
{
    if (!display_list_reserve(dl, 1)) return false;
    DisplayListCmd *cmd = &dl->cmds[dl->count++];
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = type;
    cmd->x = m->x; cmd->y = m->y; cmd->w = m->w; cmd->h = m->h;
    cmd->a = 1.0f;
    cmd->u.mask_index = mask_index;
    return true;
}

/* Case-insensitive substring test over the first n bytes of s. */
static bool memmem_ci(const char *s, size_t n, const char *needle)
{
    size_t m = strlen(needle);
    for (size_t i = 0; i + m <= n; i++)
        if (strncasecmp(s + i, needle, m) == 0) return true;
    return false;
}

/* Alpha of a CSS color token (masks use only alpha). */
static float mask_color_alpha(const char *s, size_t n)
{
    while (n && isspace((unsigned char)*s)) { s++; n--; }
    if (n >= 11 && strncasecmp(s, "transparent", 11) == 0) return 0.0f;
    if (n >= 5 && strncasecmp(s, "rgba(", 5) == 0) {
        const char *last = NULL;
        for (const char *q = s; q < s + n; q++) if (*q == ',' || *q == '/') last = q;
        if (last) {
            float a = (float)atof(last + 1);
            for (const char *q = last + 1; q < s + n && *q != ')'; q++) if (*q == '%') { a /= 100.0f; break; }
            return a;
        }
        return 1.0f;
    }
    if (n >= 4 && strncasecmp(s, "rgb(", 4) == 0) {
        for (const char *q = s; q < s + n; q++) if (*q == '/') return (float)atof(q + 1);
        return 1.0f;
    }
    if (s[0] == '#') {
        size_t hl = 0;
        while (hl + 1 < n && isxdigit((unsigned char)s[hl + 1])) hl++;
        unsigned v = 0;
        if (hl == 8) { sscanf(s + 7, "%2x", &v); return (float)v / 255.0f; }
        if (hl == 4) { sscanf(s + 4, "%1x", &v); return (float)v / 15.0f; }
    }
    return 1.0f;
}

/* linear-gradient(<angle> | to <side>, <color> [<pos>], ...) into m's angle
 * and alpha stops (px positions over the gradient line of a w x h box). */
static bool parse_mask_gradient(const char *g, DisplayMask *m, float w, float h)
{
    const char *p = strchr(g, '(');
    if (!p) return false;
    p++;
    m->angle_deg = 180.0f;
    m->nstops = 0;
    float pos[DL_MASK_STOPS];
    bool has_pos[DL_MASK_STOPS];
    bool first = true;
    while (*p && *p != ')' && m->nstops < DL_MASK_STOPS) {
        while (*p == ',' || isspace((unsigned char)*p)) p++;
        const char *st = p;
        int depth = 0;
        while (*p && (depth > 0 || (*p != ',' && *p != ')'))) {
            if (*p == '(') depth++;
            else if (*p == ')') depth--;
            p++;
        }
        size_t n = (size_t)(p - st);
        while (n && isspace((unsigned char)st[n - 1])) n--;
        if (!n) continue;
        if (first) {
            first = false;
            if (n > 3 && strncasecmp(st, "to ", 3) == 0) {
                bool r = memmem_ci(st, n, "right"), l = memmem_ci(st, n, "left");
                bool t = memmem_ci(st, n, "top"), b = memmem_ci(st, n, "bottom");
                float ax = r ? 1.0f : l ? -1.0f : 0.0f, ay = b ? 1.0f : t ? -1.0f : 0.0f;
                m->angle_deg = (float)(atan2(ax, -ay) * 180.0 / M_PI);
                if ((r || l) && (t || b) && w > 0 && h > 0)   /* corner: perpendicular to the diagonal */
                    m->angle_deg = (float)(atan2(ax * h, -ay * w) * 180.0 / M_PI);
                continue;
            }
            char *end = NULL;
            double v = strtod(st, &end);
            if (end && end > st && end < st + n) {
                if (strncasecmp(end, "deg", 3) == 0) { m->angle_deg = (float)v; continue; }
                if (strncasecmp(end, "turn", 4) == 0) { m->angle_deg = (float)(v * 360.0); continue; }
                if (strncasecmp(end, "rad", 3) == 0) { m->angle_deg = (float)(v * 180.0 / M_PI); continue; }
            }
        }
        /* color [position]: the position is the last space-separated token
         * outside parentheses. */
        const char *cut = NULL;
        depth = 0;
        for (const char *q = st; q < st + n; q++) {
            if (*q == '(') depth++;
            else if (*q == ')') depth--;
            else if (depth == 0 && isspace((unsigned char)*q)) cut = q;
        }
        int k = m->nstops++;
        has_pos[k] = false;
        pos[k] = 0.0f;
        m->stop_alpha[k] = mask_color_alpha(st, cut ? (size_t)(cut - st) : n);
        if (cut) {
            char tok[64];
            size_t tl = (size_t)(st + n - (cut + 1));
            if (tl >= sizeof(tok)) tl = sizeof(tok) - 1;
            memcpy(tok, cut + 1, tl);
            tok[tl] = '\0';
            char *end = NULL;
            double v = strtod(tok, &end);
            if (end != tok) {
                has_pos[k] = true;
                double rad = m->angle_deg * M_PI / 180.0;
                double line = fabs(w * sin(rad)) + fabs(h * cos(rad));
                pos[k] = (float)(*end == '%' ? v / 100.0 : (line > 0 ? v / line : 0.0));
            }
        }
    }
    if (m->nstops < 1) return false;
    /* Missing positions: first 0, last 1, others spread between neighbours;
     * positions never decrease. */
    if (!has_pos[0]) { pos[0] = 0.0f; has_pos[0] = true; }
    if (!has_pos[m->nstops - 1]) { pos[m->nstops - 1] = 1.0f; has_pos[m->nstops - 1] = true; }
    for (int i = 1; i < m->nstops; i++) {
        if (has_pos[i]) { if (pos[i] < pos[i - 1]) pos[i] = pos[i - 1]; continue; }
        int j = i;
        while (!has_pos[j]) j++;
        for (int k = i; k < j; k++) pos[k] = pos[i - 1] + (pos[j] - pos[i - 1]) * (float)(k - i + 1) / (float)(j - i + 1);
        i = j - 1;
    }
    memcpy(m->stop_pos, pos, sizeof(float) * (size_t)m->nstops);
    return true;
}

/* (box area k of a box: 0 border, 1 padding, 2 content, 3 margin box, from its border box) */
static void box_area_rect(const LayoutBox *box, int k, float *x, float *y, float *w, float *h)
{
    double l = 0, t = 0, r = 0, b = 0;
    if (k == 3) { l = -box->margin_left; t = -box->margin_top; r = -box->margin_right; b = -box->margin_bottom; }
    if (k == 1 || k == 2) { l = box->s->border_left; t = box->s->border_top; r = box->s->border_right; b = box->s->border_bottom; }
    if (k == 2) { l += box->padding_left; t += box->padding_top; r += box->padding_right; b += box->padding_bottom; }
    *x = (float)(box->x + l); *y = (float)(box->y + t);
    *w = (float)(box->width - l - r); *h = (float)(box->height - t - b);
    if (*w < 0.0f) *w = 0.0f;
    if (*h < 0.0f) *h = 0.0f;
}

/* The mask for a box with mask-image, or false when it has none usable. */
static bool box_mask(const LayoutBox *box, DisplayMask *m)
{
    memset(m, 0, sizeof(*m));
    m->x = (float)box->x; m->y = (float)box->y;
    m->w = (float)box->width; m->h = (float)box->height;
    m->image_handle = -1;
    if (box->s->mask_kind == 2 && box->s->mask_image) {
        m->kind = 2;
        return parse_mask_gradient(box->s->mask_image, m, m->w, m->h);
    }
    if (box->s->mask_kind != 1 || !box->s->mask_image || !g_image_cache) return false;
    m->kind = 1;
    m->repeat = ~box->s->mask_no_repeat & 3;
    /* (mask-origin: the box it is placed and sized in) */
    float ox = m->x, oy = m->y, ow = m->w, oh = m->h;
    box_area_rect(box, box->s->mask_origin, &ox, &oy, &ow, &oh);
    /* Size: auto = natural size (the box until the image is known). */
    float iw = ow, ih = oh;
    int nw = 0, nh = 0;
    int probe = image_cache_request(g_image_cache, box->s->mask_image, 0, 0);
    bool natural = probe >= 0 && image_cache_get_natural(g_image_cache, probe, &nw, &nh) && nw > 0 && nh > 0;
    if (box->s->mask_size_kind == 0 && natural) { iw = (float)nw; ih = (float)nh; }
    else if ((box->s->mask_size_kind == 1 || box->s->mask_size_kind == 2) && natural) {
        float sx = ow / (float)nw, sy = oh / (float)nh;
        float s = box->s->mask_size_kind == 1 ? (sx < sy ? sx : sy) : (sx > sy ? sx : sy);
        iw = (float)nw * s; ih = (float)nh * s;
    } else if (box->s->mask_size_kind == 3) {
        iw = (float)((box->s->mask_size_pct & 1) ? box->s->mask_size_w * ow : box->s->mask_size_w);
        if (box->s->mask_size_h >= 0.0)
            ih = (float)((box->s->mask_size_pct & 2) ? box->s->mask_size_h * oh : box->s->mask_size_h);
        else
            ih = natural ? iw * (float)nh / (float)nw : iw;
    }
    if (iw < 1.0f || ih < 1.0f) return false;
    m->iw = iw; m->ih = ih;
    m->ix = ox + (float)((box->s->mask_pos_pct & 1) ? box->s->mask_pos_x * (ow - iw) : box->s->mask_pos_x) + (float)box->s->mask_pos_xpx;
    m->iy = oy + (float)((box->s->mask_pos_pct & 2) ? box->s->mask_pos_y * (oh - ih) : box->s->mask_pos_y) + (float)box->s->mask_pos_ypx;
    m->image_handle = image_cache_request(g_image_cache, box->s->mask_image, (int)ceilf(iw), (int)ceilf(ih));
    return m->image_handle >= 0;
}

/* ---- clip-path: basic shapes (CSS Masking 1 section 3.1) ----
 * inset(), rect(), xywh(), circle(), ellipse() and a reference box alone,
 * as a rounded-rectangle mask (kind 3) over the box and its subtree. */

static __thread double t_cp_vw = 800.0, t_cp_vh = 600.0;
/* (a clip-path polygon's corners past its mask's ten, from its box's corner:
 * the kind 11 masks after it carry them) */
#define CP_POLY_MAX 128
static __thread float t_cp_px[CP_POLY_MAX], t_cp_py[CP_POLY_MAX];
static __thread int t_cp_n;
static __thread int t_cp_sub;              /* (its subpaths: more than one, their joins drawn no edge) */
static __thread HtmlDocument *t_cp_doc;    /* (for url(#id)) */

/* (a length: px, em, rem, %, vw/vh; a percentage of base; false: not one) */
static bool cp_len(const char **pp, double base, double fs, double *out)
{
    const char *p = *pp;
    while (*p == ' ' || *p == ',') p++;
    if (!strncasecmp(p, "calc(", 5)) {
        /* (calc(): lengths added and taken away) */
        const char *q = p + 5;
        double sum = 0.0;
        int sign = 1;
        for (;;) {
            while (*q == ' ') q++;
            double v;
            if (!cp_len(&q, base, fs, &v)) return false;
            sum += sign * v;
            while (*q == ' ') q++;
            if (*q == ')') { q++; break; }
            if (*q == '+') sign = 1;
            else if (*q == '-') sign = -1;
            else return false;
            q++;
        }
        *out = sum;
        *pp = q;
        return true;
    }
    char *e = NULL;
    double v = strtod(p, &e);
    if (e == p) return false;
    if (*e == '%') { v = v * base / 100.0; e++; }
    else if (!strncasecmp(e, "px", 2)) e += 2;
    else if (!strncasecmp(e, "rem", 3)) { v *= 16.0; e += 3; }
    else if (!strncasecmp(e, "em", 2)) { v *= fs; e += 2; }
    else if (!strncasecmp(e, "ch", 2)) { v *= fs * 0.5; e += 2; }
    else if (!strncasecmp(e, "vw", 2)) { v *= t_cp_vw / 100.0; e += 2; }
    else if (!strncasecmp(e, "vh", 2)) { v *= t_cp_vh / 100.0; e += 2; }
    else if (v != 0.0) return false;
    *out = v;
    *pp = e;
    return true;
}

static bool cp_word(const char **pp, const char *w)
{
    const char *p = *pp;
    while (*p == ' ' || *p == ',') p++;
    size_t n = strlen(w);
    if (strncasecmp(p, w, n) || (p[n] && (isalnum((unsigned char)p[n]) || p[n] == '-'))) return false;
    *pp = p + n;
    return true;
}

/* (a position: one or two keywords or lengths, against the box w by h) */
static void cp_position(const char *p, double w, double h, double fs, double *cx, double *cy)
{
    *cx = w / 2.0;
    *cy = h / 2.0;
    int kx = -1, ky = -1;   /* (keywords: 0 start, 1 center, 2 end) */
    bool x_given = false;
    for (int i = 0; i < 2; i++) {
        if (cp_word(&p, "left")) { kx = 0; x_given = true; }
        else if (cp_word(&p, "right")) { kx = 2; x_given = true; }
        else if (cp_word(&p, "top")) ky = 0;
        else if (cp_word(&p, "bottom")) ky = 2;
        else if (cp_word(&p, "center")) { if (!x_given && i == 0) { kx = 1; x_given = true; } else ky = 1; }
        else {
            double l;
            bool across = !x_given;
            if (!cp_len(&p, across ? w : h, fs, &l)) break;
            if (across) { *cx = l; kx = -3; x_given = true; } else { *cy = l; ky = -3; }
        }
    }
    if (kx >= 0) *cx = kx * w / 2.0;
    if (ky >= 0) *cy = ky * h / 2.0;
}

/* (an ellipse's radius on an axis: closest-side, farthest-side or a length) */
static bool cp_radius(const char **pp, double base, double fs, double d1, double d2, double *r)
{
    if (cp_word(pp, "closest-side")) { *r = d1 < d2 ? d1 : d2; return true; }
    if (cp_word(pp, "farthest-side")) { *r = d1 > d2 ? d1 : d2; return true; }
    return cp_len(pp, base, fs, r);
}

/* (border-radius's syntax after round: one to four values, / and the vertical ones) */
static void cp_round(const char *rq, double w, double h, double fs, DisplayMask *m)
{
    double hv[2][4];
    int nh[2] = { 0, 0 }, side = 0;
    while (*rq && *rq != ')') {
        while (*rq == ' ') rq++;
        if (*rq == '/') { side = 1; rq++; continue; }
        double l;
        if (nh[side] >= 4 || !cp_len(&rq, side ? h : w, fs, &l)) break;
        hv[side][nh[side]++] = l;
    }
    if (nh[0] == 0) return;
    if (nh[1] == 0) { memcpy(hv[1], hv[0], sizeof hv[0]); nh[1] = nh[0]; }
    for (int sd = 0; sd < 2; sd++) {
        int k = nh[sd];
        if (k == 1) hv[sd][1] = hv[sd][2] = hv[sd][3] = hv[sd][0];
        else if (k == 2) { hv[sd][2] = hv[sd][0]; hv[sd][3] = hv[sd][1]; }
        else if (k == 3) hv[sd][3] = hv[sd][1];
    }
    for (int c = 0; c < 4; c++) { m->rx[c] = (float)hv[0][c]; m->ry[c] = (float)hv[1][c]; }
    /* (overlapping radii scale down together) */
    float f = 1.0f;
    float sums[4] = { m->rx[0] + m->rx[1], m->ry[1] + m->ry[2], m->rx[2] + m->rx[3], m->ry[3] + m->ry[0] };
    float sides[4] = { (float)w, (float)h, (float)w, (float)h };
    for (int k = 0; k < 4; k++) if (sums[k] > sides[k] && sums[k] > 0.0f && sides[k] / sums[k] < f) f = sides[k] / sums[k];
    for (int c = 0; c < 4; c++) { m->rx[c] *= f; m->ry[c] *= f; }
}

/* (an SVG shape's clip-path circle() or ellipse() as an SVG element, of its
 * fill box, or its stroke box (the default) grown by half the stroke width) */
static bool svg_clip_shape(HtmlNode *n, const char *v, char *out, size_t cap)
{
    if (!n->tag_name) return false;
    double x = 0, y = 0, w = 0, h = 0;
#define NUM(nm) (node_attribute_value(n, nm) ? strtod(node_attribute_value(n, nm), NULL) : 0.0)
    if (!strcasecmp(n->tag_name, "rect")) { x = NUM("x"); y = NUM("y"); w = NUM("width"); h = NUM("height"); }
    else if (!strcasecmp(n->tag_name, "circle")) { double r = NUM("r"); x = NUM("cx") - r; y = NUM("cy") - r; w = h = 2 * r; }
    else if (!strcasecmp(n->tag_name, "ellipse")) { double rx = NUM("rx"), ry = NUM("ry"); x = NUM("cx") - rx; y = NUM("cy") - ry; w = 2 * rx; h = 2 * ry; }
    else return false;
    if (w <= 0.0 || h <= 0.0) return false;
    const char *p = v;
    while (*p == ' ') p++;
    bool circ = !strncasecmp(p, "circle(", 7), ell = !strncasecmp(p, "ellipse(", 8);
    if (!circ && !ell) return false;
    const char *args = p + (circ ? 7 : 8), *close = strchr(args, ')');
    if (!close) return false;
    /* (the reference box: fill-box for content/padding/fill, else the stroke box) */
    const char *after = close + 1;
    bool fill = strstr(after, "fill-box") || strstr(after, "content-box") || strstr(after, "padding-box");
    if (strstr(after, "view-box")) return false;
    if (!fill) {
        const char *sk = node_attribute_value(n, "stroke");
        if (sk && strcasecmp(sk, "none")) {
            double sw = node_attribute_value(n, "stroke-width") ? NUM("stroke-width") : 1.0;
            x -= sw / 2; y -= sw / 2; w += sw; h += sw;
        }
    }
#undef NUM
    char a[128];
    size_t al = (size_t)(close - args);
    if (al >= sizeof a) return false;
    memcpy(a, args, al);
    a[al] = 0;
    char *at = strstr(a, "at ");
    double cx = w / 2, cy = h / 2;
    if (at) { cp_position(at + 3, w, h, 16.0, &cx, &cy); *at = 0; }
    double dx1 = cx, dx2 = w - cx, dy1 = cy, dy2 = h - cy;
    const char *q = a;
    if (circ) {
        double r = fmin(fmin(dx1, dx2), fmin(dy1, dy2));
        double diag = sqrt((w * w + h * h) / 2.0);
        if (cp_word(&q, "farthest-side")) r = fmax(fmax(dx1, dx2), fmax(dy1, dy2));
        else if (!cp_word(&q, "closest-side")) { double rr; if (cp_len(&q, diag, 16.0, &rr)) r = rr; }
        snprintf(out, cap, "<circle cx=\"%.4f\" cy=\"%.4f\" r=\"%.4f\"/>", x + cx, y + cy, r);
    } else {
        double rx = fmin(dx1, dx2), ry = fmin(dy1, dy2);
        if (!cp_radius(&q, w, 16.0, dx1, dx2, &rx)) rx = fmin(dx1, dx2);
        if (!cp_radius(&q, h, 16.0, dy1, dy2, &ry)) ry = fmin(dy1, dy2);
        snprintf(out, cap, "<ellipse cx=\"%.4f\" cy=\"%.4f\" rx=\"%.4f\" ry=\"%.4f\"/>", x + cx, y + cy, rx, ry);
    }
    return true;
}

/* (path()'s data as a polygon from (bx, by): curves in short lines, a closed
 * subpath back to its start; its corners' count, -1 for too many or bad data) */
static int cp_path_points(const char *q, double bx, double by, double *px, double *py, int max)
{
    int n = 0;
    double cx = 0, cy = 0, sx = 0, sy = 0, lcx = 0, lcy = 0;   /* (current, subpath start, last control point) */
    char cmd = 0, prev = 0;
#define CP_PT(X, Y) do { if (n >= max) return -1; px[n] = bx + (X); py[n] = by + (Y); n++; } while (0)
    for (;;) {
        while (*q == ' ' || *q == ',' || *q == '\t' || *q == '\n' || *q == '\r') q++;
        if (!*q || *q == '"' || *q == '\'') break;
        if (isalpha((unsigned char)*q)) cmd = *q++;
        else if (!cmd) return -1;
        char C = (char)toupper((unsigned char)cmd);
        bool rel = cmd != C;
        int need = C == 'Z' ? 0 : C == 'H' || C == 'V' ? 1 : C == 'M' || C == 'L' || C == 'T' ? 2 : C == 'S' || C == 'Q' ? 4 : C == 'C' ? 6 : C == 'A' ? 7 : -1;
        if (need < 0) return -1;
        double a[7];
        for (int i = 0; i < need; i++) {
            while (*q == ' ' || *q == ',' || *q == '\t' || *q == '\n' || *q == '\r') q++;
            char *e = NULL;
            a[i] = strtod(q, &e);
            if (e == q) return -1;
            q = e;
        }
        double ox = rel ? cx : 0, oy = rel ? cy : 0;
        if (C == 'Z') {
            if (n > 0 && (px[n - 1] != bx + sx || py[n - 1] != by + sy)) CP_PT(sx, sy);
            cx = sx; cy = sy;
            cmd = 0;
        } else if (C == 'M') {
            /* (the last subpath back to its start first, so the joins cancel) */
            if (n > 0 && (px[n - 1] != bx + sx || py[n - 1] != by + sy)) CP_PT(sx, sy);
            /* (and to the first one's start: joins all to it, there and back) */
            if (n > 0 && (px[n - 1] != px[0] || py[n - 1] != py[0])) CP_PT(px[0] - bx, py[0] - by);
            cx = a[0] + ox; cy = a[1] + oy; sx = cx; sy = cy;
            CP_PT(cx, cy);
            t_cp_sub++;
            cmd = rel ? 'l' : 'L';   /* (pairs after a move: lines) */
        } else if (C == 'L' || C == 'H' || C == 'V') {
            if (C == 'L') { cx = a[0] + ox; cy = a[1] + oy; }
            else if (C == 'H') cx = a[0] + ox;
            else cy = a[0] + oy;
            CP_PT(cx, cy);
        } else if (C == 'A') {
            /* (an elliptical arc, SVG 2 B.2.4: its centre, then short lines) */
            double x2 = a[5] + ox, y2 = a[6] + oy, rx = fabs(a[0]), ry = fabs(a[1]);
            double phi = a[2] * 3.14159265358979 / 180.0, cph = cos(phi), sph = sin(phi);
            bool fa = a[3] != 0.0, fsw = a[4] != 0.0;
            if (rx <= 0.0 || ry <= 0.0 || (x2 == cx && y2 == cy)) { cx = x2; cy = y2; CP_PT(cx, cy); prev = C; continue; }
            double dx = (cx - x2) / 2.0, dy = (cy - y2) / 2.0;
            double x1p = cph * dx + sph * dy, y1p = -sph * dx + cph * dy;
            double lam = x1p * x1p / (rx * rx) + y1p * y1p / (ry * ry);
            if (lam > 1.0) { rx *= sqrt(lam); ry *= sqrt(lam); }
            double num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
            double den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
            double co = den > 0.0 && num > 0.0 ? sqrt(num / den) : 0.0;
            if (fa == fsw) co = -co;
            double cxp = co * rx * y1p / ry, cyp = -co * ry * x1p / rx;
            double ccx = cph * cxp - sph * cyp + (cx + x2) / 2.0, ccy = sph * cxp + cph * cyp + (cy + y2) / 2.0;
            double t1 = atan2((y1p - cyp) / ry, (x1p - cxp) / rx);
            double t2 = atan2((-y1p - cyp) / ry, (-x1p - cxp) / rx);
            double dt = t2 - t1;
            if (fsw && dt < 0.0) dt += 2.0 * 3.14159265358979;
            if (!fsw && dt > 0.0) dt -= 2.0 * 3.14159265358979;
            int segs = (int)ceil(fabs(dt) / (3.14159265358979 / 12.0));
            if (segs < 2) segs = 2;
            for (int k = 1; k <= segs; k++) {
                double t = t1 + dt * k / segs;
                double X = k == segs ? x2 : ccx + rx * cos(t) * cph - ry * sin(t) * sph;
                double Y = k == segs ? y2 : ccy + rx * cos(t) * sph + ry * sin(t) * cph;
                CP_PT(X, Y);
            }
            cx = x2; cy = y2;
        } else {
            double x1, y1, x2, y2, x, y;
            bool cubic = C == 'C' || C == 'S';
            if (C == 'C') { x1 = a[0] + ox; y1 = a[1] + oy; x2 = a[2] + ox; y2 = a[3] + oy; x = a[4] + ox; y = a[5] + oy; }
            else if (C == 'S') {
                bool after = prev == 'C' || prev == 'S';
                x1 = after ? 2 * cx - lcx : cx; y1 = after ? 2 * cy - lcy : cy;
                x2 = a[0] + ox; y2 = a[1] + oy; x = a[2] + ox; y = a[3] + oy;
            } else if (C == 'Q') { x1 = a[0] + ox; y1 = a[1] + oy; x2 = x1; y2 = y1; x = a[2] + ox; y = a[3] + oy; }
            else {
                bool after = prev == 'Q' || prev == 'T';
                x1 = after ? 2 * cx - lcx : cx; y1 = after ? 2 * cy - lcy : cy; x2 = x1; y2 = y1;
                x = a[0] + ox; y = a[1] + oy;
            }
            const int segs = 8;
            for (int k = 1; k <= segs; k++) {
                double t = (double)k / segs, u = 1 - t, X, Y;
                if (cubic) {
                    X = u * u * u * cx + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x;
                    Y = u * u * u * cy + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y;
                } else {
                    X = u * u * cx + 2 * u * t * x1 + t * t * x;
                    Y = u * u * cy + 2 * u * t * y1 + t * t * y;
                }
                CP_PT(X, Y);
            }
            lcx = cubic ? x2 : x1; lcy = cubic ? y2 : y1;
            cx = x; cy = y;
        }
        prev = C;
    }
    if (n > 0 && (px[n - 1] != bx + sx || py[n - 1] != by + sy)) CP_PT(sx, sy);
#undef CP_PT
    return n;
}

/* (a position starting with a keyword, up to a comma, ")", " with" or " of":
 * false when it doesn't start with one) */
static bool cp_kw_pos(const char **pp, double w, double h, double fs, double *x, double *y)
{
    const char *q = *pp;
    while (*q == ' ') q++;
    if (!isalpha((unsigned char)*q)) return false;
    {
        /* (calc() and the like are lengths) */
        const char *e = q;
        while (isalnum((unsigned char)*e) || *e == '-') e++;
        if (*e == '(') return false;
    }
    char ps[96];
    size_t k = 0;
    while (*q && *q != ',' && *q != ')' && strncasecmp(q, " with", 5) && strncasecmp(q, " of ", 4) && k + 1 < sizeof ps) ps[k++] = *q++;
    ps[k] = 0;
    cp_position(ps, w, h, fs, x, y);
    *pp = q;
    return true;
}

/* (shape()'s commands as SVG path data, absolute, into out: false when it
 * can't) */
bool cp_shape_path(const char *q, double w, double h, double fs, char *out, size_t cap, bool *evenodd)
{
    size_t o = 0;
    double cx = 0, cy = 0, sx = 0, sy = 0;
#define SP(...) do { int r_ = snprintf(out + o, cap - o, __VA_ARGS__); if (r_ < 0 || (size_t)r_ >= cap - o) return false; o += (size_t)r_; } while (0)
    while (*q == ' ') q++;
    if (cp_word(&q, "evenodd")) *evenodd = true;
    else cp_word(&q, "nonzero");
    if (!cp_word(&q, "from")) return false;
    /* (the start: a position up to the comma) */
    char pos[96];
    size_t k = 0;
    while (*q == ' ') q++;
    while (*q && *q != ',' && *q != ')' && k + 1 < sizeof pos) pos[k++] = *q++;
    pos[k] = 0;
    cp_position(pos, w, h, fs, &cx, &cy);
    sx = cx; sy = cy;
    SP("M %.4f %.4f ", cx, cy);
    for (;;) {
        while (*q == ' ' || *q == ',') q++;
        if (!*q || *q == ')') break;
        bool by;
        double x, y;
        /* (a coordinate pair after to or by: by from the current point) */
#define PAIR(X, Y, REL) do { if (!(REL) && cp_kw_pos(&q, w, h, fs, &(X), &(Y))) break; double a_, b_; if (!cp_len(&q, w, fs, &a_) || !cp_len(&q, h, fs, &b_)) return false; X = (REL) ? cx + a_ : a_; Y = (REL) ? cy + b_ : b_; } while (0)
#define TOBY() do { if (cp_word(&q, "to")) by = false; else if (cp_word(&q, "by")) by = true; else return false; } while (0)
        if (cp_word(&q, "close")) { SP("Z "); cx = sx; cy = sy; continue; }
        if (cp_word(&q, "move")) { TOBY(); PAIR(x, y, by); SP("M %.4f %.4f ", x, y); cx = sx = x; cy = sy = y; continue; }
        if (cp_word(&q, "line")) { TOBY(); PAIR(x, y, by); SP("L %.4f %.4f ", x, y); cx = x; cy = y; continue; }
        if (cp_word(&q, "hline") || cp_word(&q, "vline")) {
            bool hz = q[-5] == 'h' || q[-5] == 'H';
            TOBY();
            double v;
            if (!by && (cp_word(&q, "left") || cp_word(&q, "top") || cp_word(&q, "x-start") || cp_word(&q, "y-start"))) v = 0.0;
            else if (!by && cp_word(&q, "center")) v = (hz ? w : h) / 2.0;
            else if (!by && (cp_word(&q, "right") || cp_word(&q, "bottom") || cp_word(&q, "x-end") || cp_word(&q, "y-end"))) v = hz ? w : h;
            else if (!cp_len(&q, hz ? w : h, fs, &v)) return false;
            if (hz) cx = by ? cx + v : v; else cy = by ? cy + v : v;
            SP("L %.4f %.4f ", cx, cy);
            continue;
        }
        bool curve = cp_word(&q, "curve"), smooth = !curve && cp_word(&q, "smooth");
        if (curve || smooth) {
            TOBY();
            if (!by && isalpha((unsigned char)(*q == ' ' ? q[1] : *q))) {
                /* (to a position given by keywords) */
                char ps[96];
                size_t k3 = 0;
                while (*q == ' ') q++;
                while (*q && *q != ',' && *q != ')' && strncasecmp(q, " with", 5) && k3 + 1 < sizeof ps) ps[k3++] = *q++;
                ps[k3] = 0;
                cp_position(ps, w, h, fs, &x, &y);
            } else
            PAIR(x, y, by);
            double c[4];
            int nc = 0;
            if (cp_word(&q, "with")) {
                for (;;) {
                    /* (a control point up to its / or comma: a position (to), or a pair from
                     * the start (by), or a pair from start, end or origin) */
                    char cpt[96];
                    size_t k2 = 0;
                    while (*q == ' ') q++;
                    while (*q && *q != '/' && *q != ',' && *q != ')' && k2 + 1 < sizeof cpt) cpt[k2++] = *q++;
                    cpt[k2] = 0;
                    while (k2 && cpt[k2 - 1] == ' ') cpt[--k2] = 0;
                    char *fr = strstr(cpt, " from ");
                    double a_ = 0.0, b_ = 0.0;
                    if (fr) {
                        *fr = 0;
                        const char *qq = cpt;
                        if (!cp_len(&qq, w, fs, &a_) || !cp_len(&qq, h, fs, &b_)) return false;
                        const char *wh = fr + 6;
                        while (*wh == ' ') wh++;
                        double ox = 0.0, oy = 0.0;
                        if (!strncasecmp(wh, "start", 5)) { ox = cx; oy = cy; }
                        else if (!strncasecmp(wh, "end", 3)) { ox = x; oy = y; }
                        c[2 * nc] = ox + a_; c[2 * nc + 1] = oy + b_;
                    } else if (!by) {
                        bool kw = false;
                        for (const char *z = cpt; *z; z++)
                            if (isalpha((unsigned char)*z) && (z == cpt || z[-1] == ' ')) kw = true;
                        if (kw) {
                            cp_position(cpt, w, h, fs, &a_, &b_);
                        } else {
                            const char *qq = cpt;
                            if (!cp_len(&qq, w, fs, &a_) || !cp_len(&qq, h, fs, &b_)) return false;
                        }
                        c[2 * nc] = a_; c[2 * nc + 1] = b_;
                    } else {
                        const char *qq = cpt;
                        if (!cp_len(&qq, w, fs, &a_) || !cp_len(&qq, h, fs, &b_)) return false;
                        c[2 * nc] = cx + a_; c[2 * nc + 1] = cy + b_;
                    }
                    nc++;
                    while (*q == ' ') q++;
                    if (nc < 2 && *q == '/') { q++; continue; }
                    break;
                }
            }
            if (curve && nc == 1) SP("Q %.4f %.4f %.4f %.4f ", c[0], c[1], x, y);
            else if (curve && nc == 2) SP("C %.4f %.4f %.4f %.4f %.4f %.4f ", c[0], c[1], c[2], c[3], x, y);
            else if (smooth && nc == 0) SP("T %.4f %.4f ", x, y);
            else if (smooth && nc == 1) SP("S %.4f %.4f %.4f %.4f ", c[0], c[1], x, y);
            else return false;
            cx = x; cy = y;
            continue;
        }
        if (cp_word(&q, "arc")) {
            TOBY();
            PAIR(x, y, by);
            if (!cp_word(&q, "of")) return false;
            double rx, ry;
            if (!cp_len(&q, w, fs, &rx)) return false;
            const char *q2 = q;
            if (!cp_len(&q2, h, fs, &ry)) ry = rx; else q = q2;
            bool cw = false, large = false;
            double rot = 0.0;
            for (;;) {
                if (cp_word(&q, "cw")) cw = true;
                else if (cp_word(&q, "ccw")) cw = false;
                else if (cp_word(&q, "large")) large = true;
                else if (cp_word(&q, "small")) large = false;
                else if (cp_word(&q, "rotate")) {
                    while (*q == ' ') q++;
                    char *e = NULL;
                    rot = strtod(q, &e);
                    if (e == q) return false;
                    q = e;
                    if (!strncasecmp(q, "deg", 3)) q += 3;
                    else if (!strncasecmp(q, "turn", 4)) { rot *= 360.0; q += 4; }
                    else if (!strncasecmp(q, "rad", 3)) { rot *= 180.0 / 3.14159265358979; q += 3; }
                } else break;
            }
            SP("A %.4f %.4f %.4f %d %d %.4f %.4f ", rx, ry, rot, large ? 1 : 0, cw ? 1 : 0, x, y);
            cx = x; cy = y;
            continue;
        }
        return false;
#undef PAIR
#undef TOBY
    }
#undef SP
    return true;
}

/* (a number attribute of an SVG shape, a fraction of base when a percentage) */
static double cp_svg_num(HtmlNode *n, const char *name, double base)
{
    const char *v = node_attribute_value(n, name);
    if (!v) return 0.0;
    char *e = NULL;
    double d = strtod(v, &e);
    if (e && *e == '%') d = d * base / 100.0;
    return d;
}

/* (an SVG element's transform, its attribute or its style's, as the affine
 * m (a b c d e f); false for none) */
static bool cp_svg_xform(HtmlNode *n, double m[6])
{
    const char *v = node_attribute_value(n, "transform");
    const char *st = node_attribute_value(n, "style");
    const char *sv = st ? strstr(st, "transform") : NULL;
    if (sv) {
        sv += 9;
        while (*sv == ' ') sv++;
        if (*sv == ':') v = sv + 1;
    }
    m[0] = 1; m[1] = 0; m[2] = 0; m[3] = 1; m[4] = 0; m[5] = 0;
    if (!v) return false;
    bool any = false;
    for (const char *p = v; *p && *p != ';';) {
        while (*p == ' ' || *p == ',') p++;
        char fn[16];
        int k = 0;
        while (isalpha((unsigned char)*p) && k < 15) fn[k++] = (char)tolower((unsigned char)*p++);
        fn[k] = 0;
        while (*p == ' ') p++;
        if (!k || *p != '(') break;
        p++;
        double a[6] = { 0, 0, 0, 0, 0, 0 };
        int na = 0;
        while (*p && *p != ')') {
            while (*p == ' ' || *p == ',') p++;
            if (*p == ')') break;
            char *e = NULL;
            double d = strtod(p, &e);
            if (e == p) return any;
            p = e;
            if (!strncasecmp(p, "deg", 3)) p += 3;
            else if (!strncasecmp(p, "px", 2)) p += 2;
            if (na < 6) a[na++] = d;
        }
        if (*p == ')') p++;
        double f[6] = { 1, 0, 0, 1, 0, 0 };
        double r = 3.14159265358979 / 180.0;
        if (!strcmp(fn, "matrix") && na == 6) memcpy(f, a, sizeof f);
        else if (!strcmp(fn, "translate")) { f[4] = a[0]; f[5] = na > 1 ? a[1] : 0.0; }
        else if (!strcmp(fn, "scale")) { f[0] = a[0]; f[3] = na > 1 ? a[1] : a[0]; }
        else if (!strcmp(fn, "rotate")) {
            double c = cos(a[0] * r), s = sin(a[0] * r);
            f[0] = c; f[1] = s; f[2] = -s; f[3] = c;
            if (na == 3) { f[4] = a[1] - c * a[1] + s * a[2]; f[5] = a[2] - s * a[1] - c * a[2]; }
        } else if (!strcmp(fn, "skewx")) f[2] = tan(a[0] * r);
        else if (!strcmp(fn, "skewy")) f[1] = tan(a[0] * r);
        else return any;
        /* (m then f: m times f) */
        double o[6] = { m[0] * f[0] + m[2] * f[1], m[1] * f[0] + m[3] * f[1], m[0] * f[2] + m[2] * f[3],
                        m[1] * f[2] + m[3] * f[3], m[0] * f[4] + m[2] * f[5] + m[4], m[1] * f[4] + m[3] * f[5] + m[5] };
        memcpy(m, o, sizeof o);
        any = true;
    }
    return any;
}

/* (clip-path: url(#id) of an SVG <clipPath>: its shapes as one polygon from
 * (bx, by), its units the box's size for objectBoundingBox; -1 when none) */
static int cp_url_points(const char *args, double bx, double by, double bw, double bh, double *px, double *py, int max,
                         bool *evenodd)
{
    if (!t_cp_doc) return -1;
    const char *q = args;
    while (*q == ' ' || *q == '"' || *q == '\'') q++;
    if (*q != '#') return -1;
    q++;
    char id[128];
    size_t k = 0;
    while (*q && *q != ')' && *q != '"' && *q != '\'' && *q != ' ' && k + 1 < sizeof id) id[k++] = *q++;
    id[k] = 0;
    HtmlNode *cp = html_document_get_element_by_id(t_cp_doc, id);
    if (!cp || !cp->tag_name || strcasecmp(cp->tag_name, "clipPath")) return -1;
    const char *units = node_attribute_value(cp, "clipPathUnits");
    bool obb = units && !strcmp(units, "objectBoundingBox");
    double sx = obb ? bw : 1.0, sy = obb ? bh : 1.0;
    int n = 0, shapes = 0;
    t_cp_sub = 0;
    for (HtmlNode *c = html_node_first_child(t_cp_doc, cp); c; c = html_node_next_sibling(t_cp_doc, c)) {
        if (c->type != HTML_NODE_ELEMENT || !c->tag_name) continue;
        const char *tg = c->tag_name;
        int n0 = n;
        if (!shapes) {
            const char *cr = node_attribute_value(c, "clip-rule");
            *evenodd = cr && !strcmp(cr, "evenodd");
        }
        if (!strcasecmp(tg, "path")) {
            const char *d = node_attribute_value(c, "d");
            if (!d) continue;
            int sub0 = t_cp_sub;
            int m = cp_path_points(d, 0.0, 0.0, px + n, py + n, max - n);
            if (m < 0) return -1;
            n += m;
            shapes += t_cp_sub - sub0;
        } else if (!strcasecmp(tg, "rect")) {
            double x = cp_svg_num(c, "x", 1.0), y = cp_svg_num(c, "y", 1.0);
            double w = cp_svg_num(c, "width", 1.0), h = cp_svg_num(c, "height", 1.0);
            if (w <= 0.0 || h <= 0.0 || n + 5 > max) continue;
            double X[5] = { x, x + w, x + w, x, x }, Y[5] = { y, y, y + h, y + h, y };
            for (int i = 0; i < 5; i++) { px[n] = X[i]; py[n] = Y[i]; n++; }
            shapes++;
        } else if (!strcasecmp(tg, "circle") || !strcasecmp(tg, "ellipse")) {
            bool circ = tg[0] == 'c' || tg[0] == 'C';
            double cx = cp_svg_num(c, "cx", 1.0), cy = cp_svg_num(c, "cy", 1.0);
            double rx = cp_svg_num(c, circ ? "r" : "rx", 1.0), ry = circ ? rx : cp_svg_num(c, "ry", 1.0);
            const int segs = 40;
            if (rx <= 0.0 || ry <= 0.0 || n + segs + 1 > max) continue;
            for (int i = 0; i <= segs; i++) {
                double t = 2.0 * 3.14159265358979 * i / segs;
                px[n] = cx + rx * cos(t); py[n] = cy + ry * sin(t); n++;
            }
            shapes++;
        } else if (!strcasecmp(tg, "polygon")) {
            const char *pts = node_attribute_value(c, "points");
            if (!pts) continue;
            int s0 = n;
            for (const char *p = pts;;) {
                while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\t') p++;
                char *e1 = NULL, *e2 = NULL;
                double x = strtod(p, &e1);
                if (e1 == p) break;
                p = e1;
                while (*p == ' ' || *p == ',') p++;
                double y = strtod(p, &e2);
                if (e2 == p) break;
                p = e2;
                if (n + 1 >= max) return -1;
                px[n] = x; py[n] = y; n++;
            }
            if (n - s0 < 3) { n = s0; continue; }
            px[n] = px[s0]; py[n] = py[s0]; n++;
            shapes++;
        }
        /* (the shape's own transform) */
        double cm[6];
        if (n > n0 && cp_svg_xform(c, cm))
            for (int i = n0; i < n; i++) {
                double X = px[i], Y = py[i];
                px[i] = cm[0] * X + cm[2] * Y + cm[4]; py[i] = cm[1] * X + cm[3] * Y + cm[5];
            }
        /* (back to the first shape's start: the joins there and back) */
        if (n0 > 0 && n > n0 && n < max && (px[n - 1] != px[0] || py[n - 1] != py[0])) { px[n] = px[0]; py[n] = py[0]; n++; }
    }
    if (n < 3) return n;
    double pm[6];
    if (cp_svg_xform(cp, pm))
        for (int i = 0; i < n; i++) {
            double X = px[i], Y = py[i];
            px[i] = pm[0] * X + pm[2] * Y + pm[4]; py[i] = pm[1] * X + pm[3] * Y + pm[5];
        }
    for (int i = 0; i < n; i++) { px[i] = bx + px[i] * sx; py[i] = by + py[i] * sy; }
    t_cp_sub = shapes;
    return n;
}

/* The box's clip-path as a mask; false: none it can draw. */
static bool box_clip_mask(const LayoutBox *box, DisplayMask *m)
{
    const char *v = box->s->clip_path;
    if (!v) return false;
    double fs = box->s->font_size > 0.0 ? box->s->font_size : 16.0;
    const LayoutStyle *s = box->s;
    int ref = 1;   /* (the reference box: 0 margin, 1 border, 2 padding, 3 content) */
    char fn[16] = "";
    const char *args = NULL;
    const char *p = v;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (cp_word(&p, "margin-box")) ref = 0;
        else if (cp_word(&p, "border-box") || cp_word(&p, "stroke-box") || cp_word(&p, "view-box")) ref = 1;
        else if (cp_word(&p, "padding-box")) ref = 2;
        else if (cp_word(&p, "content-box") || cp_word(&p, "fill-box")) ref = 3;
        else {
            const char *q = p;
            size_t n = 0;
            while (*q && *q != '(' && *q != ' ' && n < sizeof fn - 1) fn[n++] = (char)tolower((unsigned char)*q++);
            fn[n] = 0;
            if (*q != '(') return false;
            args = q + 1;
            int d = 1;
            q++;
            while (*q && d > 0) { if (*q == '(') d++; else if (*q == ')') d--; q++; }
            p = q;
        }
    }
    if (fn[0] && strcmp(fn, "inset") && strcmp(fn, "rect") && strcmp(fn, "xywh") && strcmp(fn, "circle") && strcmp(fn, "ellipse") &&
        strcmp(fn, "polygon") && strcmp(fn, "path") && strcmp(fn, "url") && strcmp(fn, "shape"))
        return false;
    double ins[4] = { 0, 0, 0, 0 };   /* (the reference box's edges in from the border box: t r b l) */
    if (ref == 0) { ins[0] = -box->margin_top; ins[1] = -box->margin_right; ins[2] = -box->margin_bottom; ins[3] = -box->margin_left; }
    if (ref >= 2) { ins[0] = s->border_top; ins[1] = s->border_right; ins[2] = s->border_bottom; ins[3] = s->border_left; }
    if (ref == 3) { ins[0] += box->padding_top; ins[1] += box->padding_right; ins[2] += box->padding_bottom; ins[3] += box->padding_left; }
    double bx = box->x + ins[3], by = box->y + ins[0], bw = box->width - ins[1] - ins[3], bh = box->height - ins[0] - ins[2];
    if (bw < 0.0) bw = 0.0;
    if (bh < 0.0) bh = 0.0;
    memset(m, 0, sizeof(*m));
    m->kind = 3;
    m->image_handle = -1;
    double x0 = bx, y0 = by, x1 = bx + bw, y1 = by + bh;
    if (!strcmp(fn, "polygon") || !strcmp(fn, "path") || !strcmp(fn, "url") || !strcmp(fn, "shape")) {
        /* (kind 7: its corners from their bounding box's corner, ten a mask) */
        const char *q = args;
        bool evenodd = false;
        while (*q == ' ') q++;
        if (cp_word(&q, "evenodd")) evenodd = true;
        else cp_word(&q, "nonzero");
        while (*q == ' ' || *q == ',') q++;
        double px[CP_POLY_MAX], py[CP_POLY_MAX];
        int n = 0;
        t_cp_n = 0;
        t_cp_sub = 0;
        if (fn[0] == 's') {
            /* (shape(): as path data against the reference box) */
            char pd[2048];
            if (!cp_shape_path(args, bw, bh, fs, pd, sizeof pd, &evenodd)) return false;
            n = cp_path_points(pd, bx, by, px, py, CP_POLY_MAX);
            if (n < 0) return false;
            q = "";
        } else if (fn[0] == 'u') {
            /* (url(#id): an SVG clipPath's shapes, from the border box's corner) */
            n = cp_url_points(args, box->x, box->y, box->width, box->height, px, py, CP_POLY_MAX, &evenodd);
            if (n < 0) return false;
            q = "";
        } else if (fn[1] == 'a') {
            /* (path(): its data from the border box's corner) */
            if (*q != '"' && *q != '\'') return false;
            n = cp_path_points(q + 1, box->x, box->y, px, py, CP_POLY_MAX);
            if (n < 0) return false;
            /* (its numbers are px, zoomed as the box's lengths are) */
            double z = box->s->zoom > 0.0f ? box->s->zoom : 1.0;
            if (z != 1.0)
                for (int k = 0; k < n; k++) { px[k] = box->x + (px[k] - box->x) * z; py[k] = box->y + (py[k] - box->y) * z; }
            q = "";
        }
        while (*q && *q != ')') {
            double X, Y;
            if (!cp_len(&q, bw, fs, &X) || !cp_len(&q, bh, fs, &Y)) break;
            if (n >= CP_POLY_MAX) return false;
            px[n] = bx + X; py[n] = by + Y; n++;
            while (*q == ' ') q++;
            if (*q == ',') q++;
        }
        if (n < 3) { m->kind = 3; m->x = (float)bx; m->y = (float)by; m->w = m->h = 0.0f; return true; }
        double lx = px[0], ly = py[0], hx = px[0], hy = py[0];
        for (int k = 1; k < n; k++) { lx = fmin(lx, px[k]); ly = fmin(ly, py[k]); hx = fmax(hx, px[k]); hy = fmax(hy, py[k]); }
        m->kind = 7;
        m->x = (float)lx; m->y = (float)ly; m->w = (float)(hx - lx); m->h = (float)(hy - ly);
        float *slot[20];
        for (int k = 0; k < 4; k++) { slot[k] = &m->rx[k]; slot[4 + k] = &m->ry[k]; slot[8 + k] = &m->border[k]; slot[12 + k] = &m->ex_rx[k]; slot[16 + k] = &m->ex_ry[k]; }
        for (int k = 0; k < n && k < 10; k++) { *slot[2 * k] = (float)(px[k] - lx); *slot[2 * k + 1] = (float)(py[k] - ly); }
        for (int k = 0; k < n; k++) { t_cp_px[k] = (float)(px[k] - lx); t_cp_py[k] = (float)(py[k] - ly); }
        t_cp_n = n;
        m->blur = (float)n;
        m->angle_deg = evenodd ? 1.0f : 0.0f;
        m->repeat = t_cp_sub > 1 ? 1 : 0;   /* (subpaths: the edges joining them aren't edges) */
        m->nstops = 1;
        return true;
    }
    if (!fn[0]) {
        /* (the box alone: its border radii, grown or shrunk to that box) */
        for (int c = 0; c < 4; c++) {
            bool pct = (s->radius_pct >> c) & 1;
            double rx = pct ? s->radius[c] * box->width : s->radius[c], ry = LAYOUT_RADIUS_Y(s, c, box->height);
            double ix = (c == 0 || c == 3) ? ins[3] : ins[1], iy = (c == 0 || c == 1) ? ins[0] : ins[2];
            if (rx > 0.0) m->rx[c] = (float)(rx - ix > 0.0 ? rx - ix : 0.0);
            if (ry > 0.0) m->ry[c] = (float)(ry - iy > 0.0 ? ry - iy : 0.0);
        }
    } else if (!strcmp(fn, "inset")) {
        double u[4];
        int k = 0;
        const char *q = args;
        while (k < 4) {
            const char *r = q;
            if (cp_word(&r, "round")) break;
            if (!cp_len(&q, (k % 2 == 0) ? bh : bw, fs, &u[k])) break;
            k++;
        }
        if (k == 0) return false;
        if (k == 1) {
            /* (one value: top and bottom of the height, left and right of the width) */
            const char *q2 = args;
            double ww;
            cp_len(&q2, bw, fs, &ww);
            u[1] = u[3] = ww;
            u[2] = u[0];
        } else if (k == 2) { u[2] = u[0]; u[3] = u[1]; }
        else if (k == 3) u[3] = u[1];
        x0 = bx + u[3]; y0 = by + u[0]; x1 = bx + bw - u[1]; y1 = by + bh - u[2];
    } else if (!strcmp(fn, "rect")) {
        /* rect(top right bottom left) from the box's top left; auto: its edge */
        double t[4] = { 0.0, bw, bh, 0.0 };
        const char *q = args;
        for (int k = 0; k < 4; k++) {
            if (cp_word(&q, "auto")) continue;
            if (!cp_len(&q, (k % 2 == 0) ? bh : bw, fs, &t[k])) return false;
        }
        x0 = bx + t[3]; y0 = by + t[0]; x1 = bx + t[1]; y1 = by + t[2];
    } else if (!strcmp(fn, "xywh")) {
        double X, Y, W, H;
        const char *q = args;
        if (!cp_len(&q, bw, fs, &X) || !cp_len(&q, bh, fs, &Y) || !cp_len(&q, bw, fs, &W) || !cp_len(&q, bh, fs, &H)) return false;
        x0 = bx + X; y0 = by + Y; x1 = x0 + W; y1 = y0 + H;
    } else {
        bool circ = fn[0] == 'c';
        const char *at = NULL;
        for (const char *z = args; *z && *z != ')'; z++)
            if ((z == args || z[-1] == ' ') && !strncasecmp(z, "at ", 3)) { at = z + 3; break; }
        double cx = bw / 2.0, cy = bh / 2.0;
        if (at) cp_position(at, bw, bh, fs, &cx, &cy);
        double dx1 = fabs(cx), dx2 = fabs(bw - cx), dy1 = fabs(cy), dy2 = fabs(bh - cy);
        double rx = -1.0, ry = -1.0;
        const char *q = args;
        double cxs = fmin(dx1, dx2), cys = fmin(dy1, dy2), fxs = fmax(dx1, dx2), fys = fmax(dy1, dy2);
        if (circ) {
            double diag = sqrt((bw * bw + bh * bh) / 2.0);
            if (cp_word(&q, "closest-side")) rx = fmin(cxs, cys);
            else if (cp_word(&q, "farthest-side")) rx = fmax(fxs, fys);
            else if (cp_word(&q, "closest-corner")) rx = sqrt(cxs * cxs + cys * cys);
            else if (cp_word(&q, "farthest-corner")) rx = sqrt(fxs * fxs + fys * fys);
            else if (!cp_len(&q, diag, fs, &rx)) rx = fmin(cxs, cys);
            ry = rx;
        } else if (cp_word(&q, "closest-corner")) { rx = cxs * sqrt(2.0); ry = cys * sqrt(2.0); }
        else if (cp_word(&q, "farthest-corner")) { rx = fxs * sqrt(2.0); ry = fys * sqrt(2.0); }
        else {
            if (!cp_radius(&q, bw, fs, dx1, dx2, &rx)) rx = cxs;
            if (!cp_radius(&q, bh, fs, dy1, dy2, &ry)) ry = cys;
        }
        if (rx < 0.0) rx = 0.0;
        if (ry < 0.0) ry = 0.0;
        x0 = bx + cx - rx; x1 = bx + cx + rx; y0 = by + cy - ry; y1 = by + cy + ry;
        for (int c = 0; c < 4; c++) { m->rx[c] = (float)rx; m->ry[c] = (float)ry; }
    }
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    if (fn[0] && strcmp(fn, "circle") && strcmp(fn, "ellipse") && args) {
        const char *rq = NULL;
        for (const char *z = args; *z && *z != ')'; z++)
            if ((z == args || z[-1] == ' ') && !strncasecmp(z, "round", 5)) { rq = z + 5; break; }
        if (rq) cp_round(rq, x1 - x0, y1 - y0, fs, m);
    }
    m->x = (float)x0; m->y = (float)y0; m->w = (float)(x1 - x0); m->h = (float)(y1 - y0);
    return true;
}

/* border-radius as a rounded-rectangle mask over the border box; false for
 * square corners.  Overlapping radii scale down together (CSS Backgrounds
 * §5.5). */
static bool box_round_mask(const LayoutBox *box, DisplayMask *m)
{
    if (box->s->radius[0] <= 0.0f && box->s->radius[1] <= 0.0f && box->s->radius[2] <= 0.0f && box->s->radius[3] <= 0.0f)
        return false;
    memset(m, 0, sizeof(*m));
    m->kind = 3;
    m->image_handle = -1;
    m->x = (float)box->x; m->y = (float)box->y;
    m->w = (float)box->width; m->h = (float)box->height;
    if (m->w <= 0.0f || m->h <= 0.0f) return false;
    for (int c = 0; c < 4; c++) {
        bool pct = (box->s->radius_pct >> c) & 1;
        m->rx[c] = pct ? box->s->radius[c] * m->w : box->s->radius[c];
        m->ry[c] = LAYOUT_RADIUS_Y(box->s, c, m->h);
    }
    float f = 1.0f;
    float sums[4] = { m->rx[0] + m->rx[1], m->ry[1] + m->ry[2], m->rx[2] + m->rx[3], m->ry[3] + m->ry[0] };
    float sides[4] = { m->w, m->h, m->w, m->h };
    for (int k = 0; k < 4; k++) if (sums[k] > sides[k] && sums[k] > 0.0f && sides[k] / sums[k] < f) f = sides[k] / sums[k];
    for (int c = 0; c < 4; c++) { m->rx[c] *= f; m->ry[c] *= f; }
    /* (corner-shape: K for each corner in stop_pos[4..7], (K + 16.5) / 33; 0 round) */
    if (box->s->corner_shaped)
        for (int c = 0; c < 4; c++)
            m->stop_pos[4 + c] = (box->s->corner_shaped >> c & 1) ? (box->s->corner_k[c] + 16.5f) / 33.0f : 0.0f;
    return true;
}

static bool is_replaced_tag(const HtmlNode *n)
{
    if (!n || n->type != HTML_NODE_ELEMENT || !n->tag_name) return false;
    return strcasecmp(n->tag_name, "img") == 0 || display_list_object_image(n) || strcasecmp(n->tag_name, "video") == 0 ||
           strcasecmp(n->tag_name, "svg") == 0 || strcasecmp(n->tag_name, "canvas") == 0 ||
           strcasecmp(n->tag_name, "iframe") == 0;
}

/* One past the last layout index in box i's subtree (preorder numbering of
 * the document-built boxes; pseudo-element boxes, numbered after them, are
 * in their element's subtree by mask_covers). */
/* The last document-built child of e, looking into added boxes (an
 * anonymous table box wraps document-built ones); -1 for none. */
static int last_base_child(const LayoutContext *ctx, int e, int base)
{
    int last = -1;
    for (int c = ctx->tree.nodes[e].first_child_idx; c >= 0; c = ctx->tree.nodes[c].next_sibling_idx) last = c;
    for (int c = last; c >= 0; c = ctx->tree.nodes[c].prev_sibling_idx) {
        if (c < base) return c;
        int b = last_base_child(ctx, c, base);
        if (b >= 0) return b;
    }
    return -1;
}

static int subtree_end(const LayoutContext *ctx, int i)
{
    int base = ctx->base_count > 0 && ctx->base_count < ctx->tree.count ? ctx->base_count : ctx->tree.count;
    int e = i;
    if (i >= base) {
        for (;;) {
            int c = ctx->tree.nodes[e].first_child_idx, last = -1;
            for (; c >= 0; c = ctx->tree.nodes[c].next_sibling_idx) last = c;
            if (last < 0) break;
            e = last;
        }
        return e + 1;
    }
    for (int c; (c = last_base_child(ctx, e, base)) >= 0;) e = c;
    return e + 1;
}

/* Is box i in the subtree [start, end) of a mask (a pseudo-element box: by
 * the document-built box it hangs under)? */
static bool mask_covers(const LayoutContext *ctx, int i, int start, int end)
{
    if (start <= i && i < end) return true;
    int base = ctx->base_count;
    if (base <= 0 || i < base || start >= base) return false;
    while (i >= base) i = ctx->tree.nodes[i].parent_idx;
    return start <= i && i < end;
}

/* Where scrolling pushes sticky box a (CSS Position 3 3.4): its insets kept
 * inside the nearest scrollport (the viewport's, scrolled by vx, vy, when
 * none), and itself kept inside its containing block (a table part's: the
 * table).  As layout_api.c's lq_sticky_one, at paint. */
static void dl_sticky_shift(LayoutContext *ctx, int a, float vx, float vy, float *ox, float *oy)
{
    *ox = *oy = 0.0f;
    const LayoutBox *b = &ctx->boxes[a];
    unsigned sides = b->s->positioned_sides;
    if (!sides) return;
    int s = -1;
    bool in_fixed = false;
    for (int p = ctx->tree.nodes[a].parent_idx; p >= 0; p = ctx->tree.nodes[p].parent_idx) {
        /* (inside a fixed box: the viewport's, which doesn't scroll it) */
        if (ctx->boxes[p].s->position == CSS_POSITION_FIXED) { in_fixed = true; break; }
        if (!ctx->boxes[p].s->overflow) continue;
        HtmlNode *pn = ctx->doc ? css_layout_box_node(ctx, p) : NULL;
        if (pn && pn->type == HTML_NODE_ELEMENT && pn->tag_name &&
            (!strcasecmp(pn->tag_name, "html") || !strcasecmp(pn->tag_name, "body"))) break;
        s = p;
        break;
    }
    double vx0, vy0, vw, vh;
    if (s >= 0) {
        const LayoutBox *S = &ctx->boxes[s];
        float sx = 0.0f, sy = 0.0f;
        css_layout_box_offset(ctx, s, &sx, &sy);
        vx0 = S->x + S->s->border_left + S->padding_left + sx;
        vy0 = S->y + S->s->border_top + S->padding_top + sy;
        vw = S->width - S->s->border_left - S->s->border_right - S->padding_left - S->padding_right;
        vh = S->height - S->s->border_top - S->s->border_bottom - S->padding_top - S->padding_bottom;
    } else {
        vx0 = in_fixed ? 0.0 : vx; vy0 = in_fixed ? 0.0 : vy; vw = ctx->viewport_width; vh = ctx->viewport_height;
    }
    int par = ctx->tree.nodes[a].parent_idx;
    /* (in a table or row not a cell: an anonymous cell around it, just its size) */
    if (par >= 0 && b->s->table_role == 0 && ctx->boxes[par].s->table_role >= 1 && ctx->boxes[par].s->table_role <= 3) return;
    if (b->s->table_role >= 2 && b->s->table_role <= 4)
        while (par >= 0 && ctx->boxes[par].s->table_role >= 2 && ctx->boxes[par].s->table_role <= 4 && ctx->tree.nodes[par].parent_idx >= 0)
            par = ctx->tree.nodes[par].parent_idx;
    if (par < 0) return;
    const LayoutBox *C = &ctx->boxes[par];
    double cx0 = C->x + C->s->border_left + C->padding_left, cy0 = C->y + C->s->border_top + C->padding_top;
    double cx1 = cx0 + C->content_width, cy1 = cy0 + C->content_height;
    if (par == s) {
        /* (the scroller itself: as far back as its content starts) */
        double l0 = C->x + C->s->border_left, t0 = C->y + C->s->border_top, l = l0, t = t0;
        css_layout_extent_start(ctx, s, &l, &t);
        cx0 += l - l0;
        cy0 += t - t0;
    }
    double bx0 = b->x - b->margin_left, by0 = b->y - b->margin_top;
    double bx1 = b->x + b->width + b->margin_right, by1 = b->y + b->height + b->margin_bottom;
    double dy = 0.0, dx = 0.0;
    if (sides & LAYOUT_SIDE_TOP) {
        double want = vy0 + b->s->top - b->y;
        if (want > dy) { dy = want; if (by1 + dy > cy1) dy = cy1 - by1 > 0.0 ? cy1 - by1 : 0.0; }
    }
    if (sides & LAYOUT_SIDE_BOTTOM) {
        double want = vy0 + vh - b->s->bottom - (b->y + b->height);
        if (want < dy) { dy = want; if (by0 + dy < cy0) dy = cy0 - by0 < 0.0 ? cy0 - by0 : 0.0; }
    }
    if (sides & LAYOUT_SIDE_LEFT) {
        double want = vx0 + b->s->left - b->x;
        if (want > dx) { dx = want; if (bx1 + dx > cx1) dx = cx1 - bx1 > 0.0 ? cx1 - bx1 : 0.0; }
    }
    if (sides & LAYOUT_SIDE_RIGHT) {
        double want = vx0 + vw - b->s->right - (b->x + b->width);
        if (want < dx) { dx = want; if (bx0 + dx < cx0) dx = cx0 - bx0 < 0.0 ? cx0 - bx0 : 0.0; }
    }
    *ox = (float)dx;
    *oy = (float)dy;
}

void display_list_sticky_shift(LayoutContext *ctx, int a, float *ox, float *oy) { dl_sticky_shift(ctx, a, 0.0f, 0.0f, ox, oy); }

/* text-emphasis: its mark at half the size over (or under) each character
 * of [start, end) drawn from x, y (its em box's top), centered on the
 * character's advance; spaces and punctuation get none.  Where a ruby
 * annotation would go: a line of the mark's own above the content area. */
/* Whether a character takes no emphasis mark (separators, controls, format
 * characters, noncharacters, common punctuation). */
static bool dl_no_emphasis(const char *t, size_t n)
{
    const unsigned char *u = (const unsigned char *)t;
    uint32_t cp = n == 2 ? ((u[0] & 0x1Fu) << 6) | (u[1] & 0x3Fu)
                : n == 3 ? ((u[0] & 0x0Fu) << 12) | ((u[1] & 0x3Fu) << 6) | (u[2] & 0x3Fu)
                : n == 4 ? ((u[0] & 0x07u) << 18) | ((u[1] & 0x3Fu) << 12) | ((u[2] & 0x3Fu) << 6) | (u[3] & 0x3Fu) : 0;
    static const uint32_t r[][2] = {
        {0x80, 0xA0}, {0xA1, 0xA1}, {0xA7, 0xA7}, {0xAB, 0xAB}, {0xAD, 0xAD}, {0xB6, 0xB7}, {0xBB, 0xBB}, {0xBF, 0xBF},
        {0x600, 0x605}, {0x61C, 0x61C}, {0x6DD, 0x6DD}, {0x70F, 0x70F}, {0x1680, 0x1680}, {0x180E, 0x180E},
        {0x2000, 0x206F}, {0x3000, 0x3003}, {0x3008, 0x3011}, {0x3014, 0x301F}, {0xFDD0, 0xFDEF}, {0xFEFF, 0xFEFF},
        {0xFF01, 0xFF03}, {0xFF05, 0xFF0A}, {0xFF0C, 0xFF0F}, {0xFF1A, 0xFF1B}, {0xFF1F, 0xFF20}, {0xFF3B, 0xFF3D},
        {0xFF3F, 0xFF3F}, {0xFF5B, 0xFF5B}, {0xFF5D, 0xFF5D}, {0xFF5F, 0xFF65}, {0xFFF9, 0xFFFB}, {0x110BD, 0x110BD},
        {0x1BCA0, 0x1BCA3}, {0x1D173, 0x1D17A}, {0xE0001, 0xE007F},
    };
    if ((cp & 0xFFFE) == 0xFFFE) return true;
    for (size_t i = 0; i < sizeof r / sizeof r[0]; i++)
        if (cp >= r[i][0] && cp <= r[i][1]) return true;
    return false;
}

/* ---- custom highlights: the segments script set (display_list_highlights_set) ---- */
typedef struct { uint32_t hl, node, s16, e16; } DlHlSeg;
static pthread_mutex_t g_hl_mu = PTHREAD_MUTEX_INITIALIZER;
static DlHlSeg *g_hl_seg;
static int g_hl_nseg, g_hl_n;
static char (*g_hl_name)[96];
static int *g_hl_prio;

void display_list_highlights_set(const char *const *names, const int *prios, int nh, const uint32_t *seg, int nseg)
{
    pthread_mutex_lock(&g_hl_mu);
    free(g_hl_seg); free(g_hl_name); free(g_hl_prio);
    g_hl_seg = NULL; g_hl_name = NULL; g_hl_prio = NULL; g_hl_nseg = g_hl_n = 0;
    if (nh > 0 && nseg > 0) {
        g_hl_seg = (DlHlSeg *)malloc((size_t)nseg * sizeof(DlHlSeg));
        g_hl_name = (char (*)[96])calloc((size_t)nh, 96);
        g_hl_prio = (int *)calloc((size_t)nh, sizeof(int));
        if (g_hl_seg && g_hl_name && g_hl_prio) {
            for (int i = 0; i < nh; i++) { snprintf(g_hl_name[i], 96, "%s", names[i] ? names[i] : ""); g_hl_prio[i] = prios ? prios[i] : 0; }
            for (int i = 0; i < nseg; i++) {
                g_hl_seg[i].hl = seg[4 * i]; g_hl_seg[i].node = seg[4 * i + 1];
                g_hl_seg[i].s16 = seg[4 * i + 2]; g_hl_seg[i].e16 = seg[4 * i + 3];
            }
            g_hl_nseg = nseg; g_hl_n = nh;
        }
    }
    pthread_mutex_unlock(&g_hl_mu);
}

/* (a UTF-16 offset in a text node's own text as a byte offset in the text
 * the layout painted: white space it collapsed skipped) */
static size_t dl_hl_painted_at(const char *raw, const char *painted, uint32_t off16)
{
    size_t i = 0, j = 0, pn = strlen(painted);
    uint32_t u = 0;
    while (raw[i] && u < off16) {
        unsigned char c = (unsigned char)raw[i];
        size_t n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        u += n == 4 ? 2 : 1;
        bool ws = c == ' ' || c == 9 || c == 10 || c == 13 || c == 12;
        if (j < pn && !memcmp(raw + i, painted + j, n)) { i += n; j += n; continue; }
        if (ws) { i += n; continue; }   /* (collapsed away) */
        unsigned char pc = (unsigned char)painted[j];
        size_t pnn = pc < 0x80 ? 1 : pc < 0xE0 ? 2 : pc < 0xF0 ? 3 : 4;
        i += n;
        if (j < pn) j += pnn;
    }
    return j > pn ? pn : j;
}

typedef struct { int set_c, set_bg; double c[4], bg[4]; char rc[160], rbg[160]; int nv; char vn[8][48], vv[8][160];
                 const LayoutContext *ctx; int el;
                 char rtd[160];                      /* (text-decoration as declared) */
                 unsigned char td_lines, td_style;   /* (its lines and style; td_c its color, set when td_cset) */
                 int td_cset; double td_c[4];
                 char rtdc[160], rtds[32], rth[64], ruo[64];   /* (decoration color, style, thickness, underline offset as declared) */
                 int th_set, uo_set; double th, uo;
                 char rtsh[256]; } DlHlStyle;   /* (text-shadow as declared) */
static void dl_hl_decl(void *ud, const char *prop, const char *value, bool important)
{
    (void)important;
    DlHlStyle *st = (DlHlStyle *)ud;
    if (prop[0] == 1) { st->nv = 0; return; }   /* (custom properties: only the element's own pseudo's) */
    if (prop[0] == '-' && prop[1] == '-') {
        int k = 0;
        while (k < st->nv && strcmp(st->vn[k], prop)) k++;
        if (k == st->nv && st->nv < 8) st->nv++;
        if (k < 8) { snprintf(st->vn[k], sizeof st->vn[k], "%s", prop); snprintf(st->vv[k], sizeof st->vv[k], "%s", value); }
        return;
    }
    if (!strcasecmp(prop, "text-decoration") || !strcasecmp(prop, "text-decoration-line")) snprintf(st->rtd, sizeof st->rtd, "%s", value);
    else if (!strcasecmp(prop, "text-decoration-color")) snprintf(st->rtdc, sizeof st->rtdc, "%s", value);
    else if (!strcasecmp(prop, "text-decoration-style")) snprintf(st->rtds, sizeof st->rtds, "%s", value);
    else if (!strcasecmp(prop, "text-decoration-thickness")) snprintf(st->rth, sizeof st->rth, "%s", value);
    else if (!strcasecmp(prop, "text-underline-offset")) snprintf(st->ruo, sizeof st->ruo, "%s", value);
    else if (!strcasecmp(prop, "color")) snprintf(st->rc, sizeof st->rc, "%s", value);
    else if (!strcasecmp(prop, "background-color") || !strcasecmp(prop, "background")) snprintf(st->rbg, sizeof st->rbg, "%s", value);
    else if (!strcasecmp(prop, "text-shadow")) snprintf(st->rtsh, sizeof st->rtsh, "%s", value);
}

static bool dl_hl_subst(const DlHlStyle *st, const char *in, char *out, size_t cap, int depth);
/* (a highlight's text-shadow over the text [s, e) at px: the last shadow
 * first; currentColor the piece's color cc; blurred ones through a blur filter) */
static bool dl_hl_shadows(DisplayList *dl, const DlHlStyle *st, TextShaper *font, const char *run_text, size_t s, size_t e,
                          float px, float y, float scale, float w, double fs, const float cc[4], float op)
{
    char v[256];
    if (!dl_hl_subst(st, st->rtsh, v, sizeof v, 0)) return true;
    float sx[4], sy[4], sb[4], sc[4][4];
    int n = 0;
    for (const char *p = v; *p && n < 4;) {
        char sh[160];
        size_t k = 0;
        int depth = 0;
        while (*p && (depth > 0 || *p != ',')) {
            if (*p == '(') depth++;
            else if (*p == ')') depth--;
            if (k + 1 < sizeof sh) sh[k++] = *p;
            p++;
        }
        sh[k] = 0;
        if (*p == ',') p++;
        double len[3] = { 0, 0, 0 };
        int nl = 0;
        float col[4] = { cc[0], cc[1], cc[2], cc[3] };
        for (const char *q = sh; *q;) {
            while (isspace((unsigned char)*q)) q++;
            if (!*q) break;
            char tok[96];
            size_t tl = 0;
            int dp = 0;
            while (*q && (dp > 0 || !isspace((unsigned char)*q))) {
                if (*q == '(') dp++;
                else if (*q == ')') dp--;
                if (tl + 1 < sizeof tok) tok[tl++] = *q;
                q++;
            }
            tok[tl] = 0;
            double c[4];
            bool num = isdigit((unsigned char)tok[0]) || ((tok[0] == '-' || tok[0] == '+' || tok[0] == '.') && tok[1]) || !strncasecmp(tok, "calc(", 5);
            if (num && nl < 3) len[nl++] = css_layout_hl_length(st->ctx, st->el, tok, fs);
            else if (!strcasecmp(tok, "currentcolor")) { col[0] = cc[0]; col[1] = cc[1]; col[2] = cc[2]; col[3] = cc[3]; }
            else if (css_color_parse(tok, c)) { col[0] = (float)(c[0] / 255.0); col[1] = (float)(c[1] / 255.0); col[2] = (float)(c[2] / 255.0); col[3] = (float)c[3] * op; }
        }
        if (nl < 2) continue;
        sx[n] = (float)len[0]; sy[n] = (float)len[1]; sb[n] = nl > 2 && len[2] > 0.0 ? (float)len[2] : 0.0f;
        memcpy(sc[n], col, sizeof col);
        n++;
    }
    for (int k = n - 1; k >= 0; k--) {
        if (sc[k][3] <= 0.0f) continue;
        int smi = -1;
        DisplayMask sm;
        if (sb[k] > 0.0f) {
            memset(&sm, 0, sizeof sm);
            sm.kind = 9;
            sm.image_handle = -1;
            float pad = sb[k] * 2.0f + 2.0f;
            sm.x = px + sx[k] - pad; sm.y = y + sy[k] - pad;
            sm.w = w + 2.0f * pad; sm.h = (float)fs * 1.5f + 2.0f * pad;
            sm.nstops = 1;
            sm.stop_pos[0] = 9.0f / 32.0f;
            sm.rx[0] = sb[k] / 2.0f;
            smi = dl_add_mask(dl, &sm);
            if (smi >= 0 && !dl_add_mask_cmd(dl, DL_MASK_BEGIN, smi, &sm)) smi = -1;
        }
        if (!text_shaper_shape_span(font, run_text, s, e, px + sx[k], y + sy[k], scale, false, sc[k][0], sc[k][1], sc[k][2], sc[k][3], dl))
            return false;
        if (smi >= 0) dl_add_mask_cmd(dl, DL_MASK_END, smi, &sm);
    }
    return true;
}

/* (a declared value with its var()s put in: the pseudo's own custom
 * properties, else the fallback; false: one unresolved) */
static bool dl_hl_subst(const DlHlStyle *st, const char *in, char *out, size_t cap, int depth)
{
    size_t o = 0;
    for (const char *p = in; *p; ) {
        if (!strncasecmp(p, "var(", 4) && depth < 8) {
            const char *q = p + 4;
            while (*q == ' ') q++;
            const char *n0 = q;
            while (*q && *q != ',' && *q != ')' && *q != ' ') q++;
            char name[48];
            snprintf(name, sizeof name, "%.*s", (int)(q - n0), n0);
            while (*q == ' ') q++;
            const char *fb = NULL, *end = q;
            int dp = 0;
            if (*q == ',') { fb = q + 1; }
            for (end = q; *end; end++) {
                if (*end == '(') dp++;
                else if (*end == ')') { if (!dp) break; dp--; }
            }
            char rep[160] = "";
            int k = 0;
            while (k < st->nv && strcmp(st->vn[k], name)) k++;
            const char *ev = k < st->nv ? NULL : css_layout_custom_prop(st->ctx, st->el, name);
            if (k < st->nv) {
                if (!dl_hl_subst(st, st->vv[k], rep, sizeof rep, depth + 1)) return false;
            } else if (ev) {
                if (!dl_hl_subst(st, ev, rep, sizeof rep, depth + 1)) return false;
            } else if (fb) {
                char f[160];
                snprintf(f, sizeof f, "%.*s", (int)(end - fb), fb);
                if (!dl_hl_subst(st, f, rep, sizeof rep, depth + 1)) return false;
            } else return false;
            for (const char *r = rep; *r && o + 1 < cap; r++) out[o++] = *r;
            p = *end ? end + 1 : end;
            continue;
        }
        if (o + 1 < cap) out[o++] = *p;
        p++;
    }
    out[o] = 0;
    return true;
}

static void dl_hl_finish(DlHlStyle *st, double fs)
{
    char v[160];
    double c[4];
    if (st->rtdc[0] && dl_hl_subst(st, st->rtdc, v, sizeof v, 0) && css_color_parse(v, c)) { memcpy(st->td_c, c, sizeof c); st->td_cset = 1; }
    if (st->rtds[0] && dl_hl_subst(st, st->rtds, v, sizeof v, 0)) {
        static const char *const sk2[] = { "solid", "double", "dotted", "dashed", "wavy" };
        for (int k = 0; k < 5; k++) if (!strcasecmp(v, sk2[k])) st->td_style = (unsigned char)k;
    }
    if (st->rth[0] && dl_hl_subst(st, st->rth, v, sizeof v, 0) && strcasecmp(v, "auto") && strcasecmp(v, "from-font")) {
        st->th = css_layout_hl_length(st->ctx, st->el, v, fs);
        st->th_set = 1;
    }
    if (st->ruo[0] && dl_hl_subst(st, st->ruo, v, sizeof v, 0) && strcasecmp(v, "auto")) {
        st->uo = css_layout_hl_length(st->ctx, st->el, v, fs);
        st->uo_set = 1;
    }
    /* (currentcolor: the color the layers below come to, so none of its own) */
    /* (currentcolor: the color the layers below come to, so none of its own) */
    if (st->rc[0] && dl_hl_subst(st, st->rc, v, sizeof v, 0) && strcasecmp(v, "currentcolor") && css_color_parse(v, c)) { memcpy(st->c, c, sizeof c); st->set_c = 1; }
    if (st->rbg[0] && dl_hl_subst(st, st->rbg, v, sizeof v, 0) && css_color_parse(v, c)) { memcpy(st->bg, c, sizeof c); st->set_bg = 1; }
    if (st->rtd[0] && dl_hl_subst(st, st->rtd, v, sizeof v, 0)) {
        /* (the lines, a style keyword, a color: the shorthand's tokens) */
        static const char *const sk[] = { "solid", "double", "dotted", "dashed", "wavy" };
        for (char *tok = strtok(v, " \t"); tok; tok = strtok(NULL, " \t")) {
            if (!strcasecmp(tok, "underline")) st->td_lines |= 1;
            else if (!strcasecmp(tok, "line-through")) st->td_lines |= 2;
            else if (!strcasecmp(tok, "overline")) st->td_lines |= 4;
            else {
                bool kw = false;
                for (int k = 0; k < 5; k++) if (!strcasecmp(tok, sk[k])) { st->td_style = (unsigned char)k; kw = true; }
                if (!kw && strcasecmp(tok, "none") && css_color_parse(tok, c)) { memcpy(st->td_c, c, sizeof c); st->td_cset = 1; }
            }
        }
    }
}

/* (a text fragment [start, end) of box i's painted text, with the custom
 * highlights over it: the highlighted pieces' backgrounds, each piece in its
 * color; false: none over it, nothing drawn) */
static bool dl_highlight_frag(DisplayList *dl, LayoutContext *ctx, int i, HtmlNode *node, TextShaper *font, const char *run_text,
                              size_t start, size_t end, float x, float y, float scale, bool rtl, float r, float g, float b, float a,
                              float op, bool *ok)
{
    *ok = true;
    if (!g_hl_nseg || !node || !node->store_id || rtl) return false;
    enum { MAXP = 32 };
    size_t ps[MAXP], pe[MAXP];
    int ph[MAXP], prio[MAXP], np = 0;
    char names[MAXP][104];
    const char *raw = css_layout_node_text(ctx, node);
    pthread_mutex_lock(&g_hl_mu);
    for (int k = 0; k < g_hl_nseg && np < MAXP; k++) {
        if (g_hl_seg[k].node != node->store_id || g_hl_seg[k].hl >= (uint32_t)g_hl_n) continue;
        size_t s = raw ? dl_hl_painted_at(raw, run_text, g_hl_seg[k].s16) : 0;
        size_t e = raw ? dl_hl_painted_at(raw, run_text, g_hl_seg[k].e16) : 0;
        if (s < start) s = start;
        if (e > end) e = end;
        if (e <= s) continue;
        ps[np] = s; pe[np] = e; ph[np] = (int)g_hl_seg[k].hl; prio[np] = g_hl_prio[g_hl_seg[k].hl];
        const char *hn = g_hl_name[g_hl_seg[k].hl];
        if (hn[0] == 1) snprintf(names[np], sizeof names[np], "%s", hn + 1);   /* (the selection: its own pseudo) */
        else snprintf(names[np], sizeof names[np], "highlight(%s)", hn);
        np++;
    }
    pthread_mutex_unlock(&g_hl_mu);
    if (!np) return false;
    /* (the element's highlight styles) */
    int par = ctx->tree.nodes[i].parent_idx;
    /* (past anonymous boxes: the element the text is in) */
    while (par >= 0) {
        const HtmlNode *pn = css_layout_box_node(ctx, par);
        if (pn && pn->type == HTML_NODE_ELEMENT && ctx->tree.nodes[par].dom_node_idx >= 0) break;
        par = ctx->tree.nodes[par].parent_idx;
    }
    int dom = par >= 0 ? ctx->tree.nodes[par].dom_node_idx : -1;
    DlHlStyle st[MAXP];
    for (int k = 0; k < np; k++) {
        memset(&st[k], 0, sizeof st[k]);
        st[k].ctx = ctx;
        st[k].el = par;
        css_hl_marks = true;
        if (dom >= 0) css_layout_highlight_cascade(ctx, dom, names[k], dl_hl_decl, &st[k]);
        css_hl_marks = false;
        dl_hl_finish(&st[k], ctx->boxes[i].s->font_size > 0.0 ? ctx->boxes[i].s->font_size : 16.0);
        if (!strcmp(names[k], "selection") && !st[k].set_bg && !st[k].set_c) {
            /* (no ::selection: the system's Highlight and HighlightText) */
            st[k].set_bg = st[k].set_c = 1;
            st[k].bg[0] = 181; st[k].bg[1] = 213; st[k].bg[2] = 255; st[k].bg[3] = 1;
            st[k].c[0] = st[k].c[1] = st[k].c[2] = 0; st[k].c[3] = 1;
        }
    }
    /* (cut points: every piece's ends) */
    size_t cut[2 * MAXP + 2];
    int nc = 0;
    cut[nc++] = start;
    for (int k = 0; k < np; k++) { cut[nc++] = ps[k]; cut[nc++] = pe[k]; }
    cut[nc++] = end;
    for (int p = 1; p < nc; p++)
        for (int q = p; q > 0 && cut[q - 1] > cut[q]; q--) { size_t t = cut[q]; cut[q] = cut[q - 1]; cut[q - 1] = t; }
    char buf[1024];
    float va = 0.8f, vd = 0.2f, vg = 0.0f;
    text_shaper_vmetrics(font, &va, &vd, &vg);
    double fs = scale * 16.0;
    float hh = roundf(va * (float)fs) + roundf(vd * (float)fs);
    float hy = y;
    {
        /* (in an inline box, on its first line: that box's content area, as its background is) */
        int pi = ctx->tree.nodes[i].parent_idx;
        const LayoutBox *pb = pi >= 0 ? &ctx->boxes[pi] : NULL;
        if (pb && pb->s->display == CSS_DISPLAY_INLINE && !pb->s->inline_level && pb->height > 0.0) {
            float ct = (float)(pb->y + pb->s->border_top + pb->padding_top);
            float ch = (float)(pb->height - pb->s->border_top - pb->padding_top - pb->s->border_bottom - pb->padding_bottom);
            if (fabsf(y - ct) <= 4.0f && ch > 0.0f && fabsf(ch - hh) <= 3.0f) { hy = ct; hh = ch; }
        }
    }
    for (int pass = 0; pass < 3; pass++) {   /* (backgrounds, shadows, text) */
        for (int c = 0; c + 1 < nc; c++) {
            size_t s = cut[c], e = cut[c + 1];
            if (e <= s) continue;
            bool any = false;
            for (int k = 0; k < np; k++) if (ps[k] <= s && pe[k] >= e) any = true;
            size_t l0 = s - start < sizeof buf - 1 ? s - start : sizeof buf - 1;
            memcpy(buf, run_text + start, l0);
            buf[l0] = 0;
            float w0 = 0.0f, w1 = 0.0f, hgt;
            text_shaper_measure(font, buf, &w0, &hgt);
            float px = x + w0 * scale;
            if (pass == 0) {
                /* (backgrounds first, every highlight's, in registry order) */
                if (!any) continue;
                size_t l1 = e - start < sizeof buf - 1 ? e - start : sizeof buf - 1;
                memcpy(buf, run_text + start, l1);
                buf[l1] = 0;
                text_shaper_measure(font, buf, &w1, &hgt);
                int ord[MAXP];
                for (int k = 0; k < np; k++) ord[k] = k;
                for (int p2 = 1; p2 < np; p2++)   /* (lowest priority first, then registry order) */
                    for (int q = p2; q > 0 && (prio[ord[q - 1]] > prio[ord[q]] || (prio[ord[q - 1]] == prio[ord[q]] && ph[ord[q - 1]] > ph[ord[q]])); q--) {
                        int t = ord[q]; ord[q] = ord[q - 1]; ord[q - 1] = t;
                    }
                for (int kk = 0; kk < np; kk++) {
                    int k = ord[kk];
                    if (!(ps[k] <= s && pe[k] >= e) || !st[k].set_bg) continue;
                    /* (on whole pixels, as inline boxes' backgrounds are) */
                    float rx = px, ry = hy, rw = (w1 - w0) * scale, rh = hh;
                    snap_rect(&rx, &ry, &rw, &rh);
                    display_list_add_rect(dl, rx, ry, rw, rh, (float)(st[k].bg[0] / 255.0), (float)(st[k].bg[1] / 255.0),
                                          (float)(st[k].bg[2] / 255.0), (float)st[k].bg[3] * op);
                }
                continue;
            }
            /* (the color of the topmost highlight that sets one: highest priority, then the later) */
            float cr = r, cg = g, cb = b, ca = a;
            int best = -1;
            for (int k = 0; k < np; k++)
                if (ps[k] <= s && pe[k] >= e && st[k].set_c && (best < 0 || prio[k] > prio[best] || (prio[k] == prio[best] && ph[k] >= ph[best]))) best = k;
            if (best >= 0) {
                cr = (float)(st[best].c[0] / 255.0); cg = (float)(st[best].c[1] / 255.0);
                cb = (float)(st[best].c[2] / 255.0); ca = (float)st[best].c[3] * op;
            }
            if (pass == 1) {
                /* (the topmost highlight's text-shadow that sets one) */
                int sb = -1;
                for (int k = 0; k < np; k++)
                    if (ps[k] <= s && pe[k] >= e && st[k].rtsh[0] && (sb < 0 || prio[k] > prio[sb] || (prio[k] == prio[sb] && ph[k] >= ph[sb]))) sb = k;
                if (sb < 0) continue;
                size_t l1 = e - start < sizeof buf - 1 ? e - start : sizeof buf - 1;
                memcpy(buf, run_text + start, l1);
                buf[l1] = 0;
                text_shaper_measure(font, buf, &w1, &hgt);
                float cc[4] = { cr, cg, cb, ca };
                if (!dl_hl_shadows(dl, &st[sb], font, run_text, s, e, px, y, scale, (w1 - w0) * scale, fs, cc, op)) { *ok = false; return true; }
                continue;
            }
            /* (the highlights' decorations: under- and overlines beneath the text, line-throughs over it) */
            float pw = 0.0f, ul_pos = 0.1f, ul_th = 0.05f, st_pos = 0.3f, st_th = 0.05f;
            bool any_td = false;
            for (int k = 0; k < np; k++) if (ps[k] <= s && pe[k] >= e && st[k].td_lines) any_td = true;
            if (any_td) {
                size_t l1 = e - start < sizeof buf - 1 ? e - start : sizeof buf - 1;
                memcpy(buf, run_text + start, l1);
                buf[l1] = 0;
                text_shaper_measure(font, buf, &w1, &hgt);
                pw = (w1 - w0) * scale;
                text_shaper_deco_metrics(font, &ul_pos, &ul_th, &st_pos, &st_th);
            }
            float by = y + roundf(va * (float)fs), th = roundf(ul_th * (float)fs);
            if (th < 1.0f) th = 1.0f;
            for (int k = 0; any_td && k < np; k++) {
                if (!(ps[k] <= s && pe[k] >= e) || !(st[k].td_lines & 5)) continue;
                float dc[4] = { cr, cg, cb, ca };
                if (st[k].td_cset) { dc[0] = (float)(st[k].td_c[0] / 255.0); dc[1] = (float)(st[k].td_c[1] / 255.0); dc[2] = (float)(st[k].td_c[2] / 255.0); dc[3] = (float)st[k].td_c[3] * op; }
                float tk = st[k].th_set ? roundf((float)st[k].th) : th;
                if (tk < 1.0f) tk = 1.0f;
                float uy = st[k].uo_set ? by + (float)st[k].uo : by + ul_pos * (float)fs;
                if (st[k].td_lines & 1) dl_deco_line(dl, px, px + pw, roundf(uy), tk, st[k].td_style, dc);
                if (st[k].td_lines & 4) dl_deco_line(dl, px, px + pw, roundf(by - va * (float)fs), tk, st[k].td_style, dc);
            }
            if (!text_shaper_shape_span(font, run_text, s, e, px, y, scale, false, cr, cg, cb, ca, dl)) { *ok = false; return true; }
            for (int k = 0; any_td && k < np; k++) {
                if (!(ps[k] <= s && pe[k] >= e) || !(st[k].td_lines & 2)) continue;
                float dc[4] = { cr, cg, cb, ca };
                if (st[k].td_cset) { dc[0] = (float)(st[k].td_c[0] / 255.0); dc[1] = (float)(st[k].td_c[1] / 255.0); dc[2] = (float)(st[k].td_c[2] / 255.0); dc[3] = (float)st[k].td_c[3] * op; }
                float tk = st[k].th_set ? roundf((float)st[k].th) : th;
                if (tk < 1.0f) tk = 1.0f;
                dl_deco_line(dl, px, px + pw, roundf(by - st_pos * (float)fs), tk, st[k].td_style, dc);
            }
        }
    }
    return true;
}

/* (emphasis marks on a ruby's base go beyond its annotations on their side: how fside) */
static float dl_emph_ruby(const LayoutContext *ctx, int i)
{
    const LayoutStyle *s = ctx->boxes[i].s;
    bool vert = s->writing_mode != 0;
    bool fside = vert ? !s->te_left : s->te_under;   /* (the side: right or bottom) */
    double best = 0.0;
    for (int p = ctx->tree.nodes[i].parent_idx, g = 0; p >= 0 && g < 32; p = ctx->tree.nodes[p].parent_idx, g++) {
        const LayoutBox *wb = &ctx->boxes[p];
        if (wb->s->display != CSS_DISPLAY_INLINE) break;
        if (wb->s->ruby_role != 6 || !wb->s->ruby_first) continue;
        double w0 = vert ? wb->x : wb->y, w1 = w0 + (vert ? wb->width : wb->height);
        int a = wb->s->ruby_first - 1;
        for (int k = 0; a >= 0 && a < ctx->tree.count && k < 16; a = ctx->boxes[a].s->ruby_next - 1, k++) {
            const LayoutBox *ab = &ctx->boxes[a];
            if (ab->s->display == CSS_DISPLAY_NONE) continue;
            double a0 = vert ? ab->x : ab->y, a1 = a0 + (vert ? ab->width : ab->height);
            if (fside ? (a0 + a1) / 2.0 <= (w0 + w1) / 2.0 : (a0 + a1) / 2.0 >= (w0 + w1) / 2.0) continue;
            double d = fside ? a1 - w1 : w0 - a0;
            if (d > best) best = d;
        }
    }
    return (float)best;
}

static void dl_emphasis_marks(DisplayList *dl, TextShaper *font, const LayoutBox *box, const char *text, size_t start,
                              size_t end, float x, float y, float scale, float op, float rs)
{
    const LayoutStyle *s = box->s;
    const char *mark = s->te_mark;
    if (s->te_shape && s->writing_mode != 0) mark = s->te_shape == 2 ? "\xEF\xB9\x86" : "\xEF\xB9\x85";
    double fs = s->font_size > 0.0 ? s->font_size : 16.0, mfs = fs / 2.0;
    float a = 0.8f, d = 0.2f, g = 0.0f;
    text_shaper_vmetrics(font, &a, &d, &g);
    double ra = round(a * mfs), rd = round(d * mfs), adv = text_shaper_normal_line_height(font, (float)mfs);
    if (!(adv > 0.5 * mfs)) adv = mfs * 1.2;
    double ca = round(a * fs) + round(d * fs);
    /* (vertical text, drawn in its line's axes: over the line is right in
     * vertical-rl and in vertical-lr, whose caller mirrors what is drawn;
     * left in sideways-lr; left or right says the side) */
    bool below = s->te_under;
    if (s->writing_mode != 0) {
        bool rl = s->writing_mode == 1 || s->writing_mode == 3 || s->writing_mode == 2;
        below = rl ? s->te_left : !s->te_left;
    }
    double my = below ? y + ca + rs + floor((adv - ra - rd) / 2.0) : y - rs - adv + floor((adv - ra - rd) / 2.0);
    float mw = 0.0f, mh = 0.0f;
    text_shaper_measure(font, mark, &mw, &mh);
    mw *= scale / 2.0f;
    float c[4];
    if (s->te_color_set) {
        uint32_t v = s->te_rgba;
        c[0] = (float)((v >> 24) & 0xFF) / 255.0f; c[1] = (float)((v >> 16) & 0xFF) / 255.0f;
        c[2] = (float)((v >> 8) & 0xFF) / 255.0f; c[3] = (float)(v & 0xFF) / 255.0f * op;
    } else {
        c[0] = (float)s->color_r; c[1] = (float)s->color_g; c[2] = (float)s->color_b; c[3] = (float)s->color_a * op;
    }
    char buf[512];
    size_t at = start;
    while (at < end) {
        size_t nx = at + 1;
        while (nx < end && ((unsigned char)text[nx] & 0xC0) == 0x80) nx++;
        unsigned char c0 = (unsigned char)text[at];
        bool skip = c0 < 0x80 && (c0 <= ' ' || c0 == 0x7F || ispunct(c0));
        if (!skip && c0 >= 0x80) skip = dl_no_emphasis(text + at, nx - at);
        if (!skip) {
            /* (the advance before it, and its own) */
            size_t l0 = at - start < sizeof buf - 1 ? at - start : sizeof buf - 1;
            memcpy(buf, text + start, l0);
            buf[l0] = 0;
            float w0 = 0.0f, w1 = 0.0f, h;
            text_shaper_measure(font, buf, &w0, &h);
            size_t l1 = nx - start < sizeof buf - 1 ? nx - start : sizeof buf - 1;
            memcpy(buf, text + start, l1);
            buf[l1] = 0;
            text_shaper_measure(font, buf, &w1, &h);
            float cx = x + (w0 + w1) * 0.5f * scale;
            text_shaper_shape_to_display_list_scaled(font, mark, cx - mw * 0.5f, (float)my, scale / 2.0f, c[0], c[1], c[2], c[3], dl);
        }
        at = nx;
    }
}

static bool build_display_list_impl(LayoutContext *ctx, DisplayList *dl, float (*clip)[4], float (*off)[2]);
static void dl_scroll_range(DisplayList *dl, int from, int mask_from, const float o[2]);

/* The viewport's scroll of box idx's document. */
static void dl_viewport_scroll(LayoutContext *ctx, float *x, float *y)
{
    *x = *y = 0.0f;
    HtmlNode *rn = ctx->tree.root_idx >= 0 && ctx->doc ? css_layout_box_node(ctx, ctx->tree.root_idx) : NULL;
    if (rn) dom_scroll_get(dom_owner(rn->store_id), x, y);
}

bool css_layout_build_display_list(LayoutContext *ctx, DisplayList *dl)
{
    if (!ctx || !dl) return false;
    display_list_init(dl);
    /* overflow != visible clips descendants to the padding box, and they
     * move by its scroll offset (off: only while something is scrolled). */
    int n = ctx->tree.count;
    float (*clip)[4] = (float (*)[4])malloc(((size_t)n + 1) * sizeof(*clip));
    /* (offsets: while something is scrolled, or a sticky box is there to place) */
    bool any_sticky = false;
    for (int k = 0; k < n && !any_sticky; k++) any_sticky = ctx->boxes[k].s && (ctx->boxes[k].s->position == CSS_POSITION_STICKY || ctx->boxes[k].s->anc_used);
    float (*off)[2] = g_dom_scrolls.n || any_sticky ? (float (*)[2])calloc((size_t)n + 1, sizeof(*off)) : NULL;
    float view_x = 0.0f, view_y = 0.0f;
    if (off) dl_viewport_scroll(ctx, &view_x, &view_y);
    bool fixed = false;
    g_dom_scrolls.paint = false;
    bool flt_tf = false, any_flt = false;
    for (int k = 0; k < n; k++) {
        const LayoutStyle *ks = ctx->boxes[k].s;
        if (!ks) continue;
        flt_tf = flt_tf || ks->tf_text || ks->tf_rotate || ks->tf_scale || ks->op_kind;
        any_flt = any_flt || ks->filt_url || ks->filt_n;
    }
    t_flt_clip = any_flt ? (float (*)[4])calloc((size_t)n + 1, sizeof(*t_flt_clip)) : NULL;
    if (clip) {
        for (int i = 0; i < n; i++) {
            int p = ctx->tree.nodes[i].parent_idx;
            LayoutBox *b = &ctx->boxes[i];
            /* (an absolutely positioned box: clipped as its containing
             * block's content, not by the boxes between) */
            if (b->s->position == CSS_POSITION_ABSOLUTE)
                while (p >= 0 && ctx->tree.nodes[p].parent_idx >= 0 && ctx->boxes[p].s->position == CSS_POSITION_STATIC &&
                       !dl_paint_contained(&ctx->boxes[p]))
                    p = ctx->tree.nodes[p].parent_idx;
            /* (a fixed box under a paint-contained one: its containing block, clipping it) */
            bool fixed_contained = false;
            if (b->s->position == CSS_POSITION_FIXED)
                for (int q = p; q >= 0; q = ctx->tree.nodes[q].parent_idx)
                    if (dl_paint_contained(&ctx->boxes[q])) { p = q; fixed_contained = true; break; }
            float c[4] = { -1e30f, -1e30f, 1e30f, 1e30f };
            if (b->s->position == CSS_POSITION_FIXED && !fixed_contained) {
                fixed = true;
                /* (the frame scrolls as a whole with the viewport: this stays) */
                if (off) { off[i][0] = -view_x; off[i][1] = -view_y; }
            }
            if (p >= 0 && p < i && (b->s->position != CSS_POSITION_FIXED || fixed_contained)) {
                memcpy(c, clip[p], sizeof(c));
                float sx, sy;
                if (off) {
                    off[i][0] = off[p][0];
                    off[i][1] = off[p][1];
                    if (css_layout_box_offset(ctx, p, &sx, &sy)) { off[i][0] += sx; off[i][1] += sy; }
                }
                LayoutBox *pb = &ctx->boxes[p];
                /* overflow on <html>/<body> applies to the viewport. */
                HtmlNode *pn = ctx->doc ? css_layout_box_node(ctx, p) : NULL;
                bool viewport_box = pn && pn->type == HTML_NODE_ELEMENT && pn->tag_name &&
                    (strcasecmp(pn->tag_name, "html") == 0 || strcasecmp(pn->tag_name, "body") == 0);
                /* (the body's goes to the viewport only with no containment on it or the root) */
                if (viewport_box && !strcasecmp(pn->tag_name, "body") &&
                    (pb->s->contain_any || (ctx->tree.root_idx >= 0 && ctx->boxes[ctx->tree.root_idx].s->contain_any)))
                    viewport_box = false;
                if ((pb->s->overflow != 0 || dl_paint_contained(pb)) &&
                    pb->s->display != CSS_DISPLAY_NONE && !viewport_box) {
                    if (getenv("CYBER_DEBUG_CLIP") && ctx->tree.nodes[i].prev_sibling_idx < 0)
                        fprintf(stderr, "[CLIP] box %d by parent %d: %.0f,%.0f %.0fx%.0f\n", i, p,
                                pb->x, pb->y, pb->width, pb->height);
                    float px0 = (float)(pb->x + pb->s->border_left), py0 = (float)(pb->y + pb->s->border_top);
                    float px1 = (float)(pb->x + pb->width - pb->s->border_right);
                    float py1 = (float)(pb->y + pb->height - pb->s->border_bottom);
                    if (off) {
                        px0 -= off[p][0]; px1 -= off[p][0];
                        py0 -= off[p][1]; py1 -= off[p][1];
                    }
                    if (pb->s->table_role == 1) {
                        /* (a table's box, without its captions; they are
                         * outside it, not clipped by it) */
                        float cbot = 0.0f, ctop = table_caption_top(ctx, p, pb, &cbot);
                        py0 += ctop;
                        py1 -= cbot;
                        if (b->s->table_caption || b->s->table_role == 5) { px0 = c[0]; py0 = c[1]; px1 = c[2]; py1 = c[3]; }
                    }
                    /* (overflow-clip-margin: overflow: clip and paint containment clip that far out) */
                    if ((pb->s->overflow_clip_margin > 0.0f || pb->s->ocm_box) && (pb->s->overflow_clip || (pb->s->overflow == 0 && dl_paint_contained(pb)))) {
                        float om = pb->s->overflow_clip_margin;
                        /* (from its border box, or its content box) */
                        if (pb->s->ocm_box == 1) { px0 -= (float)pb->s->border_left; py0 -= (float)pb->s->border_top; px1 += (float)pb->s->border_right; py1 += (float)pb->s->border_bottom; }
                        else if (pb->s->ocm_box == 2) { px0 += (float)pb->padding_left; py0 += (float)pb->padding_top; px1 -= (float)pb->padding_right; py1 -= (float)pb->padding_bottom; }
                        px0 -= om; py0 -= om; px1 += om; py1 += om;
                    }
                    /* (overflow: clip on one axis alone: the other not clipped) */
                    unsigned char cax = pb->s->overflow_clip && pb->s->ovf_clip_axes ? pb->s->ovf_clip_axes : 3;
                    if (!(cax & 1)) { px0 = -1e9f; px1 = 1e9f; }
                    if (!(cax & 2)) { py0 = -1e9f; py1 = 1e9f; }
                    if (px0 > c[0]) c[0] = px0;
                    if (py0 > c[1]) c[1] = py0;
                    if (px1 < c[2]) c[2] = px1;
                    if (py1 < c[3]) c[3] = py1;
                }
            }
            /* (turned or scaled: its commands are in its own space, the clip from outside mapped into it) */
            if (flt_tf && (b->s->tf_text || b->s->tf_rotate || b->s->tf_scale || b->s->op_kind) && c[0] <= c[2] && (c[0] > -1e29f || c[1] > -1e29f || c[2] < 1e29f || c[3] < 1e29f)) {
                double m[6];
                if (css_layout_box_map(ctx, i, ctx->tree.nodes[i].parent_idx, m)) {
                    double det = m[0] * m[3] - m[1] * m[2];
                    /* (a quarter turn or a scale: the rectangle stays one; others are cut in their own space still) */
                    bool axis = (fabs(m[1]) < 1e-6 && fabs(m[2]) < 1e-6) || (fabs(m[0]) < 1e-6 && fabs(m[3]) < 1e-6);
                    if (axis && fabs(det) > 1e-9 && (fabs(m[0] - 1.0) > 1e-9 || fabs(m[3] - 1.0) > 1e-9 || fabs(m[1]) > 1e-9 || fabs(m[2]) > 1e-9 || fabs(m[4]) > 1e-9 || fabs(m[5]) > 1e-9)) {
                        double v[6] = { m[3] / det, -m[1] / det, -m[2] / det, m[0] / det, 0, 0 };
                        v[4] = -(v[0] * m[4] + v[2] * m[5]);
                        v[5] = -(v[1] * m[4] + v[3] * m[5]);
                        double q[4];
                        for (int e = 0; e < 4; e++) q[e] = c[e] < -1e7f ? -1e7 : c[e] > 1e7f ? 1e7 : c[e];
                        double bx = q[0], by = q[1], bw = q[2] - q[0], bh = q[3] - q[1];
                        css_layout_map_bounds(v, &bx, &by, &bw, &bh);
                        c[0] = (float)bx; c[1] = (float)by; c[2] = (float)(bx + bw); c[3] = (float)(by + bh);
                    }
                }
            }
            /* (sticky: where scrolling pushes it, its subtree with it) */
            if (off && b->s->position == CSS_POSITION_STICKY) {
                float dx, dy;
                dl_sticky_shift(ctx, i, view_x, view_y, &dx, &dy);
                off[i][0] -= dx;
                off[i][1] -= dy;
            }
            /* (placed against an anchor before the scrolling: where the anchor's scrolling takes it) */
            if (off && b->s->anc_used > 0 && b->s->anc_used - 1 < i) {
                int an = b->s->anc_used - 1;
                /* (layout has an anchor's element scrollers in its rect, not these) */
                if (b->s->position == CSS_POSITION_FIXED) {
                    bool an_fixed = false;
                    for (int q = an; q >= 0 && !an_fixed; q = ctx->tree.nodes[q].parent_idx) an_fixed = ctx->boxes[q].s->position == CSS_POSITION_FIXED;
                    if (!an_fixed) { off[i][0] += view_x; off[i][1] += view_y; }
                }
                if (ctx->boxes[an].s->position == CSS_POSITION_STICKY) {
                    float dx, dy;
                    dl_sticky_shift(ctx, an, view_x, view_y, &dx, &dy);
                    off[i][0] -= dx;
                    off[i][1] -= dy;
                }
            }
            /* (an object or embed showing a frame: its fallback content not) */
            {
                int op = ctx->tree.nodes[i].parent_idx;
                HtmlNode *on = op >= 0 && ctx->doc ? css_layout_box_node(ctx, op) : NULL;
                if (on && on->type == HTML_NODE_ELEMENT && on->tag_name && on->store_id && !strcasecmp(on->tag_name, "object") && dl_frame_has(on->store_id)) {
                    c[0] = c[1] = 1e30f;
                    c[2] = c[3] = -1e30f;
                }
            }
            /* (a line-clamp container that clamped: its content box's end, for what is in it) */
            {
                int cp = ctx->tree.nodes[i].parent_idx;
                bool inl_ok = ctx->boxes[cp >= 0 ? cp : 0].clamp_clip == 2 && b->s->display == CSS_DISPLAY_INLINE && !b->s->float_side;
                if (cp >= 0 && cp < i && ctx->boxes[cp].clamp_clip && (ctx->boxes[cp].clamp_clip != 3 || b->s->float_side) && !inl_ok && !b->clamp_keep && b->s->position != CSS_POSITION_ABSOLUTE && b->s->position != CSS_POSITION_FIXED) {
                    const LayoutBox *cb2 = &ctx->boxes[cp];
                    float cy = cb2->clamp_y - (off ? off[cp][1] : 0.0f);
                    if (cy < c[3]) c[3] = cy;
                }
            }
            /* (content-visibility: hidden: what is inside it not painted, positioned boxes too) */
            int tp = ctx->tree.nodes[i].parent_idx;
            bool pos_out = ctx->boxes[i].s->position == CSS_POSITION_ABSOLUTE || ctx->boxes[i].s->position == CSS_POSITION_FIXED;
            if (pos_out) {
                const HtmlNode *pn = css_layout_box_node(ctx, i);
                if (pn && pn->type == HTML_NODE_ELEMENT && ctx->boxes[i].s->ruby_col) pos_out = false;
            }
            if (tp >= 0 && tp < i && ((ctx->boxes[tp].s->cv_hidden && dl_cv_applies(ctx, tp)) ||
                                      (clip[tp][0] > clip[tp][2] && clip[tp][0] == 1e30f && !(pos_out && ctx->boxes[tp].clamp_gone && !ctx->boxes[i].clamp_gone)))) {
                c[0] = c[1] = 1e30f;
                c[2] = c[3] = -1e30f;
            }
            if (ctx->boxes[i].clamp_gone) {   /* (past a line-clamp's last line) */
                c[0] = c[1] = 1e30f;
                c[2] = c[3] = -1e30f;
            }
            /* (position-visibility: no-overflow that overflowed; anchors-visible (as initially)
             * with its anchor scrolled or clipped out of sight) */
            {
                const LayoutStyle *ps = ctx->boxes[i].s;
                int an = ps->pv_anchor - 1;
                bool gone = ps->pv_hidden != 0;
                if (!gone && an >= 0 && an < i && !(ps->pos_vis & 1) && (ps->pos_vis == 0 || (ps->pos_vis & 2))) {
                    const LayoutBox *ab = &ctx->boxes[an];
                    float ox = off ? off[an][0] : 0.0f, oy = off ? off[an][1] : 0.0f;
                    float ax0 = (float)ab->x - ox, ay0 = (float)ab->y - oy, ax1 = ax0 + (float)ab->width, ay1 = ay0 + (float)ab->height;
                    const float *ac = clip[an];
                    gone = ac[0] > ac[2] || ax1 <= ac[0] || ax0 >= ac[2] || ay1 <= ac[1] || ay0 >= ac[3];
                }
                if (gone) {
                    c[0] = c[1] = 1e30f;
                    c[2] = c[3] = -1e30f;
                }
            }
            /* (a CPU filter: what clips it clips its result; into it, nothing yet) */
            if (t_flt_clip && dl_box_filter_cpu(b, flt_tf) && !(c[0] > c[2])) {
                memcpy(t_flt_clip[i], c, sizeof(c));
                c[0] = c[1] = -1e30f;
                c[2] = c[3] = 1e30f;
            }
            memcpy(clip[i], c, sizeof(c));
        }
    }
    g_dom_scrolls.fixed = fixed;
    /* (print mode: as many pages as the content, what is out of flow too, takes) */
    if (g_css_print.on && ctx->tree.root_idx >= 0) {
        LayoutBox *rb = &ctx->boxes[ctx->tree.root_idx];
        g_css_print.pages = 1;
        if (rb->mc_n > 0 && rb->mc_h > 0.5f) {
            double top = rb->y + rb->padding_top + rb->s->border_top, most = top;
            for (int k = 0; k < ctx->tree.count; k++) {
                const LayoutBox *b = &ctx->boxes[k];
                if (!b->s || b->s->display == CSS_DISPLAY_NONE || !(b->flags & LAYOUT_HAS_LAYOUT) || k == ctx->tree.root_idx) continue;
                bool fx = false;
                for (int p = k, d = 0; p >= 0 && d < 1024; p = ctx->tree.nodes[p].parent_idx, d++) if (ctx->boxes[p].s && ctx->boxes[p].s->position == CSS_POSITION_FIXED) { fx = true; break; }
                if (fx || b->height <= 0.0) continue;
                if (b->y + b->height > most) most = b->y + b->height;
            }
            int n = (int)ceil((most - top - 0.01) / rb->mc_h);
            if (n < 1) n = 1;
            if (n > 200) n = 200;
            g_css_print.pages = n;
            rb->mc_used = n;
        }
    }
    bool ok = build_display_list_impl(ctx, dl, clip, off);
    /* (print mode: the first page's area where its margins put it) */
    if (ok && g_css_print.on) {
        const float o[2] = { -(float)g_css_print.ml, -(float)g_css_print.mt };
        dl_scroll_range(dl, 0, 0, o);
    }
    free(t_flt_clip);
    t_flt_clip = NULL;
    free(clip);
    free(off);
    return ok;
}

/* What a native form control has inside it: meter and progress bars, a range's
 * track and thumb, a check; along the writing mode's inline axis, from its start. */
static void paint_native_control(const LayoutContext *ctx, DisplayList *dl, int i, HtmlNode *node, float op)
{
    if (!node || node->type != HTML_NODE_ELEMENT || !node->tag_name) return;
    const LayoutBox *box = &ctx->boxes[i];
    const char *tag = node->tag_name;
    int kind = 0;   /* (1 meter, 2 progress, 3 range, 4 checkbox, 5 radio) */
    if (!strcasecmp(tag, "meter")) kind = 1;
    else if (!strcasecmp(tag, "progress")) kind = 2;
    else if (!strcasecmp(tag, "input")) {
        const char *t = node_attribute_value(node, "type");
        if (t && !strcasecmp(t, "range")) kind = 3;
        else if (t && !strcasecmp(t, "checkbox")) kind = 4;
        else if (t && !strcasecmp(t, "radio")) kind = 5;
    }
    if (!kind || box->width <= 0.0 || box->height <= 0.0) return;
    const LayoutStyle *s = box->s;
    bool none = s->appearance_none;
    if (none && kind >= 4) return;
    /* (CSS UI 4, author border or background on a bar: it falls back to what appearance: none shows) */
    if (kind <= 2 && (s->border_top > 0.0 || s->border_right > 0.0 || s->border_bottom > 0.0 || s->border_left > 0.0 ||
                      s->background_color_a > 0.0f || s->background_image_url[0] || box->padding_left > 0.0 || box->padding_top > 0.0)) none = true;
    /* (declared at all, even as what it already was) */
    if (kind <= 2 && !none) {
        const char *st = node_attribute_value(node, "style");
        if (st && (strstr(st, "background") || strstr(st, "border") || strstr(st, "padding"))) none = true;
    }
    float x = (float)(box->x + s->border_left + box->padding_left), y = (float)(box->y + s->border_top + box->padding_top);
    float w = (float)box->content_width, h = (float)box->content_height;
    if (w <= 0.0f || h <= 0.0f) return;
    float ac[3] = { 0.0f, 0.46f, 1.0f };
    for (int p = i, d = 0; p >= 0 && d < 256; p = ctx->tree.nodes[p].parent_idx, d++) {
        const LayoutStyle *ps = ctx->boxes[p].s;
        if (ps && ps->accent_set == 2) { ac[0] = (float)s->color_r; ac[1] = (float)s->color_g; ac[2] = (float)s->color_b; break; }
        if (ps && ps->accent_set) { memcpy(ac, ps->accent, sizeof ac); break; }
    }
    if (kind >= 4) {
        if (!(dom_element_state(node->store_id) & DOM_ST_CHECKED)) return;
        if (kind == 4) {
            display_list_add_rect(dl, x, y, w, h, ac[0], ac[1], ac[2], op);
            display_list_add_rect(dl, x + w * 0.2f, y + h * 0.45f, w * 0.6f, h * 0.15f, 1.0f, 1.0f, 1.0f, op);
        } else display_list_add_rect(dl, x + w * 0.25f, y + h * 0.25f, w * 0.5f, h * 0.5f, ac[0], ac[1], ac[2], op);
        return;
    }
    double mn = 0.0, mx = kind == 3 ? 100.0 : 1.0, val = kind == 3 ? 50.0 : 0.0;
    const char *a;
    if (kind != 2 && (a = node_attribute_value(node, "min"))) mn = atof(a);
    if ((a = node_attribute_value(node, "max"))) mx = atof(a);
    if ((a = node_attribute_value(node, "value"))) val = atof(a);
    else if (kind == 2) val = -1.0;
    double f = mx > mn ? (val - mn) / (mx - mn) : 0.0;
    if (f < 0.0) f = 0.0;
    if (f > 1.0) f = 1.0;
    bool vert = s->writing_mode != 0;
    /* (the inline axis runs against the physical one: right to left, or up) */
    bool rev = (s->direction == 1) != (s->writing_mode == 4);
    float L = vert ? h : w, B = vert ? w : h;
    /* (appearance: none: the parts as ::slider-track, -fill and -thumb style them, their backgrounds) */
    if (s->appearance_none) {
        float pc[3][4];
        bool have[3] = { false, false, false };
        for (int k = 0; k < 3; k++) {
            const CssDeclaration *const *ds = NULL;
            int n = css_layout_pseudo_decls(ctx, i, 14 + k, &ds);
            for (int d = 0; d < n && ds; d++) {
                if (!ds[d]->property || !ds[d]->value) continue;
                if (strcasecmp(ds[d]->property, "background-color") && strcasecmp(ds[d]->property, "background")) continue;
                double rgba[4];
                if (css_layout_parse_color_text(ds[d]->value, rgba)) { for (int c = 0; c < 4; c++) pc[k][c] = (float)rgba[c]; have[k] = true; }
            }
        }
        if (!have[0] && !have[1] && !have[2]) { if (kind == 3) return; }
        else {
            if (have[0]) display_list_add_rect(dl, x, y, w, h, pc[0][0], pc[0][1], pc[0][2], pc[0][3] * op);
            float e = (float)(f * L);
            if (have[1] && e > 0.0f && !(kind == 2 && val < 0.0)) {
                if (vert) display_list_add_rect(dl, x, rev ? y + h - e : y, w, e, pc[1][0], pc[1][1], pc[1][2], pc[1][3] * op);
                else display_list_add_rect(dl, rev ? x + w - e : x, y, e, h, pc[1][0], pc[1][1], pc[1][2], pc[1][3] * op);
            }
            if (have[2] && kind == 3) {
                float th = B, t0 = (float)(f * (L - th));
                if (rev) t0 = L - th - t0;
                if (vert) display_list_add_rect(dl, x, y + t0, w, th, pc[2][0], pc[2][1], pc[2][2], pc[2][3] * op);
                else display_list_add_rect(dl, x + t0, y, th, h, pc[2][0], pc[2][1], pc[2][2], pc[2][3] * op);
            }
            return;
        }
    }
    float tr[3] = { 0.93f, 0.93f, 0.93f }, fc[3] = { 0.0f, 0.46f, 1.0f };
    if (kind == 1) { fc[0] = 0.0f; fc[1] = 0.6f; fc[2] = 0.2f; }
    if (none) { tr[0] = tr[1] = tr[2] = 0.8f; fc[0] = fc[1] = fc[2] = 0.4f; }
    if (kind == 3) {
        /* (a track through the middle, the thumb along it) */
        float tb = 4.0f, th = 16.0f;
        float p0 = (B - tb) * 0.5f, t0 = (float)(f * (L - th));
        if (rev) t0 = L - th - t0;
        if (vert) {
            display_list_add_rect(dl, x + p0, y, tb, h, tr[0] * 0.8f, tr[1] * 0.8f, tr[2] * 0.8f, op);
            display_list_add_rect(dl, x + (B - th) * 0.5f, y + t0, th, th, ac[0], ac[1], ac[2], op);
        } else {
            display_list_add_rect(dl, x, y + p0, w, tb, tr[0] * 0.8f, tr[1] * 0.8f, tr[2] * 0.8f, op);
            display_list_add_rect(dl, x + t0, y + (B - th) * 0.5f, th, th, ac[0], ac[1], ac[2], op);
        }
        return;
    }
    display_list_add_rect(dl, x, y, w, h, tr[0], tr[1], tr[2], op);
    float e = (float)(f * L);
    if (kind == 2 && val < 0.0) return;   /* (indeterminate) */
    if (e <= 0.0f) return;
    if (vert) display_list_add_rect(dl, x, rev ? y + h - e : y, w, e, fc[0], fc[1], fc[2], op);
    else display_list_add_rect(dl, rev ? x + w - e : x, y, e, h, fc[0], fc[1], fc[2], op);
}

/* The canvas's background (CSS Backgrounds §2.11.2): the root element's
 * color, else (an HTML root without one) the body's, over the whole canvas,
 * the viewport and all the document's boxes.  Returns the box it came from
 * (its own background is not painted again), -1 for none, -2 on failure. */
static int canvas_background(LayoutContext *ctx, DisplayList *dl)
{
    int root = ctx->tree.root_idx;
    if (root < 0 || root >= ctx->tree.count || !ctx->doc || !ctx->boxes[root].s) return -1;
    /* (print: each sheet's page colour first, from the hidden boxes of the host at z-index -999999001) */
    if (g_css_print.on) {
        for (int c = ctx->tree.nodes[root].first_child_idx; c >= 0; c = ctx->tree.nodes[c].next_sibling_idx) {
            const LayoutStyle *hs = ctx->boxes[c].s;
            if (!hs || hs->position != CSS_POSITION_FIXED || hs->z_index != -999999001) continue;
            for (int q = ctx->tree.nodes[c].first_child_idx; q >= 0; q = ctx->tree.nodes[q].next_sibling_idx) {
                const LayoutBox *b = &ctx->boxes[q];
                if (!b->s || !(b->flags & LAYOUT_HAS_LAYOUT) || b->s->background_color_a <= 0.0f) continue;
                if (!display_list_add_rect(dl, (float)b->x, (float)b->y, (float)b->width, (float)b->height, (float)b->s->background_color_r,
                                           (float)b->s->background_color_g, (float)b->s->background_color_b, (float)b->s->background_color_a))
                    return -2;
            }
        }
    }
    int src = -1;
    if (ctx->boxes[root].s->background_color_a > 0.0f || ctx->boxes[root].s->background_image_url[0] || ctx->boxes[root].s->bg_grad) {
        src = root;
    } else if (ctx->boxes[root].s->display == CSS_DISPLAY_NONE) {
        /* (a root not rendered: no body's to propagate, its boxes not laid out
         * and their styles not to be read) */
        return -1;
    } else {
        HtmlNode *rn = css_layout_box_node(ctx, root);
        if (!rn || rn->type != HTML_NODE_ELEMENT || !rn->tag_name || strcasecmp(rn->tag_name, "html") != 0) return -1;
        for (int c = ctx->tree.nodes[root].first_child_idx; c >= 0; c = ctx->tree.nodes[c].next_sibling_idx) {
            HtmlNode *n = css_layout_box_node(ctx, c);
            if (!n || n->type != HTML_NODE_ELEMENT || !n->tag_name || strcasecmp(n->tag_name, "body") != 0) continue;
            /* (containment on either: the body's stays its own) */
            if ((ctx->boxes[c].s->background_color_a > 0.0f || ctx->boxes[c].s->background_image_url[0] || ctx->boxes[c].s->bg_grad) &&
                ctx->boxes[c].s->display != CSS_DISPLAY_NONE && !ctx->boxes[c].s->contain_any && !ctx->boxes[root].s->contain_any)
                src = c;
            break;
        }
    }
    if (src < 0) return -1;
    double w = ctx->viewport_width, h = ctx->viewport_height;
    for (int i = 0; i < ctx->tree.count; i++) {
        const LayoutBox *b = &ctx->boxes[i];
        if (!(b->flags & LAYOUT_HAS_LAYOUT) || b->s->display == CSS_DISPLAY_NONE) continue;
        if (b->x + b->width > w) w = b->x + b->width;
        if (b->y + b->height > h) h = b->y + b->height;
    }
    const LayoutStyle *s = ctx->boxes[src].s;
    float pr0 = g_css_print.on ? (float)-(g_css_print.ml + 2000.0) : 0.0f, pr1 = g_css_print.on ? (float)-(g_css_print.mt + 2000.0) : 0.0f;
    /* (print: the sheets, not past them) */
    if (g_css_print.on) {
        pr0 = (float)-g_css_print.ml;
        pr1 = (float)-g_css_print.mt;
        w = g_css_print.pw;
        h = (g_css_print.pages > 0 ? g_css_print.pages : 1) * (g_css_print.h + g_css_print.gap);
    }
    if (s->background_color_a > 0.0f &&
        !display_list_add_rect(dl, pr0, pr1, (float)ceil(w), (float)ceil(h), (float)s->background_color_r,
                               (float)s->background_color_g, (float)s->background_color_b,
                               (float)s->background_color_a * (float)ctx->boxes[src].opacity_eff))
        return -2;
    if (g_image_cache && (s->background_image_url[0] || s->bg_grad)) {
        float area[4] = { 0.0f, 0.0f, (float)ceil(w), (float)ceil(h) };
        /* (positioned against the root element's box, the body's too) */
        LayoutBox pb = ctx->boxes[root];
        LayoutStyle ps = *s;
        const LayoutStyle *rs = ctx->boxes[root].s;
        ps.border_left = rs->border_left;
        ps.border_right = rs->border_right;
        ps.border_top = rs->border_top;
        ps.border_bottom = rs->border_bottom;
        pb.s = &ps;
        emit_background_image(ctx, dl, &pb, area);
    }
    return src;
}

/* ---- Paint order (CSS 2.1 Appendix E, simplified) ----
 * Each stacking context paints its own box, then its negative z-index
 * stacking contexts, then its block-level descendants (backgrounds,
 * borders), its floats, its inline-level content (an inline-block whole),
 * then its positioned descendants with z-index auto or 0 in tree order, then
 * the positive z-index ones.  A float, an atomic inline or a positioned box
 * with z-index auto paints like a stacking context of its own, but its
 * positioned descendants (and the stacking contexts in it) belong to the
 * enclosing stacking context, and are painted with its own layers. */
typedef struct { int *v; int n, cap; } PaintVec;

static bool pv_push(PaintVec *p, int x)
{
    if (p->n == p->cap) {
        int nc = p->cap ? p->cap * 2 : 16;
        int *nv = (int *)realloc(p->v, (size_t)nc * sizeof(int));
        if (!nv) return false;
        p->v = nv;
        p->cap = nc;
    }
    p->v[p->n++] = x;
    return true;
}

/* (contain: paint where it applies: not inline boxes, nor ruby or table
 * internals but a cell) */
static bool dl_paint_contained(const LayoutBox *b)
{
    if (!b->s->contain_paint || b->s->ruby_int) return false;
    CssDisplay d = b->s->display;
    if (d == CSS_DISPLAY_INLINE || d == CSS_DISPLAY_NONE) return false;
    if (d == CSS_DISPLAY_OTHER && b->s->table_role != 5 && b->s->table_role != 1) return false;
    if (b->s->table_role >= 2 && b->s->table_role <= 4) return false;
    return true;
}

/* (content-visibility applies: a box with size containment, so not an inline,
 * display: contents, ruby or table internals, nor an SVG element inside one) */
static bool dl_cv_applies(const LayoutContext *ctx, int i)
{
    const LayoutStyle *s = ctx->boxes[i].s;
    CssDisplay d = s->display;
    if (s->disp_contents || d == CSS_DISPLAY_INLINE || d == CSS_DISPLAY_NONE) return false;
    if (d == CSS_DISPLAY_OTHER && s->table_role != 5 && s->table_role != 1) return false;
    if (s->table_role >= 2 && s->table_role <= 4) return false;
    HtmlNode *n = ctx->doc ? css_layout_box_node((LayoutContext *)ctx, i) : NULL;
    if (n && n->type == HTML_NODE_ELEMENT && n->tag_name && (inside_svg(&ctx->doc->array, ctx->tree.nodes[i].dom_node_idx) ||
        !strcasecmp(n->tag_name, "ruby") || !strcasecmp(n->tag_name, "rt"))) return false;
    return true;
}

/* (whether box i is t or inside it) */
static bool dl_box_under(const LayoutContext *ctx, int i, int t)
{
    for (int p = i; p >= 0; p = ctx->tree.nodes[p].parent_idx) if (p == t) return true;
    return false;
}

/* Collapsed table borders go over the table's block content (css-tables
 * #11570): each cell's border commands [a, b), held until the paint leaves
 * its table t (i outside it, or -1 at the end), then moved to the end. */
static void dl_cb_flush(const LayoutContext *ctx, DisplayList *dl, int *t, int *a, int *b, int *n, int i)
{
    int total = 0;
    for (int k = 0; k < *n; k++) if (i < 0 || !dl_box_under(ctx, i, t[k])) total += b[k] - a[k];
    if (!total) return;
    DisplayListCmd *tmp = (DisplayListCmd *)malloc((size_t)total * sizeof(DisplayListCmd));
    if (!tmp) return;
    int o = 0, removed = 0, keep = 0;
    for (int k = 0; k < *n; k++) {
        int ka = a[k] - removed, kb = b[k] - removed;
        if (i < 0 || !dl_box_under(ctx, i, t[k])) {
            memcpy(tmp + o, &dl->cmds[ka], (size_t)(kb - ka) * sizeof(DisplayListCmd));
            o += kb - ka;
            memmove(&dl->cmds[ka], &dl->cmds[kb], (size_t)(dl->count - kb) * sizeof(DisplayListCmd));
            dl->count -= kb - ka;
            removed += kb - ka;
        } else {
            t[keep] = t[k]; a[keep] = ka; b[keep] = kb; keep++;
        }
    }
    memcpy(&dl->cmds[dl->count], tmp, (size_t)total * sizeof(DisplayListCmd));
    dl->count += total;
    free(tmp);
    *n = keep;
}

/* ---- 2D canvases: the solid rectangles drawn on each, by NodeId ---- */
typedef struct { uint32_t node; int n, cap; float (*r)[4]; uint32_t *c; char **url; } DlCanvasOps;   /* (url: an image's, else NULL) */
static DlCanvasOps *g_cv;
static int g_cv_n, g_cv_cap;

static DlCanvasOps *dl_canvas_ops(uint32_t node, bool make)
{
    for (int i = 0; i < g_cv_n; i++) if (g_cv[i].node == node) return &g_cv[i];
    if (!make) return NULL;
    if (g_cv_n == g_cv_cap) {
        int nc = g_cv_cap ? g_cv_cap * 2 : 16;
        DlCanvasOps *nv = (DlCanvasOps *)realloc(g_cv, (size_t)nc * sizeof *nv);
        if (!nv) return NULL;
        g_cv = nv;
        g_cv_cap = nc;
    }
    DlCanvasOps *o = &g_cv[g_cv_n++];
    memset(o, 0, sizeof *o);
    o->node = node;
    return o;
}

void display_list_canvas_rect(uint32_t node, float x, float y, float w, float h, uint32_t rgba, int clear)
{
    if (w < 0.0f) { x += w; w = -w; }
    if (h < 0.0f) { y += h; h = -h; }
    DlCanvasOps *o = dl_canvas_ops(node, !clear);
    if (!o) return;
    if (clear) {
        /* (the rectangles it covers gone; one only partly under it kept) */
        int k = 0;
        for (int i = 0; i < o->n; i++) {
            float *r = o->r[i];
            bool under = r[0] >= x - 0.01f && r[1] >= y - 0.01f && r[0] + r[2] <= x + w + 0.01f && r[1] + r[3] <= y + h + 0.01f;
            if (under) { free(o->url[i]); continue; }
            memcpy(o->r[k], r, sizeof o->r[k]);
            o->c[k] = o->c[i];
            o->url[k] = o->url[i];
            k++;
        }
        o->n = k;
        return;
    }
    if (w <= 0.0f || h <= 0.0f || o->n >= 65536) return;
    if (o->n == o->cap) {
        int nc = o->cap ? o->cap * 2 : 16;
        float (*nr)[4] = (float (*)[4])realloc(o->r, (size_t)nc * sizeof *nr);
        if (!nr) return;
        o->r = nr;
        uint32_t *ncl = (uint32_t *)realloc(o->c, (size_t)nc * sizeof *ncl);
        if (!ncl) return;
        o->c = ncl;
        char **nu = (char **)realloc(o->url, (size_t)nc * sizeof *nu);
        if (!nu) return;
        o->url = nu;
        o->cap = nc;
    }
    o->r[o->n][0] = x; o->r[o->n][1] = y; o->r[o->n][2] = w; o->r[o->n][3] = h;
    o->c[o->n] = rgba;
    o->url[o->n] = NULL;
    o->n++;
}

void display_list_canvas_reset(void)
{
    for (int i = 0; i < g_cv_n; i++) {
        for (int k = 0; k < g_cv[i].n; k++) free(g_cv[i].url[k]);
        free(g_cv[i].r); free(g_cv[i].c); free(g_cv[i].url);
    }
    g_cv_n = 0;
}

void display_list_canvas_image(uint32_t node, const char *url, float x, float y, float w, float h)
{
    if (!url || !*url) return;
    display_list_canvas_rect(node, x, y, w, h, 0u, 0);
    DlCanvasOps *o = dl_canvas_ops(node, false);
    if (o && o->n > 0 && !o->url[o->n - 1] && o->c[o->n - 1] == 0u) o->url[o->n - 1] = strdup(url);
}

static bool paint_is_context(const LayoutContext *ctx, int i)
{
    const LayoutBox *b = &ctx->boxes[i];
    if (b->s->position == CSS_POSITION_FIXED || b->s->position == CSS_POSITION_STICKY) return true;
    if (b->s->wc_sc == 1) return true;   /* (will-change names what makes one) */
    if ((b->s->abs_cb & (1 | 2 | 4 | 8 | 16 | 512)) && b->s->display != CSS_DISPLAY_INLINE) return true;   /* (transformed: one, were the transform the identity) */
    if (b->s->z_index_set || b->s->wc_sc == 2) {
        if (b->s->position != CSS_POSITION_STATIC) return true;
        int p = ctx->tree.nodes[i].parent_idx;   /* (a flex or grid item) */
        if (p >= 0 && (ctx->boxes[p].s->display == CSS_DISPLAY_FLEX || ctx->boxes[p].s->display == CSS_DISPLAY_GRID))
            return true;
    }
    if (b->s->blend || b->s->filt_n || b->s->filt_url || b->s->bf_n || dl_paint_contained(b)) return true;   /* (mix-blend-mode, filter: a stacking context, painted as z-index 0) */
    return b->s->opacity_set && b->s->opacity < 1.0;
}

typedef struct { PaintVec negs, blocks, floats, inlines, zeros, pos; } PaintLayers;

/* The z-index of a stacking context with a nonzero one (0: none). */
static int paint_hoisted_z(const LayoutContext *ctx, int c)
{
    const LayoutBox *b = &ctx->boxes[c];
    if (!b->s->z_index_set || !paint_is_context(ctx, c)) return 0;
    /* (z-index applies to the positioned and to flex and grid items) */
    if (b->s->position == CSS_POSITION_STATIC) {
        int p = ctx->tree.nodes[c].parent_idx;
        if (!(p >= 0 && (ctx->boxes[p].s->display == CSS_DISPLAY_FLEX || ctx->boxes[p].s->display == CSS_DISPLAY_GRID))) return 0;
    }
    return b->s->z_index;
}

/* The positioned boxes and stacking contexts inside what paints like a
 * stacking context of its own (n): the enclosing context's (L), in tree
 * order (each paints its own subtree, a positioned one with what is
 * positioned in it in turn). */
static void paint_hoist(const LayoutContext *ctx, int n, PaintLayers *L)
{
    for (int c = ctx->tree.nodes[n].first_child_idx; c >= 0; c = ctx->tree.nodes[c].next_sibling_idx) {
        const LayoutBox *b = &ctx->boxes[c];
        if (b->s->display == CSS_DISPLAY_NONE) continue;
        bool sc = paint_is_context(ctx, c);
        if (sc || b->s->position != CSS_POSITION_STATIC) {
            int z = paint_hoisted_z(ctx, c);
            if (z < 0) pv_push(&L->negs, c);
            else if (z > 0) pv_push(&L->pos, c);
            else {
                pv_push(&L->zeros, c);
                if (!sc) paint_hoist(ctx, c, L);
            }
            continue;
        }
        paint_hoist(ctx, c, L);
    }
}

/* A stacking context's descendants into its layers (an atomic inline in
 * inlines as -(index + 2)).  pseudo: n only paints like one (the contexts
 * with a nonzero z-index in it went to the enclosing one). */
static void paint_walk(const LayoutContext *ctx, int n, PaintLayers *L, bool pseudo)
{
    bool flex_or_grid = ctx->boxes[n].s->display == CSS_DISPLAY_FLEX || ctx->boxes[n].s->display == CSS_DISPLAY_GRID;
    /* (flex and grid items: in order-modified document order) */
    int kbuf[64], *kids = kbuf, nk = 0, kcap = 64;
    bool reorder = false;
    for (int c = ctx->tree.nodes[n].first_child_idx; c >= 0; c = ctx->tree.nodes[c].next_sibling_idx) {
        if (nk == kcap) {
            int *nv = (int *)malloc((size_t)kcap * 2 * sizeof(int));
            if (!nv) break;
            memcpy(nv, kids, (size_t)nk * sizeof(int));
            if (kids != kbuf) free(kids);
            kids = nv;
            kcap *= 2;
        }
        kids[nk++] = c;
        if (flex_or_grid && ctx->boxes[c].s->order != 0) reorder = true;
    }
    /* (an absolutely positioned child is no item: order means nothing to it) */
    #define PW_ORD(x) (ctx->boxes[x].s->position == CSS_POSITION_ABSOLUTE || ctx->boxes[x].s->position == CSS_POSITION_FIXED ? 0 : ctx->boxes[x].s->order)
    if (reorder)
        for (int a = 1; a < nk; a++) {
            int x = kids[a], ox = PW_ORD(x), q = a - 1;
            while (q >= 0 && PW_ORD(kids[q]) > ox) { kids[q + 1] = kids[q]; q--; }
            kids[q + 1] = x;
        }
    #undef PW_ORD
    for (int ki = 0; ki < nk; ki++) {
        int c = kids[ki];
        const LayoutBox *b = &ctx->boxes[c];
        if (b->s->display == CSS_DISPLAY_NONE) continue;
        bool sc = paint_is_context(ctx, c);
        if (b->s->position != CSS_POSITION_STATIC || sc) {
            int z = sc ? paint_hoisted_z(ctx, c) : 0;
            if (pseudo) continue;   /* (the enclosing context's, hoisted) */
            if (z < 0 && sc) pv_push(&L->negs, c);
            else if (z > 0 && sc) pv_push(&L->pos, c);
            else {
                pv_push(&L->zeros, c);
                if (!sc && !pseudo) paint_hoist(ctx, c, L);
            }
            continue;
        }
        if (b->s->float_side != 0) {
            pv_push(&L->floats, c);
            if (!pseudo) paint_hoist(ctx, c, L);
            continue;
        }
        HtmlNode *dn = ctx->doc ? css_layout_box_node(ctx, c) : NULL;
        bool text = dn && dn->type == HTML_NODE_TEXT;
        if (text || (b->s->display == CSS_DISPLAY_INLINE && !b->s->inline_level)) {
            pv_push(&L->inlines, c);
            paint_walk(ctx, c, L, pseudo);
        } else if (b->s->display == CSS_DISPLAY_INLINE_BLOCK || b->s->inline_level || flex_or_grid) {
            /* (flex and grid items paint as inline blocks do: each whole, in turn) */
            pv_push(&L->inlines, -(c + 2));
            if (!pseudo) paint_hoist(ctx, c, L);
        } else {
            pv_push(&L->blocks, c);
            paint_walk(ctx, c, L, pseudo);
        }
    }
    if (kids != kbuf) free(kids);
}

/* By z-index, tree order kept among equals. */
static void paint_sort_z(const LayoutContext *ctx, PaintVec *p)
{
    for (int a = 1; a < p->n; a++) {
        int x = p->v[a], zx = ctx->boxes[x].s->z_index, b = a - 1;
        while (b >= 0 && ctx->boxes[p->v[b]].s->z_index > zx) { p->v[b + 1] = p->v[b]; b--; }
        p->v[b + 1] = x;
    }
}

static void paint_context(const LayoutContext *ctx, int root, PaintVec *out, int depth)
{
    pv_push(out, root);
    if (depth > 200) {   /* (pathologically deep: its subtree in tree order) */
        int e = subtree_end(ctx, root);
        for (int k = root + 1; k < e; k++) pv_push(out, k);
        return;
    }
    PaintLayers L;
    memset(&L, 0, sizeof(L));
    paint_walk(ctx, root, &L, root != ctx->tree.root_idx && !paint_is_context(ctx, root));
    paint_sort_z(ctx, &L.negs);
    paint_sort_z(ctx, &L.pos);
    /* (in a 3D rendering context: by depth, the eye's nearest last) */
    if (css_layout_style_keeps3d(ctx->boxes[root].s)) {
        /* (its own children: what is deeper in is flattened into one of them, at no depth of its own) */
        #define PC_D3(x) (ctx->tree.nodes[x].parent_idx == root ? css_layout_box_depth3d(ctx, x) : 0.0)
        for (int a = 1; a < L.zeros.n; a++) {
            int x = L.zeros.v[a], b = a - 1;
            double zx = PC_D3(x);
            while (b >= 0 && PC_D3(L.zeros.v[b]) > zx) { L.zeros.v[b + 1] = L.zeros.v[b]; b--; }
            L.zeros.v[b + 1] = x;
        }
        /* (behind the plane of its content: under that) */
        int nb = 0;
        while (nb < L.zeros.n && PC_D3(L.zeros.v[nb]) < 0.0) nb++;
        for (int k = 0; k < nb; k++) pv_push(&L.negs, L.zeros.v[k]);
        if (nb) { memmove(L.zeros.v, L.zeros.v + nb, (size_t)(L.zeros.n - nb) * sizeof L.zeros.v[0]); L.zeros.n -= nb; }
        #undef PC_D3
    }
    for (int k = 0; k < L.negs.n; k++) paint_context(ctx, L.negs.v[k], out, depth + 1);
    for (int k = 0; k < L.blocks.n; k++) pv_push(out, L.blocks.v[k]);
    for (int k = 0; k < L.floats.n; k++) paint_context(ctx, L.floats.v[k], out, depth + 1);
    for (int k = 0; k < L.inlines.n; k++) {
        int x = L.inlines.v[k];
        if (x >= 0) pv_push(out, x);
        else paint_context(ctx, -x - 2, out, depth + 1);
    }
    for (int k = 0; k < L.zeros.n; k++) paint_context(ctx, L.zeros.v[k], out, depth + 1);
    for (int k = 0; k < L.pos.n; k++) paint_context(ctx, L.pos.v[k], out, depth + 1);
    free(L.negs.v); free(L.blocks.v); free(L.floats.v); free(L.inlines.v); free(L.zeros.v); free(L.pos.v);
}

/* Move what a box painted, [from, dl->count) and masks [mask_from, ...), by
 * its scrolled ancestors' offsets. */
static void dl_scroll_range(DisplayList *dl, int from, int mask_from, const float o[2])
{
    if (o[0] == 0.0f && o[1] == 0.0f) return;
    for (int k = from; k < dl->count; k++) {
        DisplayListCmd *cmd = &dl->cmds[k];
        cmd->x -= o[0];
        cmd->y -= o[1];
        if (cmd->type == DL_BORDER) { cmd->u.border.x -= o[0]; cmd->u.border.y -= o[1]; }
    }
    for (int k = mask_from; k < dl->mask_count; k++) {
        DisplayMask *m = &dl->masks[k];
        m->x -= o[0];
        m->y -= o[1];
        m->ix -= o[0];
        m->iy -= o[1];
    }
}

static bool build_display_list_impl(LayoutContext *ctx, DisplayList *dl, float (*clip)[4], float (*off)[2])
{
    bool dark_mode = document_is_dark_mode(ctx);
    TextShaper *default_font = display_list_get_default_font();
    int pending_box = -1, pending_from = 0, pending_mask = 0;
    /* Open masks: each covers its box's subtree, [start, end) of the layout
     * indices.  Every such mask is also noted (reg_*), so a descendant
     * painted after its subtree was left (a positioned one) opens it again. */
    int mask_start[64], mask_end[64], mask_idx[64], nmask = 0;
    enum { REG_MAX = 512 };
    int reg_start[REG_MAX], reg_end[REG_MAX], reg_idx[REG_MAX], nreg = 0;
    int canvas_src = canvas_background(ctx, dl);
    if (canvas_src == -2) return false;

    /* (kept from build to build on this thread) */
    static __thread PaintVec order;
    order.n = 0;
    if (ctx->tree.root_idx >= 0) paint_context(ctx, ctx->tree.root_idx, &order, 0);
    bool any_mc = false;
    for (int k = 0; k < ctx->tree.count && !any_mc; k++) any_mc = ctx->boxes[k].mc_n > 0;
    /* (transforms beyond a translation: each box's commands under its own
     * and its ancestors', a DL_XFORM where that changes) */
    bool any_tf = false;
    for (int k = 0; k < ctx->tree.count && !any_tf; k++) {
        const LayoutStyle *ks = ctx->boxes[k].s;
        any_tf = ks && (ks->tf_text || ks->tf_rotate || ks->tf_scale || ks->op_kind);
    }
    bool any3d = false;
    /* (only what paints: under display: none a box may never have been styled) */
    for (int k = 0; k < order.n && !any3d; k++) {
        const LayoutStyle *ks = ctx->boxes[order.v[k]].s;
        if (ks && ks->display != CSS_DISPLAY_NONE) any3d = css_layout_style_3d(ks);
    }
    t_mc_flat = !any3d;
    if (any3d) any_tf = true;
    bool pending_hidden = false;
    /* (a box in columns opened a mask over its subtree: what it and the subtree
     * paint goes through the columns as one, once the mask has closed) */
    bool mc_hold = false;
    int hold_from = 0, hold_box = -1, hold_level = 0;
    float hold_xf[6];
    float range_xf[6] = { 1, 0, 0, 1, 0, 0 };
    float cur_xf[6] = { 1, 0, 0, 1, 0, 0 };
    enum { CBMAX = 256 };
    int cbt[CBMAX], cba[CBMAX], cbb[CBMAX], ncb = 0;   /* (collapsed cell borders waiting for their table's end) */

    for (int k = 0; k < order.n; k++) {
        int i = order.v[k];
        /* (facing away, hidden: what it painted taken back) */
        if (pending_box >= 0 && pending_hidden) { dl->count = pending_from; dl->mask_count = pending_mask; }
        pending_hidden = false;
        if (pending_box >= 0 && off) dl_scroll_range(dl, pending_from, pending_mask, off[pending_box]);
        if (pending_box >= 0 && clip) dl_clip_range(dl, pending_from, clip[pending_box]);
        if (pending_box >= 0 && any_mc) {
            dl_bdb_clone(ctx, dl, pending_from, pending_box, 1.0f);
            if (!mc_hold) {
                int ml = -1, mch0 = -1;
                for (int q = 0; q < nmask && ml < 0; q++) if (mask_start[q] == pending_box) ml = q;
                if (ml >= 0 && mc_owner(ctx, pending_box, &mch0) >= 0) {
                    mc_hold = true;
                    hold_from = pending_from;
                    hold_box = pending_box;
                    hold_level = ml;
                    memcpy(hold_xf, range_xf, sizeof hold_xf);
                }
            }
            /* (in a multicol container in another's columns: its columns, then those) */
            if (!(mc_hold && pending_box == hold_box)) dl_mc_chain(ctx, dl, pending_from, pending_box, mc_hold ? hold_box : -1, range_xf);
        }
        if (ncb) dl_cb_flush(ctx, dl, cbt, cba, cbb, &ncb, i);
        int cbr_from = -1;
        while (nmask > 0 && !mask_covers(ctx, i, mask_start[nmask - 1], mask_end[nmask - 1])) {
            nmask--;
            if (mask_idx[nmask] <= -1000) { dl_ref_filter_end(ctx, dl, mask_start[nmask], -1000 - mask_idx[nmask]); }
            else dl_add_mask_cmd(dl, DL_MASK_END, mask_idx[nmask], &dl->masks[mask_idx[nmask]]);
            if (mc_hold && nmask <= hold_level) {
                mc_hold = false;
                dl_mc_chain(ctx, dl, hold_from, hold_box, -1, hold_xf);
            }
        }
        for (int r = 0; r < nreg && nmask < 64; r++) {
            if (i == reg_start[r] || !mask_covers(ctx, i, reg_start[r], reg_end[r])) continue;
            bool open = false;
            for (int m = 0; m < nmask && !open; m++) open = mask_idx[m] == reg_idx[r];
            if (open || !dl_add_mask_cmd(dl, DL_MASK_BEGIN, reg_idx[r], &dl->masks[reg_idx[r]])) continue;
            mask_start[nmask] = reg_start[r];
            mask_end[nmask] = reg_end[r];
            mask_idx[nmask] = reg_idx[r];
            nmask++;
        }
        if (any_tf) {
            double m[6] = { 1, 0, 0, 1, 0, 0 };
            if (any3d) css_layout_box_map3d(ctx, i, m, &pending_hidden);
            else if (!css_layout_box_map(ctx, i, -1, m)) { m[0] = m[3] = 1.0; m[1] = m[2] = m[4] = m[5] = 0.0; }
            float f[6];
            for (int q = 0; q < 6; q++) f[q] = (float)m[q];
            if (memcmp(f, cur_xf, sizeof f) && display_list_reserve(dl, 1)) {
                DisplayListCmd *xc = &dl->cmds[dl->count++];
                memset(xc, 0, sizeof *xc);
                xc->type = DL_XFORM;
                memcpy(xc->u.xf, f, sizeof f);
                memcpy(cur_xf, f, sizeof f);
            }
        }
        memcpy(range_xf, cur_xf, sizeof range_xf);
        pending_box = i;
        pending_from = dl->count;
        pending_mask = dl->mask_count;
        LayoutBox *box = &ctx->boxes[i];
        /* The pieces of a split inline that start with this box: where their
         * line is in the paint order. */
        int par = ctx->tree.nodes[i].parent_idx;
        if (par >= 0 && ctx->pieces && par < ctx->pieces_n && ctx->pieces[par].n > 0) {
            const LayoutBox *pb = &ctx->boxes[par];
            if (pb->s->block_in_inline == 2 && (pb->flags & LAYOUT_HAS_LAYOUT) && pb->s->display != CSS_DISPLAY_NONE &&
                pb->s->visibility != CSS_VISIBILITY_HIDDEN && pb->opacity_eff >= 0.004 &&
                !box_or_ancestor_hidden(ctx, par))
                paint_pieces(ctx, dl, par, i, (float)pb->opacity_eff);
        }
        if (!(box->flags & LAYOUT_HAS_LAYOUT)) continue;
        if (box->s->display == CSS_DISPLAY_NONE) continue;
        if (box->s->visibility == CSS_VISIBILITY_HIDDEN || box->s->visibility == CSS_VISIBILITY_COLLAPSE) continue;
        /* Effective opacity (product over ancestors) scales every paint. */
        float op = (float)box->opacity_eff;
        if (op < 0.004f) continue;
        if (box_or_ancestor_hidden(ctx, i)) continue;
        /* (images a box has though nothing shows of them: fetched all the same) */
        if (g_image_cache && box->s->shape_image_url) {
            http_mark_cors_url(box->s->shape_image_url);
            image_cache_request(g_image_cache, box->s->shape_image_url, 0, 0);
        }
        if (g_image_cache && box->s->background_image_url[0] && (box->width <= 0.0f || box->height <= 0.0f))
            image_cache_request(g_image_cache, box->s->background_image_url, 0, 0);

        HtmlNode *node = NULL;
        if (ctx->doc) {
            node = css_layout_box_node(ctx, i);
        }

        /* backdrop-filter: what is under the box filtered within its border box,
         * the box and its subtree then over it (a mask of kind 10, its functions
         * as a filter's) */
        if (box->s->bf_n && nmask < 64) {
            DisplayMask m;
            memset(&m, 0, sizeof m);
            m.kind = 10;
            m.image_handle = -1;
            m.x = (float)box->x; m.y = (float)box->y; m.w = (float)box->width; m.h = (float)box->height;
            m.nstops = box->s->bf_n;
            /* (its corners: horizontal in border, vertical in ex_ry) */
            for (int c = 0; c < 4; c++) {
                bool pct = (box->s->radius_pct >> c) & 1;
                m.border[c] = (float)(pct ? box->s->radius[c] * box->width : box->s->radius[c]);
                m.ex_ry[c] = (float)LAYOUT_RADIUS_Y(box->s, c, box->height);
            }
            for (int k = 0; k < box->s->bf_n && k < 8; k++) {
                m.stop_pos[k] = box->s->bf_t[k] / 32.0f;
                float v = box->s->bf_v[k];
                if (box->s->bf_t[k] == 4) { v = fmodf(v, 360.0f); if (v < 0.0f) v += 360.0f; }
                if (k < 4) m.rx[k] = v; else m.ry[k - 4] = v;
            }
            int mi = dl_add_mask(dl, &m);
            if (mi >= 0 && dl_add_mask_cmd(dl, DL_MASK_BEGIN, mi, &m)) {
                mask_start[nmask] = i;
                mask_end[nmask] = subtree_end(ctx, i);
                mask_idx[nmask] = mi;
                nmask++;
            }
        }
        /* filter with a url(): the box and its subtree rasterized here, filtered
         * on the CPU, drawn as an image (mask_idx: where its commands begin) */
        /* (a mask of several layers: outside the filter; a header and its layers) */
        bool mask_cpu = dl_mask_is_layered(box);
        if (mask_cpu && nmask < 64) {
            int mi = dl_layered_mask(ctx, dl, box);
            if (mi >= 0 && dl_add_mask_cmd(dl, DL_MASK_BEGIN, mi, &dl->masks[mi])) {
                mask_start[nmask] = i;
                mask_end[nmask] = subtree_end(ctx, i);
                mask_idx[nmask] = mi;
                if (nreg < REG_MAX && mask_end[nmask] > i + 1) {
                    reg_start[nreg] = i; reg_end[nreg] = mask_end[nmask]; reg_idx[nreg] = mi; nreg++;
                }
                nmask++;
            }
        }
        bool filt_cpu = dl_box_filter_cpu(box, any_tf);
        if (filt_cpu && nmask < 64) {
            mask_start[nmask] = i;
            mask_end[nmask] = subtree_end(ctx, i);
            mask_idx[nmask] = -1000 - dl->count;
            nmask++;
        }
        /* filter: the box and its subtree painted apart, then filtered over
         * what is under them (a mask of kind 9: the functions' kinds in
         * stop_pos (as kind / 32), amounts in rx / ry, a drop-shadow's
         * offsets in ix / iy, its blur in blur and color in border) */
        if (box->s->filt_n && !filt_cpu && nmask < 64) {
            DisplayMask m;
            memset(&m, 0, sizeof m);
            m.kind = 9;
            m.image_handle = -1;
            m.x = (float)box->x; m.y = (float)box->y; m.w = (float)box->width; m.h = (float)box->height;
            m.nstops = box->s->filt_n;
            for (int k = 0; k < box->s->filt_n && k < 8; k++) {
                m.stop_pos[k] = box->s->filt_t[k] / 32.0f;
                float v = box->s->filt_v[k];
                if (box->s->filt_t[k] == 4) { v = fmodf(v, 360.0f); if (v < 0.0f) v += 360.0f; }
                if (k < 4) m.rx[k] = v; else m.ry[k - 4] = v;
            }
            m.ix = box->s->filt_ds[0]; m.iy = box->s->filt_ds[1]; m.blur = box->s->filt_ds[2];
            m.border[0] = box->s->filt_ds[3] * 255.0f; m.border[1] = box->s->filt_ds[4] * 255.0f;
            m.border[2] = box->s->filt_ds[5] * 255.0f; m.border[3] = box->s->filt_ds[6] * 255.0f;
            int mi = dl_add_mask(dl, &m);
            if (mi >= 0 && dl_add_mask_cmd(dl, DL_MASK_BEGIN, mi, &m)) {
                mask_start[nmask] = i;
                mask_end[nmask] = subtree_end(ctx, i);
                mask_idx[nmask] = mi;
                nmask++;
            }
        }
        /* mix-blend-mode: the box and its subtree painted apart, then blended
         * over what is under them (a mask of kind 8) */
        if (box->s->blend && nmask < 64) {
            DisplayMask m;
            memset(&m, 0, sizeof m);
            m.kind = 8;
            m.image_handle = -1;
            m.x = (float)box->x; m.y = (float)box->y; m.w = (float)box->width; m.h = (float)box->height;
            m.ix = -400000.0f; m.iy = -400000.0f; m.iw = 800000.0f; m.ih = 800000.0f;
            m.angle_deg = (float)box->s->blend;
            int mi = dl_add_mask(dl, &m);
            if (mi >= 0 && dl_add_mask_cmd(dl, DL_MASK_BEGIN, mi, &m)) {
                mask_start[nmask] = i;
                mask_end[nmask] = subtree_end(ctx, i);
                mask_idx[nmask] = mi;
                nmask++;
            }
        }
        /* mask-image: everything the box and its subtree paint goes through
         * the mask (closed above once the subtree is done). */
        if (box->s->mask_kind && !mask_cpu && nmask < 64) {
            DisplayMask m;
            if (box_mask(box, &m)) {
                int mi = dl_add_mask(dl, &m);
                if (mi >= 0 && dl_add_mask_cmd(dl, DL_MASK_BEGIN, mi, &m)) {
                    mask_start[nmask] = i;
                    mask_end[nmask] = subtree_end(ctx, i);
                    mask_idx[nmask] = mi;
                    if (nreg < REG_MAX && mask_end[nmask] > i + 1) {
                        reg_start[nreg] = i; reg_end[nreg] = mask_end[nmask]; reg_idx[nreg] = mi; nreg++;
                    }
                    nmask++;
                }
                /* (mask-clip: padding or content box: nothing painted outside it) */
                if (mi >= 0 && (box->s->mask_clip == 1 || box->s->mask_clip == 2) && nmask < 64) {
                    DisplayMask cm;
                    memset(&cm, 0, sizeof cm);
                    cm.kind = 3;
                    cm.image_handle = -1;
                    box_area_rect(box, box->s->mask_clip, &cm.x, &cm.y, &cm.w, &cm.h);
                    int ci = dl_add_mask(dl, &cm);
                    if (ci >= 0 && dl_add_mask_cmd(dl, DL_MASK_BEGIN, ci, &cm)) {
                        mask_start[nmask] = i;
                        mask_end[nmask] = subtree_end(ctx, i);
                        mask_idx[nmask] = ci;
                        nmask++;
                    }
                }
            }
        }
        /* clip: rect() on an absolutely positioned box: its subtree cut to the rectangle
         * (offsets from its border box's corner; auto the border box's edge) */
        if (box->s->clip_on && (box->s->position == CSS_POSITION_ABSOLUTE || box->s->position == CSS_POSITION_FIXED) && nmask < 64) {
            DisplayMask cm;
            memset(&cm, 0, sizeof cm);
            cm.kind = 3;
            cm.image_handle = -1;
            unsigned char au = box->s->clip_auto;
            float t = (au & 1) ? 0.0f : box->s->clip_v[0], r = (au & 2) ? (float)box->width : box->s->clip_v[1];
            float b = (au & 4) ? (float)box->height : box->s->clip_v[2], l = (au & 8) ? 0.0f : box->s->clip_v[3];
            cm.x = (float)box->x + l;
            cm.y = (float)box->y + t;
            cm.w = r - l > 0.0f ? r - l : 0.0f;
            cm.h = b - t > 0.0f ? b - t : 0.0f;
            cm.ix = -400000.0f; cm.iy = -400000.0f; cm.iw = 800000.0f; cm.ih = 800000.0f;
            int ci = dl_add_mask(dl, &cm);
            if (ci >= 0 && dl_add_mask_cmd(dl, DL_MASK_BEGIN, ci, &cm)) {
                mask_start[nmask] = i;
                mask_end[nmask] = subtree_end(ctx, i);
                mask_idx[nmask] = ci;
                nmask++;
            }
        }
        /* clip-path: a shape over the box and its subtree, all outside it gone
         * (the mask's region far beyond its shape: ix.. kind 3's region) */
        if (box->s->clip_path && nmask < 64) {
            DisplayMask m;
            t_cp_vw = ctx->viewport_width;
            t_cp_vh = ctx->viewport_height;
            t_cp_doc = ctx->doc;
            t_cp_n = 0;
            if (box_clip_mask(box, &m)) {
                m.ix = -400000.0f; m.iy = -400000.0f; m.iw = 800000.0f; m.ih = 800000.0f;
                int mi = dl_add_mask(dl, &m);
                /* (a polygon of more than ten corners: ten more in each mask after it) */
                for (int k0 = 10; mi >= 0 && m.kind == 7 && k0 < t_cp_n; k0 += 10) {
                    DisplayMask c;
                    memset(&c, 0, sizeof c);
                    c.kind = 11;
                    c.image_handle = -1;
                    c.x = m.x; c.y = m.y; c.w = m.w; c.h = m.h;
                    c.nstops = 1;
                    float *slot[20];
                    for (int k = 0; k < 4; k++) { slot[k] = &c.rx[k]; slot[4 + k] = &c.ry[k]; slot[8 + k] = &c.border[k]; slot[12 + k] = &c.ex_rx[k]; slot[16 + k] = &c.ex_ry[k]; }
                    for (int k = k0; k < t_cp_n && k < k0 + 10; k++) { *slot[2 * (k - k0)] = t_cp_px[k]; *slot[2 * (k - k0) + 1] = t_cp_py[k]; }
                    if (dl_add_mask(dl, &c) < 0) break;
                }
                if (mi >= 0 && dl_add_mask_cmd(dl, DL_MASK_BEGIN, mi, &m)) {
                    mask_start[nmask] = i;
                    mask_end[nmask] = subtree_end(ctx, i);
                    mask_idx[nmask] = mi;
                    if (nreg < REG_MAX && mask_end[nmask] > i + 1) {
                        reg_start[nreg] = i; reg_end[nreg] = mask_end[nmask]; reg_idx[nreg] = mi; nreg++;
                    }
                    nmask++;
                }
            }
        }
        /* box-shadow (outer): below the box, outside its rounded rectangle. */
        /* (an empty box spread out casts one too) */
        if (box->s->shadow_set && ((box->width > 0.0 && box->height > 0.0) || (box->s->shadow_spread > 0.0f && box->width >= 0.0 && box->height >= 0.0))) {
            DisplayMask sm, rr;
            memset(&sm, 0, sizeof(sm));
            sm.kind = 5;
            sm.image_handle = -1;
            float sp = box->s->shadow_spread;
            sm.x = (float)box->x + box->s->shadow_x - sp;
            sm.y = (float)box->y + box->s->shadow_y - sp;
            sm.w = (float)box->width + 2.0f * sp;
            sm.h = (float)box->height + 2.0f * sp;
            sm.blur = box->s->shadow_blur;
            sm.ix = (float)box->x; sm.iy = (float)box->y;
            sm.iw = (float)box->width; sm.ih = (float)box->height;
            if (box_round_mask(box, &rr)) {
                for (int c = 0; c < 4; c++) {
                    sm.ex_rx[c] = rr.rx[c]; sm.ex_ry[c] = rr.ry[c];
                    sm.rx[c] = rr.rx[c] > 0.0f ? rr.rx[c] + sp : 0.0f;
                    sm.ry[c] = rr.ry[c] > 0.0f ? rr.ry[c] + sp : 0.0f;
                    if (sm.rx[c] < 0.0f) sm.rx[c] = 0.0f;
                    if (sm.ry[c] < 0.0f) sm.ry[c] = 0.0f;
                }
            }
            if (sm.w > 0.0f && sm.h > 0.0f) {
                int mi = dl_add_mask(dl, &sm);
                if (mi >= 0 && display_list_reserve(dl, 1)) {
                    DisplayListCmd *cmd = &dl->cmds[dl->count++];
                    memset(cmd, 0, sizeof(*cmd));
                    cmd->type = DL_SHADOW;
                    float pad = sm.blur;
                    cmd->x = sm.x - pad; cmd->y = sm.y - pad;
                    cmd->w = sm.w + 2.0f * pad; cmd->h = sm.h + 2.0f * pad;
                    cmd->r = box->s->shadow_r; cmd->g = box->s->shadow_g; cmd->b = box->s->shadow_b;
                    cmd->a = box->s->shadow_a * op;
                    cmd->u.mask_index = mi;
                }
            }
        }
        /* border-radius: rounds the box's own background and border, and
         * clips the whole subtree when the box clips (overflow) or is a
         * replaced element (rounded thumbnails, avatars). */
        if (nmask < 64) {
            DisplayMask rm;
            if (box_round_mask(box, &rm)) {
                int mi = dl_add_mask(dl, &rm);
                if (mi >= 0 && dl_add_mask_cmd(dl, DL_MASK_BEGIN, mi, &rm)) {
                    mask_start[nmask] = i;
                    mask_end[nmask] = (box->s->overflow != 0 || is_replaced_tag(node)) ? subtree_end(ctx, i) : i + 1;
                    mask_idx[nmask] = mi;
                    if (nreg < REG_MAX && mask_end[nmask] > i + 1) {
                        reg_start[nreg] = i; reg_end[nreg] = mask_end[nmask]; reg_idx[nreg] = mi; nreg++;
                    }
                    nmask++;
                }
            }
        }

        /* An inline split around its blocks, or into pieces by bidi
         * reordering: its pieces' backgrounds and borders, not its own box's
         * (those with a child paint before it). */
        const LayoutPieceList *pieces = ctx->pieces && i < ctx->pieces_n &&
                                        (box->s->block_in_inline == 2 || ctx->pieces[i].n > 0)
                                        ? &ctx->pieces[i] : NULL;
        if (pieces) paint_pieces(ctx, dl, i, -1, op);

        /* (a table's captions above it are outside its box: its background
         * and border below them) */
        float cap_bot = 0.0f;
        float cap_top = box->s->table_role == 1 ? table_caption_top(ctx, i, box, &cap_bot) : 0.0f;
        bool hid_cell = cell_hidden_empty(ctx, i);

        /* Backgrounds paint first: under the borders, list marker and the
         * box's content. */
        /* (a color however thin: a 1px box is a line) */
        /* (background-clip: border-area: the backgrounds through the border's ring) */
        int ba_mi = -1;
        DisplayMask ba_m;
        if (box->s->bg_clip == 4 && !pieces && box->width > 0.0f && box->height > 0.0f) {
            if (!box_round_mask(box, &ba_m)) {
                memset(&ba_m, 0, sizeof ba_m);
                ba_m.image_handle = -1;
                ba_m.x = (float)box->x; ba_m.y = (float)box->y; ba_m.w = (float)box->width; ba_m.h = (float)box->height;
            }
            ba_m.kind = 4;
            ba_m.border[0] = (float)box->s->border_top; ba_m.border[1] = (float)box->s->border_right;
            ba_m.border[2] = (float)box->s->border_bottom; ba_m.border[3] = (float)box->s->border_left;
            ba_mi = dl_add_mask(dl, &ba_m);
            if (ba_mi >= 0 && !dl_add_mask_cmd(dl, DL_MASK_BEGIN, ba_mi, &ba_m)) ba_mi = -1;
        }
        if (!pieces && !hid_cell && i != canvas_src && box->width > 0.0f && box->height > cap_top + cap_bot && box->s->background_color_a > 0.0f &&
            box->s->bg_clip != 3) {
            float bgx = (float)box->x, bgy = (float)box->y + cap_top, bgw = (float)box->width, bgh = (float)box->height - cap_top - cap_bot;
            if (box->s->bg_clip && box->s->bg_clip != 4) {
                /* (background-clip: the padding or content box) */
                float l = (float)box->s->border_left, t = (float)box->s->border_top, r = (float)box->s->border_right, b = (float)box->s->border_bottom;
                if (box->s->bg_clip == 2) { l += (float)box->padding_left; t += (float)box->padding_top; r += (float)box->padding_right; b += (float)box->padding_bottom; }
                bgx += l; bgy += t; bgw -= l + r; bgh -= t + b;
            }
            snap_rect(&bgx, &bgy, &bgw, &bgh);
            if (bgw > 0.0f && bgh > 0.0f)
            if (!display_list_add_rect(dl, bgx, bgy, bgw, bgh, (float)box->s->background_color_r,
                                       (float)box->s->background_color_g, (float)box->s->background_color_b,
                                       (float)box->s->background_color_a * op))
                return false;
        }
        if (!pieces && box->width >= 2.0f && box->height >= 2.0f && box->width * box->height >= 6.0f) {
            if (g_image_cache && (box->s->background_image_url[0] || box->s->bg_grad) && i != canvas_src && !hid_cell && box->s->bg_clip != 3) {
                float bca[4];
                const float *bcp = NULL;
                if (box->s->bg_clip && box->s->bg_clip != 4) {
                    /* (clipped to the padding or content box) */
                    float l = (float)box->s->border_left, t = (float)box->s->border_top, r = (float)box->s->border_right, b = (float)box->s->border_bottom;
                    if (box->s->bg_clip == 2) { l += (float)box->padding_left; t += (float)box->padding_top; r += (float)box->padding_right; b += (float)box->padding_bottom; }
                    bca[0] = (float)box->x + l; bca[1] = (float)box->y + t;
                    bca[2] = (float)(box->x + box->width) - r; bca[3] = (float)(box->y + box->height) - b;
                    bcp = bca;
                }
                if (!bcp || (bca[2] > bca[0] && bca[3] > bca[1])) emit_background_image(ctx, dl, box, bcp);
            }
        }
        if (ba_mi >= 0) dl_add_mask_cmd(dl, DL_MASK_END, ba_mi, &ba_m);

        /* Replaced images: <img src="..."> */
        if (node && node->type == HTML_NODE_ELEMENT &&
            (strcasecmp(node->tag_name, "img") == 0 || display_list_object_image(node)) && g_image_cache) {
            /* (content-visibility: hidden: its image is its content, skipped) */
            char *url = box->s->cv_hidden ? NULL : display_list_image_source(ctx->base_url, node, (float)box->width);
            if (url && box->s->img_orient_none) url = display_list_noexif(url, ctx->base_url);
            if (url) {
                emit_image_async(dl, box, url, op);
                free(url);
            }
            /* (its border, which the rest of the loop paints) */
            goto replaced_border;
        }

        /* Inline <svg>: the outermost one paints its whole subtree. */
        if (node && node->type == HTML_NODE_ELEMENT && node->tag_name &&
            strcasecmp(node->tag_name, "svg") == 0 && g_image_cache && ctx->doc) {
            int di = ctx->tree.nodes[i].dom_node_idx;
            if (!inside_svg(&ctx->doc->array, di) && !box->s->cv_hidden) emit_inline_svg(ctx, dl, box, di, op);
            continue;
        }

        /* <video>: the frame its player shows now, letterboxed (object-fit
         * defaults to contain for video). */
        if (node && node->type == HTML_NODE_ELEMENT && node->store_id &&
            strcasecmp(node->tag_name, "video") == 0 && g_image_cache) {
            const uint8_t *px = NULL;
            int vw = 0, vh = 0;
            uint64_t frame_id = 0;
            if (media_video_frame_for_node(node->store_id, &px, &vw, &vh, &frame_id)) {
                char key[48];
                snprintf(key, sizeof(key), "cyber-video-frame:%u", node->store_id);
                image_cache_put_rgba(g_image_cache, key, vw, vh, px);
                LayoutBox vbox = *box;
                LayoutStyle vstyle = *box->s;
                vbox.s = &vstyle;
                if (vstyle.object_fit == 0) vstyle.object_fit = 1;
                emit_image_async(dl, &vbox, key, op);
            }
            continue;
        }

        /* (an iframe: its frame's document) */
        if (node && node->type == HTML_NODE_ELEMENT && node->store_id && node->tag_name && !box->s->cv_hidden && box->s->visibility != CSS_VISIBILITY_HIDDEN &&
            (!clip || clip[i][0] <= clip[i][2]) &&
            (!strcasecmp(node->tag_name, "iframe") || !strcasecmp(node->tag_name, "object") || !strcasecmp(node->tag_name, "embed") || !strcasecmp(node->tag_name, "frame")))
            dl_emit_frame(dl, box, node->store_id);
        /* (a 2D canvas: its solid rectangles, from its bitmap's pixels to its content box;
         * an element a paint worklet drew for: from its padding box's corner) */
        if (node && node->type == HTML_NODE_ELEMENT && node->store_id) {
            const DlCanvasOps *co = dl_canvas_ops(node->store_id, false);
            if (co && co->n > 0 && !box->s->cv_hidden) {   /* (content-visibility: hidden: its drawing is its content, not painted) */
                bool is_cv = strcasecmp(node->tag_name, "canvas") == 0;
                const char *wa = is_cv ? node_attribute_value(node, "width") : NULL, *ha = is_cv ? node_attribute_value(node, "height") : NULL;
                double bw = wa && *wa ? atof(wa) : 300.0, bh = ha && *ha ? atof(ha) : 150.0;
                double pd = is_cv ? 1.0 : 0.0;
                double cx = box->x + box->s->border_left + box->padding_left * pd, cy = box->y + box->s->border_top + box->padding_top * pd;
                double cw = box->width - box->s->border_left - box->s->border_right - (box->padding_left + box->padding_right) * pd;
                double ch = box->height - box->s->border_top - box->s->border_bottom - (box->padding_top + box->padding_bottom) * pd;
                if (!is_cv) { bw = cw; bh = ch; }
                if (bw > 0.0 && bh > 0.0 && cw > 0.0 && ch > 0.0) {
                    /* (the bitmap placed as object-fit and object-position say, clipped to the content box) */
                    int of = is_cv ? box->s->object_fit : 0;
                    double sx = cw / bw, sy = ch / bh;
                    if (of == 1 || of == 2 || of == 4) {
                        double s = of == 2 ? fmax(sx, sy) : fmin(sx, sy);
                        if (of == 4 && s > 1.0) s = 1.0;
                        sx = sy = s;
                    } else if (of == 3) sx = sy = 1.0;
                    double pxf = !is_cv ? 0.0 : box->s->obj_pos_set ? box->s->obj_xf : 0.5, pyf = !is_cv ? 0.0 : box->s->obj_pos_set ? box->s->obj_yf : 0.5;
                    double pxp = is_cv && box->s->obj_pos_set ? box->s->obj_xpx : 0.0, pyp = is_cv && box->s->obj_pos_set ? box->s->obj_ypx : 0.0;
                    double bx0 = cx + (cw - bw * sx) * pxf + pxp, by0 = cy + (ch - bh * sy) * pyf + pyp;
                    int from_c = dl->count;
                    for (int q = 0; q < co->n; q++) {
                        double x0 = co->r[q][0], y0 = co->r[q][1], x1 = x0 + co->r[q][2], y1 = y0 + co->r[q][3];
                        if (co->url[q]) {
                            /* (an image: drawn whole, the bitmap's edges cutting it) */
                            int hd = g_image_cache ? image_cache_request(g_image_cache, co->url[q], 0, 0) : -1;
                            if (hd >= 0) emit_image(dl, (float)(bx0 + x0 * sx), (float)(by0 + y0 * sy), (float)((x1 - x0) * sx), (float)((y1 - y0) * sy), hd);
                            continue;
                        }
                        if (x0 < 0.0) x0 = 0.0;
                        if (y0 < 0.0) y0 = 0.0;
                        if (x1 > bw) x1 = bw;
                        if (y1 > bh) y1 = bh;
                        if (x1 <= x0 || y1 <= y0) continue;
                        uint32_t v = co->c[q];
                        display_list_add_rect(dl, (float)(bx0 + x0 * sx), (float)(by0 + y0 * sy), (float)((x1 - x0) * sx), (float)((y1 - y0) * sy),
                                              (float)(v >> 24) / 255.0f, (float)((v >> 16) & 255) / 255.0f, (float)((v >> 8) & 255) / 255.0f,
                                              (float)(v & 255) / 255.0f * op);
                    }
                    /* (inside the content box and the bitmap) */
                    float cc[4] = { (float)fmax(cx, bx0), (float)fmax(cy, by0), (float)fmin(cx + cw, bx0 + bw * sx), (float)fmin(cy + ch, by0 + bh * sy) };
                    /* (a canvas: overflow: visible shows what is past its box, the bitmap's edges still) */
                    if (is_cv && box->s->ovf_visible_given && box->s->overflow == 0) { cc[0] = (float)bx0; cc[1] = (float)by0; cc[2] = (float)(bx0 + bw * sx); cc[3] = (float)(by0 + bh * sy); }
                    if (dl->count > from_c) dl_clip_range(dl, from_c, cc);
                }
            }
        }
        /* <canvas> with a WebGL or WebGPU context: its last presented frame
         * (browser/webgpu_api.c puts it in the image cache), stretched to
         * the content box as a canvas is. */
        if (node && node->type == HTML_NODE_ELEMENT && node->store_id &&
            strcasecmp(node->tag_name, "canvas") == 0 && g_image_cache && webgpu_canvas_has_frame(node->store_id)) {
            char key[48];
            snprintf(key, sizeof(key), "cyber-canvas:%u", node->store_id);
            LayoutBox cbox = *box;
            LayoutStyle cstyle = *box->s;
            cbox.s = &cstyle;
            /* (object-fit and object-position: of the canvas's bitmap, as for an image) */
            emit_image_async(dl, &cbox, key, op);
            continue;
        }

        /* Emit glyphs for text nodes when a default font is available.
         * Text nodes often have no measured box size in this layout engine,
         * so emit them before the visibility/size filters.
         * Skip nodes whose box has collapsed to the viewport root; they have
         * not been positioned and would all overlap at the origin. */
        if (default_font && ctx->doc) {
            if (node && node->type == HTML_NODE_TEXT &&
                css_layout_node_text(ctx, node) && css_layout_node_text(ctx, node)[0] &&
                !text_is_whitespace(css_layout_node_text(ctx, node))) {
                bool root_sized = (box->x == 0.0 && box->y == 0.0 &&
                                   box->width == ctx->viewport_width &&
                                   box->height == ctx->viewport_height);
                if (!root_sized) {
                    float tr = (float)box->s->color_r;
                    float tg = (float)box->s->color_g;
                    float tb = (float)box->s->color_b;
                    float ta = (float)box->s->color_a;
                    /* (transparent text paints nothing) */
                    ta *= op;
                    if (box->s->font_size < 0.5) ta = 0.0f;   /* (font-size 0) */
                    /* In dark mode, default-black text is invisible on dark
                     * backgrounds.  Render it white so labels are readable. */
                    if (dark_mode && tr < 0.1f && tg < 0.1f && tb < 0.1f) {
                        tr = 1.0f; tg = 1.0f; tb = 1.0f;
                    }
                    /* Scale glyph metrics for the box's font size; the shaper
                     * itself was loaded at 16px. */
                    double fs = box->s->font_size > 0.0 ? box->s->font_size : 16.0;
                    float scale = (float)(fs / 16.0);
                    /* Pick the font variant matching the box's typography
                     * (sans/serif/mono x bold x italic). */
                    int fslot = box->s->font_slot >= 0 ? box->s->font_slot : display_list_resolve_font_slot(
                        box->s->font_family, box->s->font_weight, box->s->font_italic ? 1 : 0);
                    TextShaper *font = display_list_get_font(fslot);
                    if (!font) font = default_font;
                    int from_cmd = dl->count;
                    /* (vertical text: drawn in its line's own axes from 0, 0, then each
                     * piece turned into the box, lines right to left (vertical-rl,
                     * sideways-rl) or left to right; glyphs kept upright in their place) */
                    bool vert_text = box->s->writing_mode != 0;
                    double box_x0 = box->x, box_y0 = box->y;
                    if (vert_text) { box->x = 0.0; box->y = 0.0; }
                    text_shaper_set_spacing(box->s->letter_spacing, box->s->word_spacing, scale);
                    text_shaper_set_upright((box->s->writing_mode == 1 || box->s->writing_mode == 2) && box->s->text_orient != 2 ? (box->s->text_orient == 1 ? 2 : 1) : 0);
                    text_shaper_set_autospace(box->s->autospace | (box->s->trim_off ? 4 : 0));
                    text_shaper_set_features(box->s->ot_tags, box->s->ot_n);
                    dl_apply_font_palette(box->s);
                    display_list_set_font_chain(box->s->font_family, box->s->font_weight, box->s->font_italic ? 1 : 0);
                    /* The white-space processed text the layout measured. */
                    const char *run_text = css_layout_box_text(ctx, i, css_layout_node_text(ctx, node));
                    /* The fragments its paragraph's layout placed, line by line
                     * (INLINE_LAYOUT_DESIGN.md), an ellipsis after one it cut. */
                    const LayoutTextFragList *tfl =
                        ctx->text_frags && i < ctx->text_frags_n ? &ctx->text_frags[i] : NULL;
                    /* (::first-line's background: the block whose first line it is, its declarations) */
                    float flbg[4] = { 0, 0, 0, 0 };
                    bool flbg_on = false;
                    for (int q = 0; tfl && q < tfl->n && !flbg_on; q++) {
                        if (!tfl->f[q].fl) continue;
                        int fb = ctx->tree.nodes[i].parent_idx;
                        while (fb >= 0 && ctx->boxes[fb].s->display == CSS_DISPLAY_INLINE) fb = ctx->tree.nodes[fb].parent_idx;
                        const CssDeclaration *const *fds = NULL;
                        int fn = fb >= 0 ? css_layout_pseudo_decls(ctx, fb, 4, &fds) : 0;
                        for (int d = 0; d < fn && fds; d++) {
                            if (!fds[d]->property || !fds[d]->value) continue;
                            if (strcasecmp(fds[d]->property, "background-color") && strcasecmp(fds[d]->property, "background")) continue;
                            double rgba[4];
                            if (css_layout_parse_color_text(fds[d]->value, rgba)) for (int c = 0; c < 4; c++) flbg[c] = (float)rgba[c];
                        }
                        flbg_on = flbg[3] > 0.0f;
                        break;
                    }
                    if (tfl && tfl->n > 0) {
                        for (int q = 0; q < tfl->n; q++) {
                            const LayoutTextFrag *fr = &tfl->f[q];
                            /* (text-autospace: the character before the fragment, here or in the text before) */
                            if (box->s->autospace)
                                text_shaper_set_autospace_prev(fr->start > 0 ? css_layout_cp_before(run_text, fr->start) : css_layout_text_prev_cp(ctx, i));
                            if (flbg_on && fr->fl && !vert_text) {
                                float fa = 0.8f, fd = 0.2f, fg = 0.0f;
                                text_shaper_vmetrics(font, &fa, &fd, &fg);
                                display_list_add_rect(dl, (float)(box->x + fr->dx), (float)(box->y + fr->dy), (float)fr->w,
                                                      (float)((fa + fd) * fs), flbg[0], flbg[1], flbg[2], flbg[3] * op);
                            }
                            float fr_r = tr, fr_g = tg, fr_b = tb, fr_a = ta;
                            if (fr->fl) {   /* (on a ::first-line) */
                                fr_r = (float)(fr->fl_rgba >> 24) / 255.0f;
                                fr_g = (float)((fr->fl_rgba >> 16) & 255) / 255.0f;
                                fr_b = (float)((fr->fl_rgba >> 8) & 255) / 255.0f;
                                fr_a = (float)(fr->fl_rgba & 255) / 255.0f * op;
                            }
                            /* (background-clip: text above it: that background's color through the glyphs, under them) */
                            for (int pa = ctx->tree.nodes[i].parent_idx; pa >= 0; pa = ctx->tree.nodes[pa].parent_idx) {
                                const LayoutStyle *ps = ctx->boxes[pa].s;
                                if (ps->bg_clip != 3) continue;
                                if (ps->background_color_a > 0.0f &&
                                    !text_shaper_shape_span(font, run_text, fr->start, fr->end, (float)(box->x + fr->dx), (float)(box->y + fr->dy), scale,
                                                            (fr->level & 1) != 0, (float)ps->background_color_r, (float)ps->background_color_g,
                                                            (float)ps->background_color_b, (float)ps->background_color_a * op, dl))
                                    return false;
                                break;
                            }
                            bool hl_ok = true;
                            /* (text-shadow: beneath, the last first; blurred ones through a blur filter) */
                            for (int sk = (int)box->s->tsh_n - 1; sk >= 0 && !vert_text; sk--) {
                                float sc[4];
                                if (box->s->tsh_cur >> sk & 1) { sc[0] = (float)box->s->color_r; sc[1] = (float)box->s->color_g; sc[2] = (float)box->s->color_b; sc[3] = (float)box->s->color_a; }
                                else {
                                    uint32_t v = box->s->tsh_rgba[sk];
                                    sc[0] = (float)(v >> 24) / 255.0f; sc[1] = (float)((v >> 16) & 255) / 255.0f;
                                    sc[2] = (float)((v >> 8) & 255) / 255.0f; sc[3] = (float)(v & 255) / 255.0f;
                                }
                                sc[3] *= op;
                                if (sc[3] <= 0.0f) continue;
                                float sx = box->s->tsh_x[sk], sy = box->s->tsh_y[sk], sbl = box->s->tsh_b[sk];
                                int smi = -1;
                                DisplayMask sm;
                                if (sbl > 0.0f) {
                                    memset(&sm, 0, sizeof sm);
                                    sm.kind = 9;
                                    sm.image_handle = -1;
                                    float pad = sbl * 2.0f + 2.0f;
                                    sm.x = (float)(box->x + fr->dx) + sx - pad; sm.y = (float)(box->y + fr->dy) + sy - pad;
                                    sm.w = (float)fr->w + 2.0f * pad; sm.h = (float)fs * 1.5f + 2.0f * pad;
                                    sm.nstops = 1;
                                    sm.stop_pos[0] = 9.0f / 32.0f;
                                    sm.rx[0] = sbl / 2.0f;   /* (the radius is twice the deviation) */
                                    smi = dl_add_mask(dl, &sm);
                                    if (smi >= 0 && !dl_add_mask_cmd(dl, DL_MASK_BEGIN, smi, &sm)) smi = -1;
                                }
                                text_shaper_shape_span(font, run_text, fr->start, fr->end, (float)(box->x + fr->dx) + sx,
                                                       (float)(box->y + fr->dy) + sy, scale, (fr->level & 1) != 0, sc[0], sc[1], sc[2], sc[3], dl);
                                if (smi >= 0) dl_add_mask_cmd(dl, DL_MASK_END, smi, &sm);
                            }
                            int sh_from = dl->count;   /* (the fragment's own drawing from here: its shadows' decorations go before it) */
                            float deco_by = 0.0f;
                            int ul_a = 0, ul_b = 0;   /* (the under- and overlines drawn, for skip-ink) */
                            /* (text-decoration-skip-spaces: start end by default: not under
                             * the spaces a line starts or ends with) */
                            float dcx = (float)(box->x + fr->dx), dcw = (float)fr->w;
                            {
                                unsigned char sk = box->s->tds_set ? box->s->tds_bits : 3;
                                if (box->s->white_space != 2 && box->s->white_space != 3) sk = 0;   /* (only spaces kept: pre, pre-wrap) */
                                bool lfirst = q == 0 || fabs(tfl->f[q - 1].dy - fr->dy) > 0.5;
                                bool llast = q + 1 >= tfl->n || fabs(tfl->f[q + 1].dy - fr->dy) > 0.5;
                                char sb[256];
                                if ((sk & 1) && lfirst && fr->end > fr->start) {
                                    size_t a = fr->start;
                                    while (a < fr->end && (run_text[a] == ' ' || run_text[a] == '\t')) a++;
                                    if (a > fr->start && a - fr->start < sizeof sb) {
                                        memcpy(sb, run_text + fr->start, a - fr->start);
                                        sb[a - fr->start] = 0;
                                        float sw = 0.0f, sh = 0.0f;
                                        text_shaper_measure(font, sb, &sw, &sh);
                                        sw *= scale;
                                        if (a >= fr->end) sw = dcw;
                                        dcx += sw; dcw -= sw;
                                    }
                                }
                                if ((sk & 2) && llast && fr->end > fr->start && dcw > 0.0f) {
                                    size_t e = fr->end;
                                    while (e > fr->start && (run_text[e - 1] == ' ' || run_text[e - 1] == '\t')) e--;
                                    if (e < fr->end && fr->end - e < sizeof sb) {
                                        memcpy(sb, run_text + e, fr->end - e);
                                        sb[fr->end - e] = 0;
                                        float sw = 0.0f, sh = 0.0f;
                                        text_shaper_measure(font, sb, &sw, &sh);
                                        dcw -= sw * scale;
                                    }
                                }
                                if (dcw < 0.0f) dcw = 0.0f;
                            }
                            {
                                float a0 = 0.8f, d0 = 0.2f, g0 = 0.0f;
                                text_shaper_vmetrics(font, &a0, &d0, &g0);
                                deco_by = (float)(box->y + fr->dy) + a0 * (float)fs;
                                ul_a = dl->count;
                                unsigned char sk_all = box->s->tds_set && (box->s->tds_bits & 4) && (box->s->white_space == 2 || box->s->white_space == 3);
                                size_t fn = fr->end - fr->start;
                                char *seg = sk_all && fn > 0 && fn < 4096 && memchr(run_text + fr->start, ' ', fn) ? (char *)malloc(fn + 1) : NULL;
                                if (seg) {
                                    /* (text-decoration-skip-spaces: all: under each run between the spaces) */
                                    size_t a = 0;
                                    while (a < fn) {
                                        while (a < fn && (run_text[fr->start + a] == ' ' || run_text[fr->start + a] == '\t')) a++;
                                        size_t b = a;
                                        while (b < fn && run_text[fr->start + b] != ' ' && run_text[fr->start + b] != '\t') b++;
                                        if (b > a) {
                                            float w0 = 0.0f, w1 = 0.0f, hh = 0.0f;
                                            memcpy(seg, run_text + fr->start, b);
                                            seg[a] = 0;
                                            if (a) text_shaper_measure(font, seg, &w0, &hh);
                                            memcpy(seg, run_text + fr->start, b);
                                            seg[b] = 0;
                                            text_shaper_measure(font, seg, &w1, &hh);
                                            dl_text_decorations(dl, ctx, i, q, tfl->n, font, fs, (float)(box->x + fr->dx) + w0 * scale,
                                                                (w1 - w0) * scale, deco_by, op, 0);
                                        }
                                        a = b;
                                    }
                                    free(seg);
                                } else {
                                    dl_text_decorations(dl, ctx, i, q, tfl->n, font, fs, dcx, dcw, deco_by, op, 0);
                                }
                                ul_b = dl->count;
                            }
                            if (box->s->autospace)
                                text_shaper_set_autospace_prev(fr->start > 0 ? css_layout_cp_before(run_text, fr->start) : css_layout_text_prev_cp(ctx, i));
                            if (dl_highlight_frag(dl, ctx, i, node, font, run_text, fr->start, fr->end, (float)(box->x + fr->dx),
                                                  (float)(box->y + fr->dy), scale, (fr->level & 1) != 0 || vert_text, fr_r, fr_g, fr_b,
                                                  fr_a, op, &hl_ok)) {
                                if (!hl_ok) return false;
                            } else if (!text_shaper_shape_span(font, run_text, fr->start, fr->end, (float)(box->x + fr->dx),
                                                        (float)(box->y + fr->dy), scale, (fr->level & 1) != 0, fr_r,
                                                        fr_g, fr_b, fr_a, dl))
                                return false;
                            /* (vertical-lr: the line's over side still to the right; what lies on its side mirrored across its em box's middle) */
                            float lr_mid = 0.0f;
                            if (vert_text && box->s->writing_mode == 2) {
                                float a1 = 0.8f, d1 = 0.2f, g1 = 0.0f;
                                text_shaper_vmetrics(font, &a1, &d1, &g1);
                                /* (about the middle of its line-height box: the content area layout placed, less the part of a pixel it sits off centre) */
                                double adv1 = 0.0, ca1 = css_layout_content_area(ctx, i, &adv1);
                                double off = fr->dy - (adv1 - ca1) * 0.5;
                                off -= floor(off + 0.5 - 1e-6);
                                float mid = (float)(box->y + fr->dy + ca1 * 0.5 - off);
                                lr_mid = mid;
                                for (int k = ul_b; k < dl->count; k++) {
                                    DisplayListCmd *c = &dl->cmds[k];
                                    if (c->type == DL_GLYPH) c->y = 2.0f * mid - c->y - c->h;   /* (upright ones too: centred as in vertical-rl) */
                                }
                            }
                            if (ul_b > ul_a && box->s->tdsi != 1 && !vert_text) dl_skip_ink(dl, ul_a, ul_b, ul_b, dl->count);
                            dl_text_decorations(dl, ctx, i, q, tfl->n, font, fs, dcx, dcw, deco_by, op, 1);
                            /* (the shadows draw the decorations too: each line's copy, offset and in
                             * the shadow's color, under the text's own) */
                            if (box->s->tsh_n > 0 && !vert_text) {
                                int nr = 0;
                                for (int k = sh_from; k < dl->count; k++) nr += dl->cmds[k].type == DL_RECT;
                                int extra = nr * (int)box->s->tsh_n, n0 = dl->count - sh_from;
                                if (nr > 0 && display_list_reserve(dl, extra)) {
                                    memmove(&dl->cmds[sh_from + extra], &dl->cmds[sh_from], (size_t)n0 * sizeof(DisplayListCmd));
                                    int o = sh_from;
                                    for (int sk = (int)box->s->tsh_n - 1; sk >= 0; sk--) {
                                        float sc[4];
                                        if (box->s->tsh_cur >> sk & 1) { sc[0] = (float)box->s->color_r; sc[1] = (float)box->s->color_g; sc[2] = (float)box->s->color_b; sc[3] = (float)box->s->color_a; }
                                        else {
                                            uint32_t v = box->s->tsh_rgba[sk];
                                            sc[0] = (float)(v >> 24) / 255.0f; sc[1] = (float)((v >> 16) & 255) / 255.0f;
                                            sc[2] = (float)((v >> 8) & 255) / 255.0f; sc[3] = (float)(v & 255) / 255.0f;
                                        }
                                        for (int k = sh_from + extra; k < sh_from + extra + n0; k++) {
                                            if (dl->cmds[k].type != DL_RECT) continue;
                                            DisplayListCmd c2 = dl->cmds[k];
                                            c2.x += box->s->tsh_x[sk];
                                            c2.y += box->s->tsh_y[sk];
                                            c2.r = sc[0]; c2.g = sc[1]; c2.b = sc[2]; c2.a = sc[3] * op;
                                            dl->cmds[o++] = c2;
                                        }
                                    }
                                    dl->count += extra;
                                }
                            }
                            /* (a line broken at a soft hyphen: the hyphenate-character at its end) */
                            if (q + 1 < tfl->n && fr->end >= 2 && (unsigned char)run_text[fr->end - 2] == 0xC2 &&
                                (unsigned char)run_text[fr->end - 1] == 0xAD && fabs(tfl->f[q + 1].dy - fr->dy) > 0.5) {
                                const char *hc = box->s->hyph_char ? box->s->hyph_char : (text_shaper_has_glyph(font, 0x2010) ? "\xE2\x80\x90" : "-");
                                size_t hl = strlen(hc);
                                if (hl && !text_shaper_shape_span(font, hc, 0, hl, (float)(box->x + fr->dx + fr->w), (float)(box->y + fr->dy), scale,
                                                                  false, fr_r, fr_g, fr_b, fr_a, dl))
                                    return false;
                            }
                            if (box->s->te_mark[0]) {
                                int em0 = dl->count;
                                dl_emphasis_marks(dl, font, box, run_text, fr->start, fr->end, (float)(box->x + fr->dx),
                                                  (float)(box->y + fr->dy), scale, op, dl_emph_ruby(ctx, i));
                                /* (emphasis marks stay upright in vertical text) */
                                for (int k = em0; k < dl->count; k++) {
                                    if (dl->cmds[k].type != DL_GLYPH) continue;
                                    dl->cmds[k].u.glyph.vo_r = 0;
                                    if (vert_text && box->s->writing_mode == 2) dl->cmds[k].y = 2.0f * lr_mid - dl->cmds[k].y - dl->cmds[k].h;
                                }
                            }
                            if (fr->ellipsis) {
                                /* (in the block's font, color and size, on the text's baseline) */
                                int eb = i;
                                for (int p = ctx->tree.nodes[i].parent_idx; p >= 0; p = ctx->tree.nodes[p].parent_idx) {
                                    eb = p;
                                    if (ctx->boxes[p].s->display != CSS_DISPLAY_INLINE) break;
                                }
                                const LayoutStyle *es = ctx->boxes[eb].s;
                                if (eb != i && es) {
                                    int eslot = es->font_slot >= 0 ? es->font_slot : display_list_resolve_font_slot(es->font_family, es->font_weight, es->font_italic ? 1 : 0);
                                    TextShaper *ef = display_list_get_font(eslot);
                                    if (!ef) ef = default_font;
                                    double efs = es->font_size > 0.0 ? es->font_size : 16.0;
                                    float a0 = 0.8f, d0 = 0.2f, g0 = 0.0f, a1 = 0.8f, d1 = 0.2f, g1 = 0.0f;
                                    text_shaper_vmetrics(font, &a0, &d0, &g0);
                                    text_shaper_vmetrics(ef, &a1, &d1, &g1);
                                    /* (on the block's baseline for the line) */
                                    float ey = fr->ell_abs ? (float)(box->y + fr->ell_top) : (float)(box->y + fr->dy + (double)a0 * fs - (double)a1 * efs + (double)fr->ell_by);
                                    text_shaper_set_spacing(es->letter_spacing, es->word_spacing, (float)(efs / 16.0));
                                    const char *etx = css_layout_block_ellipsis(ctx, i);
                                    /* (right to left: before the text's left edge) */
                                    float ex = (float)(box->x + fr->dx + fr->w) + fr->ell_dx;
                                    if (etx[0] && (es->direction == 1 || (fr->level & 1))) {
                                        float ew = 0.0f, eh = 0.0f;
                                        text_shaper_measure(ef, etx, &ew, &eh);
                                        ex = (float)(box->x + fr->dx) - ew * (float)(efs / 16.0);
                                    }
                                    /* (the block's own text on its ::first-line: that line's color) */
                                    bool efl = fr->fl && ctx->tree.nodes[i].parent_idx == eb;
                                    bool okp = !etx[0] || text_shaper_shape_span(ef, etx, 0, strlen(etx), ex, ey, (float)(efs / 16.0),
                                                                     false, efl ? fr_r : (float)es->color_r, efl ? fr_g : (float)es->color_g,
                                                                     efl ? fr_b : (float)es->color_b, efl ? fr_a : (float)es->color_a * op, dl);
                                    text_shaper_set_spacing(box->s->letter_spacing, box->s->word_spacing, scale);
                                    if (!okp) return false;
                                    continue;
                                }
                            }
                            float ex0 = (float)(box->x + fr->dx + fr->w);
                            if (fr->ellipsis && (box->s->direction == 1 || (fr->level & 1))) {
                                float ew = 0.0f, eh = 0.0f;
                                text_shaper_measure(font, "\xE2\x80\xA6", &ew, &eh);
                                ex0 = (float)(box->x + fr->dx) - ew * scale;
                            }
                            if (fr->ellipsis &&
                                !text_shaper_shape_span(font, "\xE2\x80\xA6", 0, 3, ex0,
                                                        (float)(box->y + fr->dy), scale, false, tr, tg, tb, ta, dl))
                                return false;
                        }
                    } else {
                        /* CSS strut model: center the em box in the line box;
                         * the shaper adds the em ascent on top of this y. */
                        float nlh = text_shaper_normal_line_height(font, (float)fs);
                        if (!(nlh > 0.5f * fs && nlh < 3.0f * fs)) nlh = (float)(fs * 1.5);
                        double lh = box->s->line_height > 0.0 ? box->s->line_height : nlh;
                        double half_leading = (lh - fs) * 0.5;
                        if (!text_shaper_shape_to_display_list_scaled(font,
                                                           run_text,
                                                           (float)box->x,
                                                           (float)(box->y + half_leading),
                                                           scale,
                                                           tr, tg, tb, ta,
                                                           dl)) {
                            return false;
                        }
                    }
                    text_shaper_set_spacing(0.0f, 0.0f, 1.0f);
                    text_shaper_set_upright(0);
                    text_shaper_set_autospace(0);
                    if (!(tfl && tfl->n > 0))
                        emit_text_decorations(dl, from_cmd, fs, box->s->text_decoration, tr, tg, tb, ta);
                    text_shaper_set_features(NULL, 0);
                    if (box->s->font_palette) text_shaper_set_palette(0, 0, NULL, NULL, 0);
                    text_shaper_set_chain(NULL, 0);
                    if (vert_text) {
                        box->x = box_x0;
                        box->y = box_y0;
                        bool rl = box->s->writing_mode == 1 || box->s->writing_mode == 3;
                        /* (sideways-lr: lines run up its block container, mirrored there) */
                        double up0 = 0.0, up1 = 0.0;
                        bool up = box->s->writing_mode == 4;
                        if (up) {
                            int cbk = ctx->tree.nodes[i].parent_idx;
                            while (cbk >= 0 && ctx->boxes[cbk].s->display == CSS_DISPLAY_INLINE) cbk = ctx->tree.nodes[cbk].parent_idx;
                            if (cbk < 0 || ctx->boxes[cbk].s->writing_mode != 4) up = false;
                            else {
                                const LayoutBox *cbb = &ctx->boxes[cbk];
                                up0 = cbb->y + cbb->s->border_top + cbb->padding_top;
                                up1 = cbb->y + cbb->height - cbb->s->border_bottom - cbb->padding_bottom;
                            }
                        }
                        /* (which glyphs lie on their side: all in sideways modes, R ones in mixed, none upright) */
                        int wm_ = box->s->writing_mode, to_ = box->s->text_orient;
                        for (int k = from_cmd; k < dl->count; k++) {
                            DisplayListCmd *cmd = &dl->cmds[k];
                            float gx = cmd->x, gy = cmd->y, gw = cmd->w, gh = cmd->h;
                            if (cmd->type == DL_GLYPH) {
                                bool side = wm_ >= 3 || (to_ == 2) || (to_ == 0 && cmd->u.glyph.vo_r) || cmd->u.glyph.vo_r == 2;
                                if (side) {
                                    float sx = rl ? (float)(box->x + box->width) - (gy + gh) : (float)box->x + gy;
                                    float sy = (float)box->y + gx;
                                    if (up) sy = (float)(up0 + up1) - sy - gw;
                                    cmd->x = sx; cmd->y = sy; cmd->w = gh; cmd->h = gw;
                                    cmd->u.glyph.rot = wm_ == 4 ? 2 : 1;
                                    continue;
                                }
                            }
                            /* (the line's block offset across the box; its inline offset down it) */
                            float cx = rl ? (float)(box->x + box->width) - (gy + gh) : (float)box->x + gy;
                            float cy = (float)box->y + gx;
                            if (up) cy = (float)(up0 + up1) - cy - gw;
                            if (cmd->type == DL_GLYPH || cmd->type == DL_IMAGE) {
                                /* (upright in the turned cell, at its centre) */
                                /* (on whole pixels, as the boxes around it are painted) */
                                cmd->x = floorf(cx + (gh - gw) * 0.5f + 0.01f);
                                cmd->y = floorf(cy + (gw - gh) * 0.5f + 0.01f);
                            } else {
                                cmd->x = cx;
                                cmd->y = cy;
                                cmd->w = gh;
                                cmd->h = gw;
                            }
                            if (cmd->type == DL_BORDER) { cmd->u.border.x = cmd->x; cmd->u.border.y = cmd->y; }
                        }
                        /* (text-shadow, its offsets physical: copies of the turned glyphs
                         * put beneath them, the last shadow first; unblurred) */
                        int ng = 0;
                        for (int k = from_cmd; k < dl->count; k++) ng += dl->cmds[k].type == DL_GLYPH;
                        int extra = ng * (int)box->s->tsh_n, n0 = dl->count - from_cmd;
                        if (extra > 0 && display_list_reserve(dl, extra)) {
                            memmove(&dl->cmds[from_cmd + extra], &dl->cmds[from_cmd], (size_t)n0 * sizeof(DisplayListCmd));
                            int o = from_cmd;
                            for (int sk = (int)box->s->tsh_n - 1; sk >= 0; sk--) {
                                float sc[4];
                                if (box->s->tsh_cur >> sk & 1) { sc[0] = (float)box->s->color_r; sc[1] = (float)box->s->color_g; sc[2] = (float)box->s->color_b; sc[3] = (float)box->s->color_a; }
                                else {
                                    uint32_t v = box->s->tsh_rgba[sk];
                                    sc[0] = (float)(v >> 24) / 255.0f; sc[1] = (float)((v >> 16) & 255) / 255.0f;
                                    sc[2] = (float)((v >> 8) & 255) / 255.0f; sc[3] = (float)(v & 255) / 255.0f;
                                }
                                for (int k = from_cmd + extra; k < from_cmd + extra + n0; k++) {
                                    if (dl->cmds[k].type != DL_GLYPH) continue;
                                    DisplayListCmd c = dl->cmds[k];
                                    c.x += box->s->tsh_x[sk];
                                    c.y += box->s->tsh_y[sk];
                                    c.r = sc[0]; c.g = sc[1]; c.b = sc[2]; c.a = sc[3] * op;
                                    dl->cmds[o++] = c;
                                }
                            }
                            dl->count += extra;
                        }
                    }
                }
            }
        }

        /* display:list-item marker (bullet / ordinal) in the gutter to the
         * left of the item's first line. */
        /* (list-style-image: the image, when it has loaded, outside to the
         * left of the first line) */
        bool image_marker = false;
        if (g_image_cache && node && node->type == HTML_NODE_ELEMENT && box->s->display == CSS_DISPLAY_LIST_ITEM &&
            box->s->list_image && box->s->list_image[0] && !css_layout_marker_none(ctx, i)) {
            int iw = 0, ih = 0;
            if (image_cache_natural_size(g_image_cache, box->s->list_image, &iw, &ih) && iw > 0 && ih > 0) {
                int handle = image_cache_request(g_image_cache, box->s->list_image, iw, ih);
                if (handle >= 0) {
                    double mb = css_layout_marker_baseline(ctx, i);   /* (its bottom on the baseline) */
                    emit_image(dl, (float)(box->x + box->marker_dx - iw - 7.0), (float)(mb - ih), (float)iw, (float)ih, handle);
                    image_marker = true;
                }
            }
        }
        /* (a block or underscore caret in a focused, empty editing host: at its content start) */
        if (node && node->type == HTML_NODE_ELEMENT && box->s->caret_shape >= 2 && node->store_id &&
            (dom_element_state(node->store_id) & DOM_ST_FOCUS) && ctx->tree.nodes[i].first_child_idx < 0) {
            const char *ce = node_attribute_value(node, "contenteditable");
            if (ce && strcasecmp(ce, "false")) {
                double rgba[4] = { box->s->color_r, box->s->color_g, box->s->color_b, box->s->color_a };
                if (box->s->caret_color) css_layout_parse_color_text(box->s->caret_color, rgba);
                double fs = box->s->font_size > 0.0 ? box->s->font_size : 16.0;
                TextShaper *cf = display_list_get_font(box->s->font_slot >= 0 ? box->s->font_slot : 0);
                float za = cf ? text_shaper_zero_advance(cf) : -1.0f;
                double cw = (za > 0.0f ? za : 0.5) * fs;
                double cx = box->x + box->s->border_left + box->padding_left, cy = box->y + box->s->border_top + box->padding_top;
                double ch = box->content_height > 0.0 ? box->content_height : fs;
                if (box->s->direction == 1) cx += box->content_width - cw;
                if (box->s->caret_shape == 3) { double t = fs * 0.1 < 1.0 ? 1.0 : fs * 0.1; cy += ch - t; ch = t; }
                display_list_add_rect(dl, (float)cx, (float)cy, (float)cw, (float)ch, (float)rgba[0], (float)rgba[1], (float)rgba[2], (float)rgba[3] * op);
            }
        }
        if (default_font && node && node->type == HTML_NODE_ELEMENT && !image_marker && !box->s->list_inside &&
            box->s->display == CSS_DISPLAY_LIST_ITEM && !css_layout_box_clips(ctx, i) && (box->s->list_style_type != 0 || (!(node->tag_name && node->tag_name[0] == ':') && css_layout_marker_has_content(ctx, i))) && !css_layout_marker_none(ctx, i)) {   /* (::marker content: shown with list-style-type none too) */
            char marker[512];
            /* (its text from the counter pass: the list-item counter in its counter style; the
             * suffix's trailing space is the gap below) */
            bool m_trail = false;
            const char *mt = css_layout_marker_text(ctx, i);
            if (mt) {
                snprintf(marker, sizeof marker, "%s", mt);
                size_t ml = strlen(marker);
                /* (trailing spaces become the gap below; a <string> or content without them: no gap) */
                for (;;) {   /* (a no-break space too) */
                    if (ml && marker[ml - 1] == ' ') { marker[--ml] = 0; m_trail = true; }
                    else if (ml >= 2 && (unsigned char)marker[ml - 2] == 0xC2 && (unsigned char)marker[ml - 1] == 0xA0) { ml -= 2; marker[ml] = 0; m_trail = true; }
                    else break;
                }
            } else if (box->s->list_style_type == 4) {
                /* decimal: ordinal from preceding <li> element siblings. */
                int ordinal = 1;
                int dom_idx = ctx->tree.nodes[i].dom_node_idx;
                for (int s = po_array_prev_sibling(&ctx->doc->array, dom_idx);
                     s >= 0;
                     s = po_array_prev_sibling(&ctx->doc->array, s)) {
                    HtmlNode *sn = (HtmlNode *)po_array_payload(&ctx->doc->array, s);
                    if (sn && sn->type == HTML_NODE_ELEMENT &&
                        strcasecmp(sn->tag_name, "li") == 0) ordinal++;
                }
                snprintf(marker, sizeof(marker), "%d.", ordinal);
            } else if (box->s->list_style_type == 2) {
                snprintf(marker, sizeof(marker), "\xE2\x97\xA6"); /* U+25E6 white bullet */
            } else if (box->s->list_style_type == 3) {
                snprintf(marker, sizeof(marker), "\xE2\x96\xAA"); /* U+25AA black small square */
            } else {
                snprintf(marker, sizeof(marker), "\xE2\x80\xA2"); /* U+2022 bullet */
            }
            double mfs = box->s->font_size > 0.0 ? box->s->font_size : 16.0;
            /* (the ::marker's font, else the item's) */
            int mwt = box->s->font_weight, mit = box->s->font_italic ? 1 : 0;
            const char *mfam = NULL;
            css_layout_marker_font(ctx, i, &mfs, &mwt, &mit, &mfam);
            float mscale = (float)(mfs / 16.0);
            int mfslot = !mfam && mwt == box->s->font_weight && mit == (box->s->font_italic ? 1 : 0) && box->s->font_slot >= 0
                             ? box->s->font_slot
                             : display_list_resolve_font_slot(mfam ? mfam : box->s->font_family, mwt, mit);
            TextShaper *mfont = display_list_get_font(mfslot);
            if (!mfont) mfont = default_font;
            float mw = 0.0f, mh = 0.0f;
            text_shaper_measure(mfont, marker, &mw, &mh);
            /* (a <string> type: no suffix, so no gap, and free to overflow to the left) */
            bool mstr = ((box->s->list_style_type == 5 && box->s->list_style && (box->s->list_style[0] == '"' || box->s->list_style[0] == '\'')) || css_layout_marker_has_content(ctx, i)) && !m_trail;
            double mx = box->x + box->marker_dx - (double)mw * mscale - (mstr ? 0.0 : mfs * 0.4);
            if (mx < 0.0 && !mstr) mx = 0.0;
            /* (the ::marker's color, else the item's) */
            float mc[4] = { (float)box->s->color_r, (float)box->s->color_g, (float)box->s->color_b, (float)box->s->color_a };
            css_layout_marker_color(ctx, i, mc);
            /* (on the first line's baseline, its own font's ascent above it) */
            float my = (float)box->y;
            {
                double mbl = css_layout_marker_baseline(ctx, i);
                float ma = 0.8f, md = 0.2f, mg = 0.0f;
                text_shaper_vmetrics(mfont, &ma, &md, &mg);
                /* (the marker on that line too: its own font's baseline in the line height, if deeper) */
                double mlh = box->s->line_height > 0.0 ? box->s->line_height : (ma + md + mg) * mfs;
                double ra_ = round(ma * mfs), rd_ = round(md * mfs);   /* (whole pixels, the leading split as the line splits it) */
                double own = box->y + box->s->border_top + box->padding_top + ra_ + floor((mlh - ra_ - rd_) / 2.0);
                if (!isnan(mbl) && own > mbl) mbl = own;
                if (!isnan(mbl) && mbl > box->y - 1.0) my = (float)(mbl - ma * mfs);
            }
            text_shaper_shape_to_display_list_scaled(mfont, marker,
                    (float)mx, my, mscale,
                    mc[0], mc[1], mc[2], mc[3] * op, dl);
        }

        if (box->mc_n > 0 && !box->s->gr_any) paint_column_rules(ctx, i, dl, box, op);
    replaced_border:
        paint_native_control(ctx, dl, i, node, op);
        cbr_from = dl->count;
        /* Borders, painted as four filled edge rects so per-side widths work.
         * The color defaults to currentColor when no border-color was given.
         * Emitted before the small-box filter: a thin box (e.g. <hr>) still
         * needs its border painted. */
        if (!pieces && !hid_cell && (box->s->bi_src || box->s->bi_grad) && emit_border_image(dl, box, op)) {
            /* (a border image instead) */
        } else if (!pieces && !hid_cell && (box->s->border_top > 0.0 || box->s->border_right > 0.0 ||
                        box->s->border_bottom > 0.0 || box->s->border_left > 0.0)) {
            float cl[4], cr[4], ct[4], cb[4];
            border_side_color(box, 0, op, cl);
            border_side_color(box, 1, op, cr);
            border_side_color(box, 2, op, ct);
            border_side_color(box, 3, op, cb);
            const float col[4][4] = { { cl[0], cl[1], cl[2], cl[3] }, { cr[0], cr[1], cr[2], cr[3] },
                                      { ct[0], ct[1], ct[2], ct[3] }, { cb[0], cb[1], cb[2], cb[3] } };
            float bx = (float)box->x, by = (float)box->y + cap_top;
            float bw = (float)box->width, bh = (float)box->height - cap_top - cap_bot;
            snap_rect(&bx, &by, &bw, &bh);
            /* The widths painted (top, right, bottom, left): none for a side
             * styled none or hidden (a collapsed table's own border is room
             * only), and a collapsed table's outer cells paint out past
             * their boxes (border_out). */
            static const int sk[4] = { 2, 1, 3, 0 };
            float bwid[4], out[4];
            const double used[4] = { box->s->border_top, box->s->border_right, box->s->border_bottom, box->s->border_left };
            bool outward = false;
            for (int q = 0; q < 4; q++) {
                bool painted = box->s->border_style[sk[q]] >= CSS_BORDER_SOLID && used[q] > 0.0;
                bwid[q] = painted ? (float)used[q] : 0.0f;
                out[q] = painted ? box->s->border_out[sk[q]] : 0.0f;
                if (out[q] > 0.0f) { bwid[q] += out[q]; outward = true; }
            }
            if (outward) {
                bx -= out[3];
                by -= out[0];
                bw += out[1] + out[3];
                bh += out[0] + out[2];
            }
            DisplayMask ring;
            if (bwid[0] <= 0.0f && bwid[1] <= 0.0f && bwid[2] <= 0.0f && bwid[3] <= 0.0f) {
                /* (nothing painted) */
            } else if (box_round_mask(box, &ring)) {
                /* Rounded: the border is the ring between the outer and the
                 * inset rounded rectangles. */
                ring.kind = 4;
                ring.border[0] = (float)box->s->border_top;
                ring.border[1] = (float)box->s->border_right;
                ring.border[2] = (float)box->s->border_bottom;
                ring.border[3] = (float)box->s->border_left;
                int mi = dl_add_mask(dl, &ring);
                if (mi >= 0 && dl_add_mask_cmd(dl, DL_MASK_BEGIN, mi, &ring)) {
                    /* (painted sides alike: one fill; else each side its own, inside the ring) */
                    static const int cq[4] = { 2, 1, 3, 0 };
                    bool alike = true;
                    int first = -1;
                    for (int q = 0; q < 4; q++) {
                        if (bwid[q] <= 0.0f) continue;
                        if (first < 0) first = q;
                        else if (memcmp(col[cq[q]], col[cq[first]], sizeof col[0])) alike = false;
                    }
                    if (alike && first >= 0) display_list_add_rect(dl, bx, by, bw, bh, col[cq[first]][0], col[cq[first]][1], col[cq[first]][2], col[cq[first]][3]);
                    else if (first >= 0) paint_border_sides(dl, box, bx, by, bw, bh, col, bwid);
                    dl_add_mask_cmd(dl, DL_MASK_END, mi, &ring);
                }
            } else if (!border_uniform(box, col, bwid)) {
                paint_border_sides(dl, box, bx, by, bw, bh, col, bwid);
            } else {
            if (bwid[0] > 0.0f) display_list_add_rect(dl, bx, by, bw, bwid[0], ct[0], ct[1], ct[2], ct[3]);
            if (bwid[2] > 0.0f) display_list_add_rect(dl, bx, by + bh - bwid[2], bw, bwid[2], cb[0], cb[1], cb[2], cb[3]);
            float mid_h = bh - bwid[0] - bwid[2];
            if (mid_h < 0.0f) mid_h = 0.0f;
            if (bwid[3] > 0.0f) display_list_add_rect(dl, bx, by + bwid[0], bwid[3], mid_h, cl[0], cl[1], cl[2], cl[3]);
            if (bwid[1] > 0.0f)
                display_list_add_rect(dl, bx + bw - bwid[1], by + bwid[0], bwid[1], mid_h, cr[0], cr[1], cr[2], cr[3]);
            }
        }

        if (box->s->table_role == 4 && box->s->border_collapse && cbr_from >= 0 && dl->count > cbr_from && ncb < CBMAX) {
            int tb = ctx->tree.nodes[i].parent_idx;
            while (tb >= 0 && ctx->boxes[tb].s->table_role != 1) tb = ctx->tree.nodes[tb].parent_idx;
            if (tb >= 0) { cbt[ncb] = tb; cba[ncb] = cbr_from; cbb[ncb] = dl->count; ncb++; }
        }
        /* (the outline: a border outside the border box, as far out as its offset) */
        if (box->s->outline_style >= CSS_BORDER_SOLID && box->s->display != CSS_DISPLAY_NONE &&
            box->s->visibility == CSS_VISIBILITY_VISIBLE && box->s->table_role != 6) {
            float ow = box->s->outline_wset ? box->s->outline_w : 3.0f * (box->s->zoom > 0.0f ? box->s->zoom : 1.0f), oo = box->s->outline_off;
            if (box->s->outline_inh & 16) oo = -ow;   /* (outline-offset: inset) */
            if (ow >= 1.0f) ow = floorf(ow);
            /* (the rectangle it goes around: at least empty, centred, when the offset eats the box) */
            float iw = (float)box->width + 2.0f * oo, ih = (float)box->height + 2.0f * oo;
            float ix = (float)box->x - oo, iy = (float)box->y - oo;
            if (iw < 0.0f) { ix += iw * 0.5f; iw = 0.0f; }
            if (ih < 0.0f) { iy += ih * 0.5f; ih = 0.0f; }
            float ox = ix - ow, oy = iy - ow;
            float oww = iw + 2.0f * ow, ohh = ih + 2.0f * ow;
            if (ow > 0.0f && oww > 0.0f && ohh > 0.0f) {
                float oc[4];
                if (box->s->outline_cset) {
                    uint32_t v = box->s->outline_rgba;
                    oc[0] = (float)(v >> 24) / 255.0f; oc[1] = (float)((v >> 16) & 255) / 255.0f;
                    oc[2] = (float)((v >> 8) & 255) / 255.0f; oc[3] = (float)(v & 255) / 255.0f * op;
                } else {
                    oc[0] = (float)box->s->color_r; oc[1] = (float)box->s->color_g; oc[2] = (float)box->s->color_b; oc[3] = (float)box->s->color_a * op;
                }
                LayoutStyle os = *box->s;
                LayoutBox ob = *box;
                ob.s = &os;
                for (int k = 0; k < 4; k++) os.border_style[k] = box->s->outline_style;
                const float ocol[4][4] = { { oc[0], oc[1], oc[2], oc[3] }, { oc[0], oc[1], oc[2], oc[3] },
                                           { oc[0], oc[1], oc[2], oc[3] }, { oc[0], oc[1], oc[2], oc[3] } };
                const float obw[4] = { ow, ow, ow, ow };
                snap_rect(&ox, &oy, &oww, &ohh);
                if (box->s->outline_style == CSS_BORDER_SOLID) {
                    display_list_add_rect(dl, ox, oy, oww, ow, oc[0], oc[1], oc[2], oc[3]);
                    display_list_add_rect(dl, ox, oy + ohh - ow, oww, ow, oc[0], oc[1], oc[2], oc[3]);
                    display_list_add_rect(dl, ox, oy + ow, ow, ohh - 2.0f * ow, oc[0], oc[1], oc[2], oc[3]);
                    display_list_add_rect(dl, ox + oww - ow, oy + ow, ow, ohh - 2.0f * ow, oc[0], oc[1], oc[2], oc[3]);
                } else {
                    paint_border_sides(dl, &ob, ox, oy, oww, ohh, ocol, obw);
                }
            }
        }
        /* (gap decorations: just above the border) */
        if (box->s->gr_any) {
            int gfrom = dl->count;
            if (box->mc_n > 0) paint_column_rules(ctx, i, dl, box, op);   /* (with insets and the rest: over its border as a grid's) */
            paint_gap_rules(ctx, dl, box, i, op);
            /* (inside its own overflow clip: its padding box) */
            if (box->s->overflow != 0) {
                float pc[4] = { (float)(box->x + box->s->border_left), (float)(box->y + box->s->border_top),
                                (float)(box->x + box->width - box->s->border_right), (float)(box->y + box->height - box->s->border_bottom) };
                dl_clip_range(dl, gfrom, pc);
            }
        }
        if (box->width < 2.0f || box->height < 2.0f) continue;
        if (box->width * box->height < 6.0f) continue;

        /* Placeholder fills for content shells injected without visible CSS in
         * our engine.  This makes skeleton thumbnails, avatars, and text shells
         * visible instead of transparent.  Colors are chosen to contrast with
         * dark backgrounds.  Only applies when the cascade did not provide a
         * background color — otherwise it would paint over the real (light)
         * skeleton color from the page's CSS.
         * YouTube-only: the class needles are YouTube skeleton classes, and
         * substring matching would otherwise misfire on classes like
         * MediaWiki's "vector-search-box-show-thumbnail". */
        /* Retired: the layout engine now renders YouTube's real content
         * (CYBER_YT_PLACEHOLDERS=1 re-enables the skeleton fill). */
        static int placeholders_env = -1;
        if (placeholders_env < 0) placeholders_env = getenv("CYBER_YT_PLACEHOLDERS") ? 1 : 0;
        bool placeholders_enabled = placeholders_env && ctx->base_url &&
                                    strstr(ctx->base_url, "youtube.") != NULL;
        if (placeholders_enabled && !box->s->background_image_url[0] && box->s->background_color_a <= 0.0f) {
            static const char *thumbnail_needles[] = {
                "rich-thumbnail", "video-thumbnail", "thumbnail", NULL
            };
            static const char *avatar_needles[] = {
                "channel-avatar", "avatar", NULL
            };
            static const char *title_needles[] = {
                "rich-video-title", "video-title", "text-shell", NULL
            };
            static const char *meta_needles[] = {
                "rich-video-meta", "video-meta", NULL
            };
            static const char *details_needles[] = {
                "details-text-shell", NULL
            };
            static const char *skeleton_needles[] = {
                "skeleton-bg-color", "video-skeleton",
                "skeleton-light-border-bottom", NULL
            };
            static const char *chip_needles[] = {
                "home-chips", "home-chips-ghost", NULL
            };
            static const char *guide_needles[] = {
                "guide-skeleton", "guide-ghost", "guide-ghost-icon",
                "guide-ghost-text", NULL
            };
            float pr = 0.0f, pg = 0.0f, pb = 0.0f, pa = 0.0f;
            if (node_class_contains_any(node, thumbnail_needles)) {
                pr = 0.25f; pg = 0.25f; pb = 0.32f; pa = 1.0f; /* slate thumbnail */
            } else if (node_class_contains_any(node, avatar_needles)) {
                pr = 0.50f; pg = 0.50f; pb = 0.50f; pa = 1.0f; /* #808080 */
            } else if (node_class_contains_any(node, title_needles)) {
                pr = 0.35f; pg = 0.35f; pb = 0.35f; pa = 1.0f; /* #595959 */
            } else if (node_class_contains_any(node, meta_needles)) {
                pr = 0.28f; pg = 0.28f; pb = 0.28f; pa = 1.0f; /* #474747 */
            } else if (node_class_contains_any(node, details_needles)) {
                pr = 0.22f; pg = 0.22f; pb = 0.22f; pa = 1.0f; /* #383838 */
            } else if (node_class_contains_any(node, skeleton_needles)) {
                pr = 0.40f; pg = 0.40f; pb = 0.40f; pa = 1.0f; /* #666666 */
            } else if (node_class_contains_any(node, chip_needles)) {
                pr = 0.45f; pg = 0.45f; pb = 0.45f; pa = 1.0f; /* chip pill */
            } else if (node_class_contains_any(node, guide_needles)) {
                pr = 0.18f; pg = 0.18f; pb = 0.18f; pa = 1.0f; /* guide sidebar */
            }
            if (pa > 0.0f) {
                if (!display_list_add_rect(dl,
                                           (float)box->x, (float)box->y,
                                           (float)box->width, (float)box->height,
                                           pr, pg, pb, pa)) {
                    return false;
                }
            }
        }

    }
    if (pending_box >= 0 && pending_hidden) { dl->count = pending_from; dl->mask_count = pending_mask; }
    if (pending_box >= 0 && off) dl_scroll_range(dl, pending_from, pending_mask, off[pending_box]);
    if (pending_box >= 0 && clip) dl_clip_range(dl, pending_from, clip[pending_box]);
    if (pending_box >= 0 && any_mc) {
        dl_bdb_clone(ctx, dl, pending_from, pending_box, 1.0f);
        if (!mc_hold) {
            int ml = -1, mch0 = -1;
            for (int q = 0; q < nmask && ml < 0; q++) if (mask_start[q] == pending_box) ml = q;
            if (ml >= 0 && mc_owner(ctx, pending_box, &mch0) >= 0) {
                mc_hold = true;
                hold_from = pending_from;
                hold_box = pending_box;
                hold_level = ml;
                memcpy(hold_xf, range_xf, sizeof hold_xf);
            }
        }
        if (!(mc_hold && pending_box == hold_box)) dl_mc_chain(ctx, dl, pending_from, pending_box, mc_hold ? hold_box : -1, range_xf);
    }
    if (ncb) dl_cb_flush(ctx, dl, cbt, cba, cbb, &ncb, -1);
    while (nmask > 0) {
        nmask--;
        if (mask_idx[nmask] <= -1000) { dl_ref_filter_end(ctx, dl, mask_start[nmask], -1000 - mask_idx[nmask]); }
        else dl_add_mask_cmd(dl, DL_MASK_END, mask_idx[nmask], &dl->masks[mask_idx[nmask]]);
        if (mc_hold && nmask <= hold_level) {
            mc_hold = false;
            dl_mc_chain(ctx, dl, hold_from, hold_box, -1, hold_xf);
        }
    }
    /* (back to none, for what is drawn after) */
    if (any_tf && (cur_xf[0] != 1.0f || cur_xf[1] != 0.0f || cur_xf[2] != 0.0f || cur_xf[3] != 1.0f ||
                   cur_xf[4] != 0.0f || cur_xf[5] != 0.0f) && display_list_reserve(dl, 1)) {
        DisplayListCmd *xc = &dl->cmds[dl->count++];
        memset(xc, 0, sizeof *xc);
        xc->type = DL_XFORM;
        xc->u.xf[0] = xc->u.xf[3] = 1.0f;
    }

    return true;
}

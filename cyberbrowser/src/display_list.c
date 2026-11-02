/*
 * Display List - Implementation
 */

#include "display_list.h"
#include "platform.h"
#include "url_utils.h"
#include "text_shaper.h"
#include "font_registry.h"
#include "url_whatwg.h"
#include "image_cache.h"
#include "svg_render.h"
#include "browser_api_impl_types.h"
#include "browser/webgpu_bridge.h"

/* The frame a <video> shows now (browser/media_source_api.c). */
bool media_video_frame_for_node(uint32_t node, const uint8_t **rgba, int *w, int *h,
                                uint64_t *frame_id);

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <math.h>

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

bool display_list_add_rect(DisplayList *dl, float x, float y, float w, float h,
                           float r, float g, float b, float a)
{
    if (!display_list_reserve(dl, 1)) return false;
    DisplayListCmd *cmd = &dl->cmds[dl->count++];
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = DL_RECT;
    cmd->x = x; cmd->y = y; cmd->w = w; cmd->h = h;
    cmd->r = r; cmd->g = g; cmd->b = b; cmd->a = a;
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
    int nw = 0, nh = 0;
    if (!image_cache_natural_size(g_image_cache, s->background_image_url, &nw, &nh) || nw <= 0 || nh <= 0) {
        image_cache_request(g_image_cache, s->background_image_url, 0, 0);
        return;
    }
    double bx = box->x, by = box->y, bw = box->width, bh = box->height;
    /* positioning area: the padding box (the viewport when fixed) */
    double ax = bx + s->border_left, ay = by + s->border_top;
    double aw = bw - s->border_left - s->border_right, ah = bh - s->border_top - s->border_bottom;
    if (s->bg_fixed) { ax = 0.0; ay = 0.0; aw = ctx->viewport_width; ah = ctx->viewport_height; }
    if (aw <= 0.0 || ah <= 0.0) return;
    /* (an image without a size or ratio of its own, an SVG: CSS
     * Backgrounds 3 section 3.9, the area's for what it lacks) */
    unsigned dims = 7;
    float ratio = 0.0f;
    image_cache_intrinsic(g_image_cache, s->background_image_url, &dims, &ratio);
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
    int handle = image_cache_request(g_image_cache, s->background_image_url, (int)ceil(tw), (int)ceil(th));
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
    if (!s->bg_no_repeat_x) {
        x0 = ox - ceil((ox - bx) / tw) * tw;
        x1 = bx + bw;
    }
    if (!s->bg_no_repeat_y) {
        y0 = oy - ceil((oy - by) / th) * th;
        y1 = by + bh;
    }
    double ntiles = ceil((x1 - x0) / tw) * ceil((y1 - y0) / th);
    if (ntiles > 20000.0) return;   /* (a tiny image over a huge box) */
    int from = dl->count;
    for (double y = y0; y < y1 - 0.001; y += th)
        for (double x = x0; x < x1 - 0.001; x += tw)
            if (x + tw > c[0] && x < c[2] && y + th > c[1] && y < c[3])
                emit_image(dl, (float)x, (float)y, (float)tw, (float)th, handle);
    dl_clip_range(dl, from, c);
}

static void on_async_image_loaded(const char *url, void *user_data);

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

static bool node_has_class(HtmlNode *node, const char *needle)
{
    if (!node || node->type != HTML_NODE_ELEMENT || !needle) return false;
    size_t needle_len = strlen(needle);
    for (HtmlAttribute *a = node->attributes; a; a = a->next) {
        if (strcasecmp(a->name, "class") == 0 && a->value[0]) {
            const char *p = a->value;
            size_t len = strlen(p);
            for (size_t i = 0; i < len; ) {
                while (i < len && isspace((unsigned char)p[i])) i++;
                size_t start = i;
                while (i < len && !isspace((unsigned char)p[i])) i++;
                if (i - start == needle_len && strncasecmp(p + start, needle, needle_len) == 0) return true;
            }
        }
    }
    return false;
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

static bool node_or_ancestor_has_class(LayoutContext *ctx, int node_idx,
                                        const char *needle)
{
    if (node_idx < 0 || node_idx >= ctx->tree.count) return false;
    int idx = node_idx;
    while (idx >= 0) {
        HtmlNode *node = css_layout_box_node(ctx, idx);
        if (node_has_class(node, needle)) return true;
        idx = ctx->tree.nodes[idx].parent_idx;
    }
    return false;
}

static bool node_has_hidden_class(LayoutContext *ctx, int node_idx)
{
    /* Treat any node inside a hidden subtree as hidden. Some page skeletons
     * carry class "hidden" on their root while descendants do not, so we must
     * walk up the layout tree. */
    if (node_or_ancestor_has_class(ctx, node_idx, "hidden")) return true;
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
    *x = x0;
    *y = y0;
    *w = x1 - x0;
    *h = y1 - y0;
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
        c[3] = (float)(v & 0xFF) / 255.0f * op;
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
            bool solid = st == CSS_BORDER_SOLID && box->s->border_style[style_k[n]] == CSS_BORDER_SOLID;
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

static void paint_pieces(LayoutContext *ctx, DisplayList *dl, int el, int anchor, float op)
{
    const LayoutBox *box = &ctx->boxes[el];
    const LayoutPieceList *pieces = &ctx->pieces[el];
    float cl[4], cr[4], ct[4], cb[4];
    border_side_color(box, 0, op, cl);
    border_side_color(box, 1, op, cr);
    border_side_color(box, 2, op, ct);
    border_side_color(box, 3, op, cb);
    for (int q = 0; q < pieces->n; q++) {
        const LayoutPiece *pc = &pieces->p[q];
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

static bool box_or_ancestor_hidden(LayoutContext *ctx, int node_idx)
{
    int idx = node_idx;
    while (idx >= 0) {
        LayoutBox *b = &ctx->boxes[idx];
        if (b->s->display == CSS_DISPLAY_NONE || b->s->visibility == CSS_VISIBILITY_HIDDEN) return true;
        idx = ctx->tree.nodes[idx].parent_idx;
    }
    return false;
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

    float dx = cx, dy = cy, dw = cw, dh = ch;
    float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
    int fit = box->s->object_fit;
    if (fit == 4) {   /* scale-down: none or contain, whichever is smaller */
        fit = ((float)nw <= cw && (float)nh <= ch) ? 3 : 1;
    }
    if (fit == 1 || fit == 3) {
        float s = fit == 1 ? fminf(cw / (float)nw, ch / (float)nh) : 1.0f;
        dw = (float)nw * s;
        dh = (float)nh * s;
        dx = cx + (cw - dw) * 0.5f;     /* object-position: 50% 50% */
        dy = cy + (ch - dh) * 0.5f;
        if (fit == 3 && (dw > cw || dh > ch)) {
            /* none, larger than the box: crop to the box. */
            float vx0 = fmaxf(dx, cx), vy0 = fmaxf(dy, cy);
            float vx1 = fminf(dx + dw, cx + cw), vy1 = fminf(dy + dh, cy + ch);
            u0 = (vx0 - dx) / dw; u1 = (vx1 - dx) / dw;
            v0 = (vy0 - dy) / dh; v1 = (vy1 - dy) / dh;
            dx = vx0; dy = vy0; dw = vx1 - vx0; dh = vy1 - vy0;
        }
    } else if (fit == 2) {
        /* cover: scale to fill, crop the overflow symmetrically. */
        float s = fmaxf(cw / (float)nw, ch / (float)nh);
        float sw = (float)nw * s, sh = (float)nh * s;
        u0 = ((sw - cw) * 0.5f) / sw;
        u1 = 1.0f - u0;
        v0 = ((sh - ch) * 0.5f) / sh;
        v1 = 1.0f - v0;
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
    sb_puts(b, "<");
    sb_puts(b, tag);
    bool has_xmlns = false;
    for (HtmlAttribute *a = n->attributes; a; a = a->next) {
        if (!a->name) continue;
        if (root && (strcasecmp(a->name, "width") == 0 || strcasecmp(a->name, "height") == 0 ||
                     strcasecmp(a->name, "style") == 0)) continue;
        if (strcasecmp(a->name, "xmlns") == 0) has_xmlns = true;
        sb_puts(b, " ");
        sb_puts(b, svg_case(a->name));
        sb_puts(b, "=\"");
        if (a->value) sb_escaped(b, a->value, strlen(a->value));
        sb_puts(b, "\"");
    }
    if (root) {
        char t[64];
        if (!has_xmlns) sb_puts(b, " xmlns=\"http://www.w3.org/2000/svg\"");
        snprintf(t, sizeof(t), " width=\"%d\" height=\"%d\"", w, h);
        sb_puts(b, t);
        if (root_style && root_style[0]) {
            sb_puts(b, " style=\"");
            sb_escaped(b, root_style, strlen(root_style));
            sb_puts(b, "\"");
        }
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
    svg_serialize(&ctx->doc->array, dom_idx, &b, true, cw, ch, style);
    if (!b.s) return;
    char key[96];
    snprintf(key, sizeof(key), "cyber-svg:%016llx:%08x", (unsigned long long)svg_hash(b.s, b.n), color);
    if (image_cache_status(g_image_cache, key) != 1) {
        uint8_t *px = NULL;
        int pw = 0, ph = 0;
        if (svg_render_rgba(b.s, b.n, cw, ch, color, &px, &pw, &ph)) {
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

static bool inside_svg(PreOrderCompactionArray *arr, int dom_idx)
{
    for (int p = po_array_parent(arr, dom_idx); p >= 0; p = po_array_parent(arr, p)) {
        HtmlNode *n = (HtmlNode *)po_array_payload(arr, p);
        if (n && n->type == HTML_NODE_ELEMENT && n->tag_name && strcasecmp(n->tag_name, "svg") == 0) return true;
    }
    return false;
}

char *display_list_image_source(const char *base_url, HtmlNode *node, float css_width)
{
    if (!node || node->type != HTML_NODE_ELEMENT) return NULL;
    const char *src = node_attribute_value(node, "src");
    /* srcset, when present, overrides src with the candidate that best
     * matches the laid-out width (or the 1x candidate). */
    const char *srcset = node_attribute_value(node, "srcset");
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
static void dl_clip_range(DisplayList *dl, int from, const float c[4])
{
    for (int k = from; k < dl->count; k++) {
        DisplayListCmd *cmd = &dl->cmds[k];
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
        if (u0 && cmd->w > 0.0f && cmd->h > 0.0f) {
            float du = *u1 - *u0, dv = *v1 - *v0;
            float nu0 = *u0 + du * (nx0 - x0) / cmd->w, nu1 = *u0 + du * (nx1 - x0) / cmd->w;
            float nv0 = *v0 + dv * (ny0 - y0) / cmd->h, nv1 = *v0 + dv * (ny1 - y0) / cmd->h;
            *u0 = nu0; *u1 = nu1; *v0 = nv0; *v1 = nv1;
        }
        cmd->x = nx0; cmd->y = ny0; cmd->w = nx1 - nx0; cmd->h = ny1 - ny0;
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
    m->repeat = !box->s->mask_no_repeat;
    /* Size: auto = natural size (the box until the image is known). */
    float iw = m->w, ih = m->h;
    int nw = 0, nh = 0;
    int probe = image_cache_request(g_image_cache, box->s->mask_image, 0, 0);
    bool natural = probe >= 0 && image_cache_get_natural(g_image_cache, probe, &nw, &nh) && nw > 0 && nh > 0;
    if (box->s->mask_size_kind == 0 && natural) { iw = (float)nw; ih = (float)nh; }
    else if ((box->s->mask_size_kind == 1 || box->s->mask_size_kind == 2) && natural) {
        float sx = m->w / (float)nw, sy = m->h / (float)nh;
        float s = box->s->mask_size_kind == 1 ? (sx < sy ? sx : sy) : (sx > sy ? sx : sy);
        iw = (float)nw * s; ih = (float)nh * s;
    } else if (box->s->mask_size_kind == 3) {
        iw = (float)((box->s->mask_size_pct & 1) ? box->s->mask_size_w * m->w : box->s->mask_size_w);
        if (box->s->mask_size_h >= 0.0)
            ih = (float)((box->s->mask_size_pct & 2) ? box->s->mask_size_h * m->h : box->s->mask_size_h);
        else
            ih = natural ? iw * (float)nh / (float)nw : iw;
    }
    if (iw < 1.0f || ih < 1.0f) return false;
    m->iw = iw; m->ih = ih;
    m->ix = m->x + (float)((box->s->mask_pos_pct & 1) ? box->s->mask_pos_x * (m->w - iw) : box->s->mask_pos_x);
    m->iy = m->y + (float)((box->s->mask_pos_pct & 2) ? box->s->mask_pos_y * (m->h - ih) : box->s->mask_pos_y);
    m->image_handle = image_cache_request(g_image_cache, box->s->mask_image, (int)ceilf(iw), (int)ceilf(ih));
    return m->image_handle >= 0;
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
        m->ry[c] = pct ? box->s->radius[c] * m->h : box->s->radius[c];
    }
    float f = 1.0f;
    float sums[4] = { m->rx[0] + m->rx[1], m->ry[1] + m->ry[2], m->rx[2] + m->rx[3], m->ry[3] + m->ry[0] };
    float sides[4] = { m->w, m->h, m->w, m->h };
    for (int k = 0; k < 4; k++) if (sums[k] > sides[k] && sums[k] > 0.0f && sides[k] / sums[k] < f) f = sides[k] / sums[k];
    for (int c = 0; c < 4; c++) { m->rx[c] *= f; m->ry[c] *= f; }
    return true;
}

static bool is_replaced_tag(const HtmlNode *n)
{
    if (!n || n->type != HTML_NODE_ELEMENT || !n->tag_name) return false;
    return strcasecmp(n->tag_name, "img") == 0 || strcasecmp(n->tag_name, "video") == 0 ||
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

static bool build_display_list_impl(LayoutContext *ctx, DisplayList *dl, float (*clip)[4]);

bool css_layout_build_display_list(LayoutContext *ctx, DisplayList *dl)
{
    if (!ctx || !dl) return false;
    display_list_init(dl);
    /* overflow != visible clips descendants to the padding box. */
    int n = ctx->tree.count;
    float (*clip)[4] = (float (*)[4])malloc(((size_t)n + 1) * sizeof(*clip));
    if (clip) {
        for (int i = 0; i < n; i++) {
            int p = ctx->tree.nodes[i].parent_idx;
            LayoutBox *b = &ctx->boxes[i];
            /* (an absolutely positioned box: clipped as its containing
             * block's content, not by the boxes between) */
            if (b->s->position == CSS_POSITION_ABSOLUTE)
                while (p >= 0 && ctx->tree.nodes[p].parent_idx >= 0 && ctx->boxes[p].s->position == CSS_POSITION_STATIC)
                    p = ctx->tree.nodes[p].parent_idx;
            float c[4] = { -1e30f, -1e30f, 1e30f, 1e30f };
            if (p >= 0 && p < i && b->s->position != CSS_POSITION_FIXED) {
                memcpy(c, clip[p], sizeof(c));
                LayoutBox *pb = &ctx->boxes[p];
                /* overflow on <html>/<body> applies to the viewport. */
                HtmlNode *pn = ctx->doc ? css_layout_box_node(ctx, p) : NULL;
                bool viewport_box = pn && pn->type == HTML_NODE_ELEMENT && pn->tag_name &&
                    (strcasecmp(pn->tag_name, "html") == 0 || strcasecmp(pn->tag_name, "body") == 0);
                if (pb->s->overflow != 0 && pb->s->display != CSS_DISPLAY_NONE && !viewport_box) {
                    if (getenv("CYBER_DEBUG_CLIP") && ctx->tree.nodes[i].prev_sibling_idx < 0)
                        fprintf(stderr, "[CLIP] box %d by parent %d: %.0f,%.0f %.0fx%.0f\n", i, p,
                                pb->x, pb->y, pb->width, pb->height);
                    float px0 = (float)(pb->x + pb->s->border_left), py0 = (float)(pb->y + pb->s->border_top);
                    float px1 = (float)(pb->x + pb->width - pb->s->border_right);
                    float py1 = (float)(pb->y + pb->height - pb->s->border_bottom);
                    if (pb->s->table_role == 1) {
                        /* (a table's box, without its captions; they are
                         * outside it, not clipped by it) */
                        float cbot = 0.0f, ctop = table_caption_top(ctx, p, pb, &cbot);
                        py0 += ctop;
                        py1 -= cbot;
                        if (b->s->table_caption || b->s->table_role == 5) { px0 = c[0]; py0 = c[1]; px1 = c[2]; py1 = c[3]; }
                    }
                    if (px0 > c[0]) c[0] = px0;
                    if (py0 > c[1]) c[1] = py0;
                    if (px1 < c[2]) c[2] = px1;
                    if (py1 < c[3]) c[3] = py1;
                }
            }
            memcpy(clip[i], c, sizeof(c));
        }
    }
    bool ok = build_display_list_impl(ctx, dl, clip);
    free(clip);
    return ok;
}

/* The canvas's background (CSS Backgrounds §2.11.2): the root element's
 * color, else (an HTML root without one) the body's, over the whole canvas,
 * the viewport and all the document's boxes.  Returns the box it came from
 * (its own background is not painted again), -1 for none, -2 on failure. */
static int canvas_background(LayoutContext *ctx, DisplayList *dl)
{
    int root = ctx->tree.root_idx;
    if (root < 0 || root >= ctx->tree.count || !ctx->doc) return -1;
    int src = -1;
    if (ctx->boxes[root].s->background_color_a > 0.0f || ctx->boxes[root].s->background_image_url[0]) {
        src = root;
    } else {
        HtmlNode *rn = css_layout_box_node(ctx, root);
        if (!rn || rn->type != HTML_NODE_ELEMENT || !rn->tag_name || strcasecmp(rn->tag_name, "html") != 0) return -1;
        for (int c = ctx->tree.nodes[root].first_child_idx; c >= 0; c = ctx->tree.nodes[c].next_sibling_idx) {
            HtmlNode *n = css_layout_box_node(ctx, c);
            if (!n || n->type != HTML_NODE_ELEMENT || !n->tag_name || strcasecmp(n->tag_name, "body") != 0) continue;
            if ((ctx->boxes[c].s->background_color_a > 0.0f || ctx->boxes[c].s->background_image_url[0]) &&
                ctx->boxes[c].s->display != CSS_DISPLAY_NONE)
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
    if (s->background_color_a > 0.0f &&
        !display_list_add_rect(dl, 0.0f, 0.0f, (float)ceil(w), (float)ceil(h), (float)s->background_color_r,
                               (float)s->background_color_g, (float)s->background_color_b,
                               (float)s->background_color_a * (float)ctx->boxes[src].opacity_eff))
        return -2;
    if (g_image_cache && s->background_image_url[0]) {
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
 * with z-index auto paints like a stacking context of its own (its positioned
 * descendants with it: an approximation). */
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

static bool paint_is_context(const LayoutContext *ctx, int i)
{
    const LayoutBox *b = &ctx->boxes[i];
    if (b->s->position == CSS_POSITION_FIXED || b->s->position == CSS_POSITION_STICKY) return true;
    if (b->s->z_index_set) {
        if (b->s->position != CSS_POSITION_STATIC) return true;
        int p = ctx->tree.nodes[i].parent_idx;   /* (a flex or grid item) */
        if (p >= 0 && (ctx->boxes[p].s->display == CSS_DISPLAY_FLEX || ctx->boxes[p].s->display == CSS_DISPLAY_GRID))
            return true;
    }
    return b->s->opacity_set && b->s->opacity < 1.0;
}

typedef struct { PaintVec negs, blocks, floats, inlines, zeros, pos; } PaintLayers;

/* A stacking context's descendants into its layers (an atomic inline in
 * inlines as -(index + 2)). */
static void paint_walk(const LayoutContext *ctx, int n, PaintLayers *L)
{
    for (int c = ctx->tree.nodes[n].first_child_idx; c >= 0; c = ctx->tree.nodes[c].next_sibling_idx) {
        const LayoutBox *b = &ctx->boxes[c];
        if (b->s->display == CSS_DISPLAY_NONE) continue;
        bool sc = paint_is_context(ctx, c);
        if (b->s->position != CSS_POSITION_STATIC || sc) {
            int z = b->s->z_index_set ? b->s->z_index : 0;
            if (z < 0 && sc) pv_push(&L->negs, c);
            else if (z > 0 && sc) pv_push(&L->pos, c);
            else pv_push(&L->zeros, c);
            continue;
        }
        if (b->s->float_side != 0) { pv_push(&L->floats, c); continue; }
        HtmlNode *dn = ctx->doc ? css_layout_box_node(ctx, c) : NULL;
        bool text = dn && dn->type == HTML_NODE_TEXT;
        if (text || (b->s->display == CSS_DISPLAY_INLINE && !b->s->inline_level)) {
            pv_push(&L->inlines, c);
            paint_walk(ctx, c, L);
        } else if (b->s->display == CSS_DISPLAY_INLINE_BLOCK || b->s->inline_level) {
            pv_push(&L->inlines, -(c + 2));
        } else {
            pv_push(&L->blocks, c);
            paint_walk(ctx, c, L);
        }
    }
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
    paint_walk(ctx, root, &L);
    paint_sort_z(ctx, &L.negs);
    paint_sort_z(ctx, &L.pos);
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

static bool build_display_list_impl(LayoutContext *ctx, DisplayList *dl, float (*clip)[4])
{
    bool dark_mode = document_is_dark_mode(ctx);
    TextShaper *default_font = display_list_get_default_font();
    int pending_box = -1, pending_from = 0;
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

    for (int k = 0; k < order.n; k++) {
        int i = order.v[k];
        if (pending_box >= 0 && clip) dl_clip_range(dl, pending_from, clip[pending_box]);
        while (nmask > 0 && !mask_covers(ctx, i, mask_start[nmask - 1], mask_end[nmask - 1])) {
            nmask--;
            dl_add_mask_cmd(dl, DL_MASK_END, mask_idx[nmask], &dl->masks[mask_idx[nmask]]);
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
        pending_box = i;
        pending_from = dl->count;
        LayoutBox *box = &ctx->boxes[i];
        /* The pieces of a split inline that start with this box: where their
         * line is in the paint order. */
        int par = ctx->tree.nodes[i].parent_idx;
        if (par >= 0 && ctx->pieces && par < ctx->pieces_n && ctx->pieces[par].n > 0) {
            const LayoutBox *pb = &ctx->boxes[par];
            if (pb->s->block_in_inline == 2 && (pb->flags & LAYOUT_HAS_LAYOUT) && pb->s->display != CSS_DISPLAY_NONE &&
                pb->s->visibility != CSS_VISIBILITY_HIDDEN && pb->opacity_eff >= 0.004 &&
                !node_has_hidden_class(ctx, par) && !box_or_ancestor_hidden(ctx, par))
                paint_pieces(ctx, dl, par, i, (float)pb->opacity_eff);
        }
        if (!(box->flags & LAYOUT_HAS_LAYOUT)) continue;
        if (box->s->display == CSS_DISPLAY_NONE) continue;
        if (box->s->visibility == CSS_VISIBILITY_HIDDEN) continue;
        /* Effective opacity (product over ancestors) scales every paint. */
        float op = (float)box->opacity_eff;
        if (op < 0.004f) continue;
        if (node_has_hidden_class(ctx, i)) continue;
        if (box_or_ancestor_hidden(ctx, i)) continue;

        HtmlNode *node = NULL;
        if (ctx->doc) {
            node = css_layout_box_node(ctx, i);
        }

        /* mask-image: everything the box and its subtree paint goes through
         * the mask (closed above once the subtree is done). */
        if (box->s->mask_kind && nmask < 64) {
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
            }
        }
        /* box-shadow (outer): below the box, outside its rounded rectangle. */
        if (box->s->shadow_set && box->width > 0.0 && box->height > 0.0) {
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

        /* Backgrounds paint first: under the borders, list marker and the
         * box's content. */
        /* (a color however thin: a 1px box is a line) */
        if (!pieces && i != canvas_src && box->width > 0.0f && box->height > cap_top + cap_bot && box->s->background_color_a > 0.0f) {
            float bgx = (float)box->x, bgy = (float)box->y + cap_top, bgw = (float)box->width, bgh = (float)box->height - cap_top - cap_bot;
            snap_rect(&bgx, &bgy, &bgw, &bgh);
            if (!display_list_add_rect(dl, bgx, bgy, bgw, bgh, (float)box->s->background_color_r,
                                       (float)box->s->background_color_g, (float)box->s->background_color_b,
                                       (float)box->s->background_color_a * op))
                return false;
        }
        if (!pieces && box->width >= 2.0f && box->height >= 2.0f && box->width * box->height >= 6.0f) {
            if (g_image_cache && box->s->background_image_url[0] && i != canvas_src) emit_background_image(ctx, dl, box, NULL);
        }

        /* Replaced images: <img src="..."> */
        if (node && node->type == HTML_NODE_ELEMENT &&
            strcasecmp(node->tag_name, "img") == 0 && g_image_cache) {
            char *url = display_list_image_source(ctx->base_url, node, (float)box->width);
            if (url) {
                emit_image_async(dl, box, url, op);
                free(url);
            }
            continue;
        }

        /* Inline <svg>: the outermost one paints its whole subtree. */
        if (node && node->type == HTML_NODE_ELEMENT && node->tag_name &&
            strcasecmp(node->tag_name, "svg") == 0 && g_image_cache && ctx->doc) {
            int di = ctx->tree.nodes[i].dom_node_idx;
            if (!inside_svg(&ctx->doc->array, di)) emit_inline_svg(ctx, dl, box, di, op);
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
            cstyle.object_fit = 0;
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
                    if (ta <= 0.0f) ta = 1.0f;
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
                    text_shaper_set_spacing(box->s->letter_spacing, box->s->word_spacing, scale);
                    /* The white-space processed text the layout measured. */
                    const char *run_text = css_layout_box_text(ctx, i, css_layout_node_text(ctx, node));
                    /* The fragments its paragraph's layout placed, line by line
                     * (INLINE_LAYOUT_DESIGN.md), an ellipsis after one it cut. */
                    const LayoutTextFragList *tfl =
                        ctx->text_frags && i < ctx->text_frags_n ? &ctx->text_frags[i] : NULL;
                    if (tfl && tfl->n > 0) {
                        for (int q = 0; q < tfl->n; q++) {
                            const LayoutTextFrag *fr = &tfl->f[q];
                            float fr_r = tr, fr_g = tg, fr_b = tb, fr_a = ta;
                            if (fr->fl) {   /* (on a ::first-line) */
                                fr_r = (float)(fr->fl_rgba >> 24) / 255.0f;
                                fr_g = (float)((fr->fl_rgba >> 16) & 255) / 255.0f;
                                fr_b = (float)((fr->fl_rgba >> 8) & 255) / 255.0f;
                                fr_a = (float)(fr->fl_rgba & 255) / 255.0f * op;
                            }
                            if (!text_shaper_shape_span(font, run_text, fr->start, fr->end, (float)(box->x + fr->dx),
                                                        (float)(box->y + fr->dy), scale, (fr->level & 1) != 0, fr_r,
                                                        fr_g, fr_b, fr_a, dl))
                                return false;
                            if (fr->ellipsis &&
                                !text_shaper_shape_span(font, "\xE2\x80\xA6", 0, 3, (float)(box->x + fr->dx + fr->w),
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
                    emit_text_decorations(dl, from_cmd, fs, box->s->text_decoration,
                                          tr, tg, tb, ta);
                }
            }
        }

        /* display:list-item marker (bullet / ordinal) in the gutter to the
         * left of the item's first line. */
        /* (list-style-image: the image, when it has loaded, outside to the
         * left of the first line) */
        bool image_marker = false;
        if (g_image_cache && node && node->type == HTML_NODE_ELEMENT && box->s->display == CSS_DISPLAY_LIST_ITEM &&
            box->s->list_image && box->s->list_image[0]) {
            int iw = 0, ih = 0;
            if (image_cache_natural_size(g_image_cache, box->s->list_image, &iw, &ih) && iw > 0 && ih > 0) {
                int handle = image_cache_request(g_image_cache, box->s->list_image, iw, ih);
                if (handle >= 0) {
                    double mb = css_layout_marker_baseline(ctx, i);   /* (its bottom on the baseline) */
                    emit_image(dl, (float)(box->x - iw - 7.0), (float)(mb - ih), (float)iw, (float)ih, handle);
                    image_marker = true;
                }
            }
        }
        if (default_font && node && node->type == HTML_NODE_ELEMENT && !image_marker &&
            box->s->display == CSS_DISPLAY_LIST_ITEM && box->s->list_style_type != 0) {
            char marker[24];
            if (box->s->list_style_type == 4) {
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
            float mscale = (float)(mfs / 16.0);
            int mfslot = box->s->font_slot >= 0 ? box->s->font_slot : display_list_resolve_font_slot(
                box->s->font_family, box->s->font_weight, box->s->font_italic ? 1 : 0);
            TextShaper *mfont = display_list_get_font(mfslot);
            if (!mfont) mfont = default_font;
            float mw = 0.0f, mh = 0.0f;
            text_shaper_measure(mfont, marker, &mw, &mh);
            double mx = box->x - (double)mw * mscale - mfs * 0.4;
            if (mx < 0.0) mx = 0.0;
            text_shaper_shape_to_display_list_scaled(mfont, marker,
                    (float)mx, (float)box->y, mscale,
                    (float)box->s->color_r, (float)box->s->color_g,
                    (float)box->s->color_b, (float)box->s->color_a * op, dl);
        }

        /* Borders, painted as four filled edge rects so per-side widths work.
         * The color defaults to currentColor when no border-color was given.
         * Emitted before the small-box filter: a thin box (e.g. <hr>) still
         * needs its border painted. */
        if (!pieces && (box->s->border_top > 0.0 || box->s->border_right > 0.0 ||
                        box->s->border_bottom > 0.0 || box->s->border_left > 0.0)) {
            float cl[4], cr[4], ct[4], cb[4];
            border_side_color(box, 0, op, cl);
            border_side_color(box, 1, op, cr);
            border_side_color(box, 2, op, ct);
            border_side_color(box, 3, op, cb);
            float br_r = ct[0], br_g = ct[1], br_b = ct[2], br_a = ct[3];   /* (a rounded ring: one color) */
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
                    display_list_add_rect(dl, bx, by, bw, bh, br_r, br_g, br_b, br_a);
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
    if (pending_box >= 0 && clip) dl_clip_range(dl, pending_from, clip[pending_box]);
    while (nmask > 0) {
        nmask--;
        dl_add_mask_cmd(dl, DL_MASK_END, mask_idx[nmask], &dl->masks[mask_idx[nmask]]);
    }

    return true;
}

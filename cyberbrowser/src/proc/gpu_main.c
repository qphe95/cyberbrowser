/* gpu_main.c - cb_gpu.exe: the GPU process (SANDBOX_DESIGN.md section 5),
 * the shim around the verified decoder (gpu_decode.c).
 *
 * Started by the coordinator with its control channel:
 *   cb_gpu.exe --chan S E0 E1
 *
 * The coordinator grants it a draw channel per tab (GRANT_TAB, the tab's id
 * and site) and one to the UI (GRANT_UI).  A tab uploads glyph atlas bands
 * and images into its own namespace and sends display lists; each is copied
 * out of the channel, decoded against that tab's namespace alone, drawn into
 * that tab's target, and presented in each of the UI's content windows that
 * shows that tab.  Nothing is ever read back to a tab.  The UI says which
 * tab each of its windows shows, and asks for their pixels for screenshots.
 *
 * Compositor scrolling: a tab draws a layer reaching past its viewport, and
 * says where the viewport lay in it (GpuFrame).  The UI sends each scroll to
 * the tab and here at once (UG_SCROLL), numbered; until a frame says the tab
 * has seen a scroll, its windows show the layer moved by it, so scrolling
 * keeps up however busy the tab is.
 *
 * It draws with Vulkan (gpu_vk.c) when it can, else with the CPU rasterizer
 * (raster_cpu.c), presenting with GDI; CYBER_GPU=cpu asks for the CPU.
 *
 * CYBER_GPU_CRASH_TEST=N (tests): exit after drawing N frames, as a crashing
 * driver would, so the coordinator's restart can be watched. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "chan.h"
#include "bigmsg.h"
#include "proto.h"
#include "spawn.h"
#include "site.h"
#include "gpu_wire.h"
#include "gpu_decode.h"
#include "gpu_backend.h"

#define MAX_CLIENTS 61                  /* + the control and UI channels: 63 waits */
#define CLIENT_BYTES (1024ull << 20)    /* a tab's resources, at most */

typedef struct Res {
    uint8_t *px;                        /* the CPU copy: w * h (atlas) or w * h * 4 (image) */
    uint32_t w, h;
    GpuTexture *tex;
} Res;

typedef struct Client {
    uint32_t tab;
    char site[SITE_MAX];
    Chan chan;                          /* side 1: the GPU process */
    BigRx rx;
    GpuClientRes ns;                    /* what it uploaded: the decoder's view */
    Res atlas[GPU_FONT_SLOTS];
    Res *images;                        /* GPU_MAX_IMAGES */
    uint64_t bytes;
    GpuTarget *target;
    uint32_t width, height;
    GpuFrame at;                        /* the last frame's place in the page */
    struct { uint32_t seq; int32_t dy; } scrolls[64];  /* the UI's, since the last frame's */
    uint32_t nscrolls;
} Client;

/* One of the UI's content windows. */
typedef struct Window {
    HWND hwnd;
    uint32_t tab;                       /* what it shows, 0 none */
    uint32_t w, h;
    bool shown;                         /* the UI heard it shows tab (GU_SHOWN) */
    bool dirty;                         /* to present again */
} Window;

#define MAX_WINDOWS 16

static Chan g_ctl, g_ui;
static bool g_have_ui;
static BigRx g_ui_rx;
static Client *g_clients[MAX_CLIENTS];
static const GpuBackend *g_be;
static Window g_windows[MAX_WINDOWS];

/* ---- the CPU backend ----------------------------------------------------------- */

struct GpuTarget {
    uint32_t w, h;
    uint8_t *bgrx;
};

static bool cpu_render(GpuTarget **tp, uint32_t w, uint32_t h, const DisplayList *dl, GpuTexture *const *atlases,
                       GpuTexture *const *images, const RasterResources *res) {
    (void)atlases;
    (void)images;
    GpuTarget *t = *tp;
    if (!t || t->w != w || t->h != h) {
        if (t) free(t->bgrx);
        free(t);
        t = (GpuTarget *)calloc(1, sizeof(*t));
        if (t) t->bgrx = (uint8_t *)malloc((size_t)w * h * 4);
        if (!t || !t->bgrx) {
            free(t);
            *tp = NULL;
            return false;
        }
        t->w = w;
        t->h = h;
        *tp = t;
    }
    static uint8_t *rgb;
    static size_t rgb_bytes;
    if (rgb_bytes < (size_t)w * h * 3) {
        free(rgb);
        rgb_bytes = (size_t)w * h * 3;
        rgb = (uint8_t *)malloc(rgb_bytes);
        if (!rgb) { rgb_bytes = 0; return false; }
    }
    raster_display_list_with(dl, res, rgb, (int)w, (int)h);
    const uint8_t *in = rgb;
    uint8_t *out = t->bgrx;
    for (size_t i = 0, n = (size_t)w * h; i < n; i++, in += 3, out += 4) {
        out[0] = in[2];
        out[1] = in[1];
        out[2] = in[0];
        out[3] = 255;
    }
    return true;
}

static void cpu_target_free(GpuTarget *t) {
    if (t) free(t->bgrx);
    free(t);
}

static bool cpu_present(HWND hwnd, uint32_t w, uint32_t h, GpuTarget *t, int32_t src_y) {
    HDC dc = GetDC(hwnd);
    if (!dc) return false;
    int64_t top = src_y < 0 ? 0 : src_y, bottom = (int64_t)src_y + h;
    if (bottom > t->h) bottom = t->h;
    HBRUSH white = (HBRUSH)GetStockObject(WHITE_BRUSH);
    if (bottom > top) {
        BITMAPINFO bi;
        memset(&bi, 0, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = (LONG)t->w;
        bi.bmiHeader.biHeight = -(LONG)(bottom - top);
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        SetDIBitsToDevice(dc, 0, (int)(top - src_y), t->w, (DWORD)(bottom - top), 0, 0, 0, (UINT)(bottom - top),
                          t->bgrx + (size_t)top * t->w * 4, &bi, DIB_RGB_COLORS);
    } else {
        top = bottom = src_y;
    }
    /* What the target doesn't cover. */
    if (top > src_y) { RECT r = { 0, 0, (LONG)w, (LONG)(top - src_y) }; FillRect(dc, &r, white); }
    if (t->w < w) { RECT r = { (LONG)t->w, 0, (LONG)w, (LONG)h }; FillRect(dc, &r, white); }
    if (bottom - src_y < h) { RECT r = { 0, (LONG)(bottom - src_y), (LONG)w, (LONG)h }; FillRect(dc, &r, white); }
    ReleaseDC(hwnd, dc);
    return true;
}

static bool cpu_readback(GpuTarget *t, uint8_t *out, uint32_t *w, uint32_t *h) {
    memcpy(out, t->bgrx, (size_t)t->w * t->h * 4);
    *w = t->w;
    *h = t->h;
    return true;
}

static const GpuBackend g_cpu = {
    "cpu", NULL, NULL, NULL, cpu_render, cpu_target_free, cpu_present, cpu_readback, NULL,
};

/* ---- a tab's resources --------------------------------------------------------- */

static bool client_atlas(void *user, int slot, const uint8_t **pixels, int *w, int *h) {
    Client *c = (Client *)user;
    if (slot < 0 || slot >= GPU_FONT_SLOTS || !c->ns.font[slot]) return false;
    *pixels = c->atlas[slot].px;
    *w = (int)c->atlas[slot].w;
    *h = (int)c->atlas[slot].h;
    return true;
}

static bool client_image(void *user, int id, const uint8_t **pixels, int *w, int *h, int *ch) {
    Client *c = (Client *)user;
    if (id < 0 || id >= GPU_MAX_IMAGES || !c->ns.image[id]) return false;
    *pixels = c->images[id].px;
    *w = (int)c->images[id].w;
    *h = (int)c->images[id].h;
    *ch = 4;
    return true;
}

static void res_free(Res *r) {
    if (r->tex && g_be->texture_free) g_be->texture_free(r->tex);
    free(r->px);
    memset(r, 0, sizeof(*r));
}

/* TG_ATLAS: rows of a font slot's atlas. */
static void upload_atlas(Client *c, const uint8_t *m, uint32_t len) {
    TgAtlas a;
    if (len < sizeof(a)) return;
    memcpy(&a, m, sizeof(a));
    if (a.slot >= GPU_FONT_SLOTS || a.width == 0 || a.height == 0 || a.width > 4096 || a.height > 4096 ||
        a.y0 > a.height || a.rows > a.height - a.y0 || len - sizeof(a) != (uint64_t)a.rows * a.width)
        return;
    Res *r = &c->atlas[a.slot];
    if (!r->px || r->w != a.width || r->h != a.height) {
        uint64_t bytes = (uint64_t)a.width * a.height;
        if (c->bytes + bytes > CLIENT_BYTES) return;
        if (r->px) c->bytes -= (uint64_t)r->w * r->h;
        res_free(r);
        r->px = (uint8_t *)calloc(1, bytes);
        if (!r->px) return;
        r->w = a.width;
        r->h = a.height;
        c->bytes += bytes;
        if (g_be->texture_new) r->tex = g_be->texture_new(false, a.width, a.height);
    }
    memcpy(r->px + (size_t)a.y0 * a.width, m + sizeof(a), (size_t)a.rows * a.width);
    if (r->tex) g_be->texture_update(r->tex, a.y0, a.rows, r->px + (size_t)a.y0 * a.width);
    c->ns.font[a.slot] = 1;
}

/* TG_IMAGE: an image, new or replaced. */
static void upload_image(Client *c, const uint8_t *m, uint32_t len) {
    TgImage t;
    if (len < sizeof(t)) return;
    memcpy(&t, m, sizeof(t));
    if (t.id >= GPU_MAX_IMAGES || t.width == 0 || t.height == 0 || t.width > 16384 || t.height > 16384 ||
        len - sizeof(t) != (uint64_t)t.width * t.height * 4)
        return;
    Res *r = &c->images[t.id];
    uint64_t bytes = (uint64_t)t.width * t.height * 4;
    if (!r->px || r->w != t.width || r->h != t.height) {
        uint64_t old = r->px ? (uint64_t)r->w * r->h * 4 : 0;
        if (c->bytes - old + bytes > CLIENT_BYTES) {
            fprintf(stderr, "[gpu] tab %u: over its %llu MB for resources, image %u dropped\n", c->tab,
                    CLIENT_BYTES >> 20, t.id);
            return;
        }
        c->bytes -= old;
        c->ns.image[t.id] = 0;
        res_free(r);
        r->px = (uint8_t *)malloc(bytes);
        if (!r->px) return;
        r->w = t.width;
        r->h = t.height;
        c->bytes += bytes;
        if (g_be->texture_new) r->tex = g_be->texture_new(true, t.width, t.height);
    }
    memcpy(r->px, m + sizeof(t), bytes);
    if (r->tex) g_be->texture_update(r->tex, 0, t.height, r->px);
    c->ns.image[t.id] = 1;
}

/* ---- frames -------------------------------------------------------------------- */

static float len_px(int32_t v) { return (float)v / (float)WIRE_UNIT; }
static float unit(uint16_t v) { return (float)v / 65535.0f; }
static float uvf(uint32_t v) { return (float)((double)v / WIRE_UV_ONE); }

/* The decoded records as the rasterizers' DisplayList. */
static void to_display_list(const WireCmd *cmds, uint32_t n, const WireMask *masks, uint32_t nm, DisplayList *dl) {
    for (uint32_t i = 0; i < n; i++) {
        const WireCmd *w = &cmds[i];
        DisplayListCmd *c = &dl->cmds[i];
        memset(c, 0, sizeof(*c));
        c->type = (DisplayListCmdType)w->type;
        c->x = len_px(w->x);
        c->y = len_px(w->y);
        c->w = len_px(w->w);
        c->h = len_px(w->h);
        c->r = unit(w->r);
        c->g = unit(w->g);
        c->b = unit(w->b);
        c->a = unit(w->a);
        switch (w->type) {
        case WIRE_BORDER:
            c->u.border.x = c->x;
            c->u.border.y = c->y;
            c->u.border.w = c->w;
            c->u.border.h = c->h;
            c->u.border.thickness = len_px(w->thickness);
            break;
        case WIRE_GLYPH:
            c->u.glyph.font_slot = (uint8_t)w->ref;
            c->u.glyph.u0 = uvf(w->u0);
            c->u.glyph.v0 = uvf(w->v0);
            c->u.glyph.u1 = uvf(w->u1);
            c->u.glyph.v1 = uvf(w->v1);
            break;
        case WIRE_IMAGE:
            c->u.image.image_handle = (int)w->ref;
            c->u.image.u0 = uvf(w->u0);
            c->u.image.v0 = uvf(w->v0);
            c->u.image.u1 = uvf(w->u1);
            c->u.image.v1 = uvf(w->v1);
            break;
        case WIRE_MASK_BEGIN:
        case WIRE_SHADOW:
            c->u.mask_index = (int)w->ref;
            break;
        default:
            break;
        }
    }
    for (uint32_t i = 0; i < nm; i++) {
        const WireMask *w = &masks[i];
        DisplayMask *m = &dl->masks[i];
        memset(m, 0, sizeof(*m));
        m->kind = (int)w->kind;
        m->image_handle = w->image == GPU_NO_IMAGE ? -1 : (int)w->image;
        m->x = len_px(w->x);
        m->y = len_px(w->y);
        m->w = len_px(w->w);
        m->h = len_px(w->h);
        m->ix = len_px(w->ix);
        m->iy = len_px(w->iy);
        m->iw = len_px(w->iw);
        m->ih = len_px(w->ih);
        m->repeat = (int)w->repeat;
        m->angle_deg = len_px(w->angle);
        m->nstops = (int)w->nstops;
        for (int k = 0; k < WIRE_MASK_STOPS && k < DL_MASK_STOPS; k++) {
            m->stop_pos[k] = unit(w->stop_pos[k]);
            m->stop_alpha[k] = unit(w->stop_alpha[k]);
        }
        for (int k = 0; k < 4; k++) {
            m->rx[k] = len_px(w->rx[k]);
            m->ry[k] = len_px(w->ry[k]);
            m->border[k] = len_px(w->border[k]);
            m->ex_rx[k] = len_px(w->ex_rx[k]);
            m->ex_ry[k] = len_px(w->ex_ry[k]);
        }
        m->blur = len_px(w->blur);
    }
    dl->count = (int)n;
    dl->mask_count = (int)nm;
}

static Client *client_of(uint32_t tab);

/* Where the viewport of c lies now, in the page: as its last frame says,
 * moved by the scrolls the tab hadn't seen, clamped as the tab clamps. */
static int64_t view_y(const Client *c) {
    int64_t max = c->at.content_h > c->at.view_h ? (int64_t)c->at.content_h - c->at.view_h : 0;
    int64_t y = c->at.scroll_y < max ? c->at.scroll_y : max;
    for (uint32_t i = 0; i < c->nscrolls; i++) {
        y += c->scrolls[i].dy;
        y = y < 0 ? 0 : y > max ? max : y;
    }
    return y;
}

/* The row of c's layer at the top of its viewport now. */
static int32_t layer_row(const Client *c) {
    int64_t r = view_y(c) - (int64_t)c->at.origin_y;
    return r < -GPU_MAX_H ? -GPU_MAX_H : r > GPU_MAX_H ? GPU_MAX_H : (int32_t)r;
}

static void mark_dirty(uint32_t tab) {
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (g_windows[i].hwnd && g_windows[i].tab == tab) g_windows[i].dirty = true;
}

/* Present every window with something new to show. */
static void present_dirty(void) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        Window *w = &g_windows[i];
        if (!w->hwnd || !w->dirty) continue;
        w->dirty = false;
        Client *c = client_of(w->tab);
        if (!c || !c->target) continue;
        if (g_be->present(w->hwnd, w->w, w->h, c->target, layer_row(c)) && !w->shown) {
            w->shown = true;
            GuShown s = { c->tab, (uint64_t)(uintptr_t)w->hwnd };
            if (g_have_ui) big_send(&g_ui, GU_SHOWN, &s, sizeof(s), NULL, 0, NULL, 0, 100);
        }
    }
}

/* TG_FRAME: decode against this tab's namespace, draw, present, answer. */
static void frame(Client *c, const uint8_t *m, uint32_t len) {
    static WireCmd *cmds;
    static uint8_t *levels;
    static WireMask *masks;
    static uint32_t cmd_cap, mask_cap;
    static DisplayList dl;
    uint32_t in_cmds = len >= WIRE_FRAME_BYTES ? ((const GpuFrame *)m)->ncmds : 0;
    uint32_t in_masks = len >= WIRE_FRAME_BYTES ? ((const GpuFrame *)m)->nmasks : 0;
    if (in_cmds > GPU_MAX_CMDS || in_masks > GPU_MAX_MASKS) return;
    if (cmd_cap < in_cmds + GPU_MAX_DEPTH) {
        cmd_cap = in_cmds + GPU_MAX_DEPTH;
        cmds = (WireCmd *)realloc(cmds, (size_t)cmd_cap * sizeof(WireCmd));
        levels = (uint8_t *)realloc(levels, cmd_cap);
        dl.cmds = (DisplayListCmd *)realloc(dl.cmds, (size_t)cmd_cap * sizeof(DisplayListCmd));
        if (!cmds || !levels || !dl.cmds) { cmd_cap = 0; return; }
    }
    if (mask_cap < in_masks || !masks) {
        mask_cap = in_masks ? in_masks : 1;
        masks = (WireMask *)realloc(masks, (size_t)mask_cap * sizeof(WireMask));
        dl.masks = (DisplayMask *)realloc(dl.masks, (size_t)mask_cap * sizeof(DisplayMask));
        if (!masks || !dl.masks) { mask_cap = 0; return; }
    }
    GpuFrame f;
    uint32_t n = 0, nm = 0;
    if (!gpu_decode(m, len, &c->ns, &f, cmds, levels, cmd_cap, &n, masks, mask_cap, &nm)) {
        fprintf(stderr, "[gpu] tab %u: malformed frame (%u bytes)\n", c->tab, len);
        return;
    }
    to_display_list(cmds, n, masks, nm, &dl);
    RasterResources res = { client_atlas, client_image, c };
    static GpuTexture **atlases, **images;
    if (!atlases) {
        atlases = (GpuTexture **)calloc(GPU_FONT_SLOTS, sizeof(*atlases));
        images = (GpuTexture **)calloc(GPU_MAX_IMAGES, sizeof(*images));
        if (!atlases || !images) return;
    }
    for (int i = 0; i < GPU_FONT_SLOTS; i++) atlases[i] = c->ns.font[i] ? c->atlas[i].tex : NULL;
    for (int i = 0; i < GPU_MAX_IMAGES; i++) images[i] = c->ns.image[i] ? c->images[i].tex : NULL;
    if (g_be->render(&c->target, f.width, f.height, &dl, atlases, images, &res)) {
        c->width = f.width;
        c->height = f.height;
        /* The layer's place: the header's last five numbers, which the
         * decoder leaves alone (they are only clamped where they are used). */
        memcpy(&c->at, m, sizeof(c->at));
        c->at.width = f.width;
        c->at.height = f.height;
        /* The scrolls this frame has seen are in its scroll_y. */
        uint32_t k = 0;
        for (uint32_t i = 0; i < c->nscrolls; i++)
            if ((int32_t)(c->scrolls[i].seq - f.scroll_seq) > 0) c->scrolls[k++] = c->scrolls[i];
        c->nscrolls = k;
        mark_dirty(c->tab);
    }
    static int crash_after = -1;
    if (crash_after < 0) crash_after = getenv("CYBER_GPU_CRASH_TEST") ? atoi(getenv("CYBER_GPU_CRASH_TEST")) : 0;
    static int frames;
    if (crash_after > 0 && ++frames >= crash_after) {
        fprintf(stderr, "[gpu] CYBER_GPU_CRASH_TEST: ending after %d frames\n", frames);
        fflush(stderr);
        ExitProcess(3);
    }
    GtFrameDone d = { f.frame };
    big_send(&c->chan, GT_FRAME_DONE, &d, sizeof(d), NULL, 0, NULL, 0, 1000);
}

static void client_message(Client *c) {
    switch (c->rx.type) {
    case TG_FRAME: frame(c, c->rx.buf, c->rx.total); break;
    case TG_ATLAS: upload_atlas(c, c->rx.buf, c->rx.total); break;
    case TG_IMAGE: upload_image(c, c->rx.buf, c->rx.total); break;
    default: break;
    }
}

static Client *client_of(uint32_t tab) {
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (g_clients[i] && g_clients[i]->tab == tab) return g_clients[i];
    return NULL;
}

/* ---- the UI ---------------------------------------------------------------------- */

static Window *window_of(HWND hwnd) {
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (hwnd && g_windows[i].hwnd == hwnd) return &g_windows[i];
    return NULL;
}

static void ui_message(void) {
    if (g_ui_rx.type == UG_WINDOW && g_ui_rx.total == sizeof(UgWindow)) {
        UgWindow u;
        memcpy(&u, g_ui_rx.buf, sizeof(u));
        HWND hwnd = (HWND)(uintptr_t)u.hwnd;
        Window *w = window_of(hwnd);
        if (u.width == 0 && u.height == 0) {
            /* The window is gone. */
            if (w) {
                if (g_be->window_gone) g_be->window_gone(hwnd);
                memset(w, 0, sizeof(*w));
            }
            return;
        }
        if (!hwnd || !IsWindow(hwnd)) return;
        for (int i = 0; i < MAX_WINDOWS && !w; i++)
            if (!g_windows[i].hwnd) {
                w = &g_windows[i];
                memset(w, 0, sizeof(*w));
                w->hwnd = hwnd;
            }
        if (!w) return;
        if (u.tab != w->tab) w->shown = false;
        w->tab = u.tab;
        w->w = u.width < GPU_MAX_W ? u.width : GPU_MAX_W;
        w->h = u.height < GPU_MAX_H ? u.height : GPU_MAX_H;
        w->dirty = true;
    } else if (g_ui_rx.type == UG_SCROLL && g_ui_rx.total == sizeof(UgScroll)) {
        UgScroll q;
        memcpy(&q, g_ui_rx.buf, sizeof(q));
        Client *c = client_of(q.tab);
        if (!c) return;
        uint32_t cap = (uint32_t)(sizeof(c->scrolls) / sizeof(c->scrolls[0]));
        if (c->nscrolls == cap) {
            /* The tab is far behind: forget the oldest (its next frame
             * puts the viewport right). */
            memmove(&c->scrolls[0], &c->scrolls[1], (cap - 1) * sizeof(c->scrolls[0]));
            c->nscrolls--;
        }
        c->scrolls[c->nscrolls].seq = q.seq;
        c->scrolls[c->nscrolls].dy = q.dy < -GPU_COORD_MAX ? -GPU_COORD_MAX : q.dy > GPU_COORD_MAX ? GPU_COORD_MAX : q.dy;
        c->nscrolls++;
        mark_dirty(c->tab);
    } else if (g_ui_rx.type == UG_SHOT && g_ui_rx.total == sizeof(UgShot)) {
        /* What the window shows: its rows of the layer, white where it has none. */
        UgShot q;
        memcpy(&q, g_ui_rx.buf, sizeof(q));
        Window *w = window_of((HWND)(uintptr_t)q.hwnd);
        Client *c = w ? client_of(w->tab) : NULL;
        static uint8_t *px, *out;
        GuShot s = { 0, 0 };
        if (c && c->target && w->h) {
            free(px);
            free(out);
            px = (uint8_t *)malloc((size_t)c->width * c->height * 4);
            out = (uint8_t *)malloc((size_t)c->width * w->h * 4);
            uint32_t tw = 0, th = 0;
            if (px && out && g_be->readback(c->target, px, &tw, &th)) {
                int32_t row = layer_row(c);
                memset(out, 255, (size_t)tw * w->h * 4);
                for (uint32_t y = 0; y < w->h; y++) {
                    int64_t sy = (int64_t)row + y;
                    if (sy >= 0 && sy < th) memcpy(out + (size_t)y * tw * 4, px + (size_t)sy * tw * 4, (size_t)tw * 4);
                }
                s.width = tw;
                s.height = w->h;
            }
        }
        big_send(&g_ui, GU_SHOT, &s, sizeof(s), out, s.width * s.height * 4, NULL, 0, 5000);
    }
}

/* ---- the coordinator's grants ------------------------------------------------------ */

static void client_free(Client *c) {
    for (int i = 0; i < GPU_FONT_SLOTS; i++) res_free(&c->atlas[i]);
    for (int i = 0; c->images && i < GPU_MAX_IMAGES; i++)
        if (c->images[i].px) res_free(&c->images[i]);
    free(c->images);
    if (c->target) g_be->target_free(c->target);
    big_free(&c->rx);
    chan_close(&c->chan);
    free(c);
}

static void grant(const CsGrant *g) {
    HANDLE s = (HANDLE)(uintptr_t)g->chan_section, e0 = (HANDLE)(uintptr_t)g->chan_ev0,
           e1 = (HANDLE)(uintptr_t)g->chan_ev1;
    if (g->kind == GRANT_UI) {
        if (g_have_ui) chan_close(&g_ui);
        g_have_ui = chan_attach(&g_ui, s, e0, e1, 1);
        return;
    }
    if (g->kind != GRANT_TAB || g->site[PROTO_SITE_MAX - 1]) return;
    int slot = -1;
    for (int i = 0; i < MAX_CLIENTS && slot < 0; i++)
        if (!g_clients[i]) slot = i;
    Client *c = (Client *)calloc(1, sizeof(Client));
    if (c) c->images = (Res *)calloc(GPU_MAX_IMAGES, sizeof(Res));
    if (slot < 0 || !c || !c->images || !chan_attach(&c->chan, s, e0, e1, 1)) {
        if (c) free(c->images);
        free(c);
        CloseHandle(s); CloseHandle(e0); CloseHandle(e1);
        return;
    }
    c->tab = g->client;
    memcpy(c->site, g->site, SITE_MAX);
    g_clients[slot] = c;
}

static void revoke(uint32_t tab) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        Client *c = g_clients[i];
        if (!c || c->tab != tab) continue;
        g_clients[i] = NULL;
        client_free(c);
    }
}

int main(int argc, char **argv) {
    if (argc < 5 || strcmp(argv[1], "--chan") ||
        !chan_attach(&g_ctl, spawn_handle_from_arg(argv[2]), spawn_handle_from_arg(argv[3]),
                     spawn_handle_from_arg(argv[4]), 1)) {
        fprintf(stderr, "cb_gpu: bad arguments\n");
        return 1;
    }
    const char *want = getenv("CYBER_GPU");
    g_be = want && !strcmp(want, "cpu") ? NULL : gpu_vk_backend();
    if (!g_be) g_be = &g_cpu;
    fprintf(stderr, "[gpu] drawing with %s\n", g_be->name);
    if (getenv("CYBER_GPU_CRASH_TEST")) fprintf(stderr, "[gpu] CYBER_GPU_CRASH_TEST=%s\n", getenv("CYBER_GPU_CRASH_TEST"));
    static ChanMsg m;
    for (;;) {
        int r;
        while ((r = chan_recv(&g_ctl, &m)) > 0) {
            if (m.type == CS_GRANT && m.len == sizeof(CsGrant)) {
                CsGrant g;
                memcpy(&g, m.data, sizeof(g));
                grant(&g);
            } else if (m.type == CS_REVOKE && m.len == sizeof(CsRevoke)) {
                CsRevoke v;
                memcpy(&v, m.data, sizeof(v));
                revoke(v.client);
            } else if (m.type == CS_QUIT) {
                return 0;
            }
        }
        if (r < 0) return 1;
        if (g_have_ui) {
            int got;
            while ((got = big_recv(&g_ui, &g_ui_rx)) > 0) ui_message();
            if (got < 0) {
                fprintf(stderr, "[gpu] the UI broke its channel\n");
                chan_close(&g_ui);
                g_have_ui = false;
            }
        }
        for (int i = 0; i < MAX_CLIENTS; i++) {
            Client *c = g_clients[i];
            int got;
            while (c && (got = big_recv(&c->chan, &c->rx)) > 0) client_message(c);
            if (c && got < 0) {
                fprintf(stderr, "[gpu] tab %u broke its channel\n", c->tab);
                revoke(c->tab);
            }
        }
        present_dirty();
        fflush(stderr);
        HANDLE waits[MAX_CLIENTS + 2];
        int n = 0;
        bool ready = !chan_arm(&g_ctl);
        waits[n++] = chan_event(&g_ctl);
        if (g_have_ui) {
            if (!chan_arm(&g_ui)) ready = true;
            waits[n++] = chan_event(&g_ui);
        }
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!g_clients[i]) continue;
            if (!chan_arm(&g_clients[i]->chan)) ready = true;
            waits[n++] = chan_event(&g_clients[i]->chan);
        }
        if (!ready) WaitForMultipleObjects((DWORD)n, waits, FALSE, INFINITE);
        chan_disarm(&g_ctl);
        if (g_have_ui) chan_disarm(&g_ui);
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (g_clients[i]) chan_disarm(&g_clients[i]->chan);
    }
}

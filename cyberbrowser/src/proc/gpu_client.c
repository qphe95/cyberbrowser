/* gpu_client.c - a tab's side of the GPU process (gpu_client.h). */
#include "gpu_client.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "bigmsg.h"
#include "proto.h"
#include "gpu_wire.h"
#include "gpu_decode.h"
#include "font_registry.h"
#include "text_shaper.h"
#include "image_cache.h"

_Static_assert(sizeof(GpuFrame) == WIRE_FRAME_BYTES, "the decoder reads GpuFrame byte by byte");
_Static_assert(sizeof(WireCmd) == WIRE_CMD_BYTES, "the decoder reads WireCmd byte by byte");
_Static_assert(sizeof(WireMask) == WIRE_MASK_BYTES, "the decoder reads WireMask byte by byte");

static Chan g_ch;
static BigRx g_rx;
static bool g_attached;
static bool g_font_sent[GPU_FONT_SLOTS];
static uint64_t g_image_gen[GPU_MAX_IMAGES];        /* the generation the GPU process has */

bool gpu_client_attach(HANDLE section, HANDLE ev0, HANDLE ev1) {
    if (g_attached) chan_close(&g_ch);
    big_free(&g_rx);
    memset(&g_rx, 0, sizeof(g_rx));
    g_attached = chan_attach(&g_ch, section, ev0, ev1, 0);
    gpu_client_reset();
    return g_attached;
}

void gpu_client_reset(void) {
    memset(g_font_sent, 0, sizeof(g_font_sent));
    memset(g_image_gen, 0, sizeof(g_image_gen));
}

HANDLE gpu_client_event(void) { return chan_event(&g_ch); }
bool gpu_client_arm(void) { return chan_arm(&g_ch); }
void gpu_client_disarm(void) { chan_disarm(&g_ch); }

/* ---- encoding ------------------------------------------------------------------ */

static int32_t fx(float v) {
    double d = (double)v * WIRE_UNIT;
    if (!(d == d)) return 0;                        /* NaN */
    if (d > 2147483520.0) return 2147483520;
    if (d < -2147483520.0) return -2147483520;
    return (int32_t)llround(d);
}

static uint16_t unit16(float v) {
    if (!(v > 0.0f)) return 0;
    if (v >= 1.0f) return 65535;
    return (uint16_t)lroundf(v * 65535.0f);
}

static uint32_t uv(float v) {
    if (!(v > 0.0f)) return 0;
    if (v >= 1.0f) return WIRE_UV_ONE;
    return (uint32_t)llround((double)v * WIRE_UV_ONE);
}

uint32_t gpu_encode_frame(const DisplayList *dl, const GpuFrame *hdr, uint8_t **out) {
    size_t bytes = WIRE_FRAME_BYTES + (size_t)dl->count * WIRE_CMD_BYTES + (size_t)dl->mask_count * WIRE_MASK_BYTES;
    if (bytes > BIG_MAX) return 0;
    uint8_t *buf = (uint8_t *)malloc(bytes);
    if (!buf) return 0;
    GpuFrame f = *hdr;
    f.ncmds = (uint32_t)dl->count;
    f.nmasks = (uint32_t)dl->mask_count;
    memcpy(buf, &f, sizeof(f));
    WireCmd *wc = (WireCmd *)(buf + WIRE_FRAME_BYTES);
    for (int i = 0; i < dl->count; i++) {
        const DisplayListCmd *c = &dl->cmds[i];
        WireCmd w;
        memset(&w, 0, sizeof(w));
        w.type = (uint32_t)c->type;
        w.x = fx(c->x);
        w.y = fx(c->y);
        w.w = fx(c->w);
        w.h = fx(c->h);
        w.r = unit16(c->r);
        w.g = unit16(c->g);
        w.b = unit16(c->b);
        w.a = unit16(c->a);
        switch (c->type) {
        case DL_BORDER:
            w.thickness = fx(c->u.border.thickness);
            break;
        case DL_GLYPH:
            w.ref = c->u.glyph.font_slot;
            w.u0 = uv(c->u.glyph.u0);
            w.v0 = uv(c->u.glyph.v0);
            w.u1 = uv(c->u.glyph.u1);
            w.v1 = uv(c->u.glyph.v1);
            break;
        case DL_IMAGE:
            w.ref = (uint32_t)c->u.image.image_handle;
            w.u0 = uv(c->u.image.u0);
            w.v0 = uv(c->u.image.v0);
            w.u1 = uv(c->u.image.u1);
            w.v1 = uv(c->u.image.v1);
            break;
        case DL_MASK_BEGIN:
        case DL_SHADOW:
            w.ref = (uint32_t)c->u.mask_index;
            break;
        default:
            break;
        }
        memcpy(&wc[i], &w, sizeof(w));
    }
    WireMask *wm = (WireMask *)(buf + WIRE_FRAME_BYTES + (size_t)dl->count * WIRE_CMD_BYTES);
    for (int i = 0; i < dl->mask_count; i++) {
        const DisplayMask *m = &dl->masks[i];
        WireMask w;
        memset(&w, 0, sizeof(w));
        w.kind = (uint32_t)m->kind;
        w.image = (uint32_t)m->image_handle;
        w.x = fx(m->x);
        w.y = fx(m->y);
        w.w = fx(m->w);
        w.h = fx(m->h);
        w.ix = fx(m->ix);
        w.iy = fx(m->iy);
        w.iw = fx(m->iw);
        w.ih = fx(m->ih);
        w.repeat = m->repeat ? 1 : 0;
        w.angle = fx(m->angle_deg);
        w.nstops = (uint32_t)(m->nstops < 0 ? 0 : m->nstops);
        for (int k = 0; k < WIRE_MASK_STOPS && k < DL_MASK_STOPS; k++) {
            w.stop_pos[k] = unit16(m->stop_pos[k]);
            w.stop_alpha[k] = unit16(m->stop_alpha[k]);
        }
        for (int k = 0; k < 4; k++) {
            w.rx[k] = fx(m->rx[k]);
            w.ry[k] = fx(m->ry[k]);
            w.border[k] = fx(m->border[k]);
            w.ex_rx[k] = fx(m->ex_rx[k]);
            w.ex_ry[k] = fx(m->ex_ry[k]);
        }
        w.blur = fx(m->blur);
        memcpy(&wm[i], &w, sizeof(w));
    }
    *out = buf;
    return (uint32_t)bytes;
}

/* ---- resources ------------------------------------------------------------------ */

static bool send_atlas(int slot, TextShaper *f, int y0, int y1) {
    const uint8_t *px = text_shaper_atlas_pixels(f);
    int w = text_shaper_atlas_width(f), h = text_shaper_atlas_height(f);
    if (!px || w <= 0 || h <= 0) return true;
    if (y1 > h) y1 = h;
    if (y0 < 0) y0 = 0;
    if (y1 <= y0) y1 = y0 + 1 <= h ? y0 + 1 : h;   /* at least a row, so the slot exists there */
    TgAtlas a = { (uint32_t)slot, (uint32_t)w, (uint32_t)h, (uint32_t)y0, (uint32_t)(y1 - y0) };
    return big_send(&g_ch, TG_ATLAS, &a, sizeof(a), px + (size_t)y0 * w, (uint32_t)((size_t)(y1 - y0) * w), NULL, 0,
                    5000);
}

static bool send_font(int slot) {
    if (slot < 0 || slot >= GPU_FONT_SLOTS) return true;
    TextShaper *f = font_registry_slot(slot);
    if (!f || !text_shaper_atlas_pixels(f)) return true;
    int y0, y1;
    if (!g_font_sent[slot]) {
        /* Everything drawn so far; what is drawn after goes as dirty rows. */
        text_shaper_atlas_take_dirty(f, &y0, &y1);
        g_font_sent[slot] = true;
        return send_atlas(slot, f, 0, text_shaper_atlas_used_rows(f));
    }
    if (text_shaper_atlas_take_dirty(f, &y0, &y1)) return send_atlas(slot, f, y0, y1);
    return true;
}

static bool send_image(int handle) {
    if (handle < 0 || handle >= GPU_MAX_IMAGES) return true;
    ImageCache *cache = display_list_get_image_cache();
    uint64_t gen = cache ? image_cache_generation(cache, handle) : 0;
    if (!gen || gen == g_image_gen[handle]) return true;
    int w = 0, h = 0, ch = 0;
    uint8_t *px = NULL;
    if (!image_cache_get(cache, handle, &w, &h, &ch, &px) || !px || ch != 4 || w <= 0 || h <= 0) return true;
    uint64_t bytes = (uint64_t)w * h * 4;
    if (bytes + sizeof(TgImage) > BIG_MAX) return true;
    TgImage t = { (uint32_t)handle, (uint32_t)w, (uint32_t)h };
    if (!big_send(&g_ch, TG_IMAGE, &t, sizeof(t), px, (uint32_t)bytes, NULL, 0, 5000)) return false;
    g_image_gen[handle] = gen;
    return true;
}

bool gpu_client_send(const DisplayList *dl, const GpuFrame *hdr) {
    if (!g_attached) return false;
    static bool slot_used[GPU_FONT_SLOTS];
    memset(slot_used, 0, sizeof(slot_used));
    for (int i = 0; i < dl->count; i++) {
        const DisplayListCmd *c = &dl->cmds[i];
        if (c->type == DL_GLYPH && c->u.glyph.font_slot < GPU_FONT_SLOTS && !slot_used[c->u.glyph.font_slot]) {
            slot_used[c->u.glyph.font_slot] = true;
            if (!send_font(c->u.glyph.font_slot)) return false;
        } else if (c->type == DL_IMAGE) {
            if (!send_image(c->u.image.image_handle)) return false;
        }
    }
    for (int i = 0; i < dl->mask_count; i++)
        if (dl->masks[i].kind == 1 && !send_image(dl->masks[i].image_handle)) return false;
    uint8_t *msg = NULL;
    uint32_t n = gpu_encode_frame(dl, hdr, &msg);
    if (!n) return false;
    bool ok = big_send(&g_ch, TG_FRAME, msg, n, NULL, 0, NULL, 0, 5000);
    free(msg);
    return ok;
}

int gpu_client_poll(uint32_t *frame) {
    if (!g_attached) return -1;
    int r;
    while ((r = big_recv(&g_ch, &g_rx)) > 0) {
        if (g_rx.type == GT_FRAME_DONE && g_rx.total == sizeof(GtFrameDone)) {
            GtFrameDone d;
            memcpy(&d, g_rx.buf, sizeof(d));
            *frame = d.frame;
            return 1;
        }
    }
    return r < 0 ? -1 : 0;
}

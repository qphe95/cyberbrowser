#ifndef GPU_INPUT_H
#define GPU_INPUT_H

/* What the GPU process takes from a tab besides the display list itself
 * (SANDBOX_DESIGN.md sections 5.2 and 8.7.3): its uploads of glyph atlas
 * bands and images, and the frame header's numbers the shim (gpu_main.c)
 * allocates and presents with.  Pure C, no library calls, bounded loops:
 * proved with Frama-C/WP (tools/prove.sh gpu_input) free of undefined
 * behavior for any bytes (P1), bounded (P7), accepting only messages of the
 * grammars below with every field it passes on in range.  It also decides
 * when a font slot or an image counts as uploaded in the tab's namespace
 * (GpuClientRes, which gpu_decode.c checks every record against): only once
 * its pixels are in place, so P5's "every glyph names an uploaded atlas"
 * means one that can be drawn.
 *
 *   TG_ATLAS   slot, width, height, y0, rows (little-endian uint32), then
 *              rows * width bytes: rows y0 .. y0 + rows - 1 of the slot's
 *              atlas; slot < GPU_FONT_SLOTS, 1 <= width, height <= 4096,
 *              y0 + rows <= height, nothing after
 *   TG_IMAGE   id, width, height, then width * height * 4 bytes (RGBA);
 *              id < GPU_MAX_IMAGES, 1 <= width, height <= 16384, nothing after
 *   TG_FRAME   the GpuFrame header (gpu_wire.h, ten uint32), at most
 *              GPU_MAX_CMDS records and GPU_MAX_MASKS masks; the rest is
 *              gpu_decode's */

#include <stdbool.h>
#include <stdint.h>
#include "proto.h"
#include "gpu_wire.h"
#include "gpu_decode.h"

#define GPU_ATLAS_MAX 4096
#define GPU_IMAGE_MAX 16384
#define GPU_CLIENT_BYTES (1024ull << 20)    /* a tab's resources, at most */
#define GPU_MAX_SCROLLS 64                  /* the UI's scrolls a tab hasn't seen, kept */

#define TG_ATLAS_BYTES 20u
#define TG_IMAGE_BYTES 12u

/* The CPU copy of an atlas (w * h bytes) or an image (w * h * 4). */
typedef struct GpuPixels {
    uint8_t *px;                    /* NULL: none */
    uint32_t w, h;
} GpuPixels;

/* One of the UI's scrolls (UG_SCROLL) that the tab hadn't seen. */
typedef struct GpuScroll {
    uint32_t seq;
    int32_t dy;
} GpuScroll;

enum { GPU_UP_DROP = 0, GPU_UP_WRITE, GPU_UP_NEW };

/*@ logic integer g32{L}(uint8_t *p) = p[0] + p[1] * 0x100 + p[2] * 0x10000 + p[3] * 0x1000000;

    predicate atlas_ok{L}(TgAtlas *a, integer len) =
        a->slot < GPU_FONT_SLOTS && 1 <= a->width <= GPU_ATLAS_MAX && 1 <= a->height <= GPU_ATLAS_MAX &&
        a->y0 + a->rows <= a->height && len == TG_ATLAS_BYTES + a->rows * a->width;

    predicate image_ok{L}(TgImage *t, integer len) =
        t->id < GPU_MAX_IMAGES && 1 <= t->width <= GPU_IMAGE_MAX && 1 <= t->height <= GPU_IMAGE_MAX &&
        len == TG_IMAGE_BYTES + t->width * t->height * 4;
 */

/* TG_ATLAS's header into *a; false unless the message is of the grammar. */
/*@ requires \valid_read(m + (0 .. len - 1)) && \valid(a) && \separated(a, m + (0 .. len - 1));
    assigns *a;
    ensures \result <==> len >= TG_ATLAS_BYTES && g32(m) < GPU_FONT_SLOTS &&
        1 <= g32(m + 4) <= GPU_ATLAS_MAX && 1 <= g32(m + 8) <= GPU_ATLAS_MAX &&
        g32(m + 12) + g32(m + 16) <= g32(m + 8) && len == TG_ATLAS_BYTES + g32(m + 16) * g32(m + 4);
    ensures \result ==> atlas_ok(a, len) && a->slot == g32(m) && a->width == g32(m + 4) &&
        a->height == g32(m + 8) && a->y0 == g32(m + 12) && a->rows == g32(m + 16);
 */
bool gpu_atlas_parse(const uint8_t *m, uint32_t len, TgAtlas *a);

/* TG_IMAGE's header into *t; false unless the message is of the grammar. */
/*@ requires \valid_read(m + (0 .. len - 1)) && \valid(t) && \separated(t, m + (0 .. len - 1));
    assigns *t;
    ensures \result <==> len >= TG_IMAGE_BYTES && g32(m) < GPU_MAX_IMAGES &&
        1 <= g32(m + 4) <= GPU_IMAGE_MAX && 1 <= g32(m + 8) <= GPU_IMAGE_MAX &&
        len == TG_IMAGE_BYTES + g32(m + 4) * g32(m + 8) * 4;
    ensures \result ==> image_ok(t, len) && t->id == g32(m) && t->width == g32(m + 4) && t->height == g32(m + 8);
 */
bool gpu_image_parse(const uint8_t *m, uint32_t len, TgImage *t);

/* What to do with an upload of a w x h resource (bpp bytes a pixel) to a
 * slot or id now holding cur, whose mark in the namespace is *mark, *bytes
 * being what the tab's resources take: GPU_UP_WRITE into cur as it is;
 * GPU_UP_NEW, for which the mark is cleared and cur's bytes given back, so
 * the shim frees cur and allocates w x h (and adds them to *bytes if it
 * can); GPU_UP_DROP, past GPU_CLIENT_BYTES, nothing changed.  keep_old: an
 * atlas's limit counts the old pixels too (as M5's shim did), an image's
 * doesn't. */
/*@ requires \valid(mark) && \valid(bytes) && \valid_read(cur) && \separated(mark, bytes, cur);
    requires 1 <= w <= GPU_IMAGE_MAX && 1 <= h <= GPU_IMAGE_MAX && (bpp == 1 || bpp == 4);
    assigns *mark, *bytes;
    ensures GPU_UP_DROP <= \result <= GPU_UP_NEW;
    ensures \result == GPU_UP_WRITE <==> cur->px != \null && cur->w == w && cur->h == h;
    ensures \result != GPU_UP_NEW ==> *mark == \old(*mark) && *bytes == \old(*bytes);
    ensures \result == GPU_UP_NEW ==> *mark == 0 && *bytes <= \old(*bytes) &&
        *bytes + w * h * bpp <= GPU_CLIENT_BYTES;
 */
int gpu_upload_plan(uint8_t *mark, uint64_t *bytes, const GpuPixels *cur, uint32_t w, uint32_t h, uint32_t bpp,
                    bool keep_old);

/* Copy an atlas band (the message m, len bytes, whose header is a) into cur,
 * and mark the slot: only if cur holds that atlas's pixels (allocated and of
 * its size).  The shim's part is that cur->px has cur->w * cur->h bytes. */
/*@ requires \valid_read(m + (0 .. len - 1)) && \valid_read(a) && atlas_ok(a, len);
    requires \valid(ns) && \valid_read(cur);
    requires cur->px != \null ==> \valid(cur->px + (0 .. cur->w * cur->h - 1));
    requires \separated(ns, cur, a, m + (0 .. len - 1));
    requires cur->px != \null ==> \separated(cur->px + (0 .. cur->w * cur->h - 1), ns, cur, a, m + (0 .. len - 1));
    assigns ns->font[a->slot], cur->px[0 .. cur->w * cur->h - 1];
    ensures \result <==> cur->px != \null && cur->w == a->width && cur->h == a->height;
    ensures \result ==> ns->font[a->slot] == 1 &&
        \forall integer i; 0 <= i < a->rows * a->width ==> cur->px[a->y0 * a->width + i] == m[TG_ATLAS_BYTES + i];
    ensures !\result ==> ns->font[a->slot] == \old(ns->font[a->slot]);
 */
bool gpu_atlas_put(GpuClientRes *ns, const GpuPixels *cur, const TgAtlas *a, const uint8_t *m, uint32_t len);

/* The same for an image: all its pixels, then its id is marked. */
/*@ requires \valid_read(m + (0 .. len - 1)) && \valid_read(t) && image_ok(t, len);
    requires \valid(ns) && \valid_read(cur);
    requires cur->px != \null ==> \valid(cur->px + (0 .. cur->w * cur->h * 4 - 1));
    requires \separated(ns, cur, t, m + (0 .. len - 1));
    requires cur->px != \null ==> \separated(cur->px + (0 .. cur->w * cur->h * 4 - 1), ns, cur, t, m + (0 .. len - 1));
    assigns ns->image[t->id], cur->px[0 .. cur->w * cur->h * 4 - 1];
    ensures \result <==> cur->px != \null && cur->w == t->width && cur->h == t->height;
    ensures \result ==> ns->image[t->id] == 1 &&
        \forall integer i; 0 <= i < t->width * t->height * 4 ==> cur->px[i] == m[TG_IMAGE_BYTES + i];
    ensures !\result ==> ns->image[t->id] == \old(ns->image[t->id]);
 */
bool gpu_image_put(GpuClientRes *ns, const GpuPixels *cur, const TgImage *t, const uint8_t *m, uint32_t len);

/* TG_FRAME's header, its ten numbers as they come, into *f: false if the
 * message is shorter than the header or names more records or masks than a
 * frame may have (the shim allocates for them).  The rest is gpu_decode's. */
/*@ requires \valid_read(m + (0 .. len - 1)) && \valid(f) && \separated(f, m + (0 .. len - 1));
    assigns *f;
    ensures \result <==> len >= WIRE_FRAME_BYTES && g32(m + 12) <= GPU_MAX_CMDS && g32(m + 16) <= GPU_MAX_MASKS;
    ensures \result ==> f->frame == g32(m) && f->width == g32(m + 4) && f->height == g32(m + 8) &&
        f->ncmds == g32(m + 12) && f->nmasks == g32(m + 16) && f->origin_y == g32(m + 20) &&
        f->view_h == g32(m + 24) && f->scroll_y == g32(m + 28) && f->scroll_seq == g32(m + 32) &&
        f->content_h == g32(m + 36);
 */
bool gpu_frame_header(const uint8_t *m, uint32_t len, GpuFrame *f);

/* Forget the scrolls (n of them) a frame that has seen up to seq has seen:
 * those not after seq in the UI's numbering (mod 2^32).  The count left. */
/*@ requires n <= GPU_MAX_SCROLLS && \valid(s + (0 .. n - 1));
    assigns s[0 .. n - 1];
    ensures 0 <= \result <= n;
 */
uint32_t gpu_scrolls_seen(GpuScroll *s, uint32_t n, uint32_t seq);

/* Where the viewport lies now, in the page: as the last frame (at) says,
 * moved by the n scrolls the tab hadn't seen, clamped as the tab clamps. */
/*@ requires n <= GPU_MAX_SCROLLS && \valid_read(at) && \valid_read(s + (0 .. n - 1));
    assigns \nothing;
    ensures 0 <= \result <= (at->content_h > at->view_h ? at->content_h - at->view_h : 0);
 */
int64_t gpu_view_y(const GpuFrame *at, const GpuScroll *s, uint32_t n);

/* The row of the tab's layer at the top of its viewport now: what the shim
 * presents from (src_y), in [-GPU_MAX_H, GPU_MAX_H]. */
/*@ requires n <= GPU_MAX_SCROLLS && \valid_read(at) && \valid_read(s + (0 .. n - 1));
    assigns \nothing;
    ensures -GPU_MAX_H <= \result <= GPU_MAX_H;
 */
int32_t gpu_layer_row(const GpuFrame *at, const GpuScroll *s, uint32_t n);

#endif

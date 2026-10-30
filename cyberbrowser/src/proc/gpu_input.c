/* gpu_input.c - what the GPU process takes from a tab besides the display
 * list (gpu_input.h).  No library calls, no allocation, bounded loops:
 * proved with Frama-C/WP (tools/prove.sh gpu_input).  Integers are read
 * byte by byte, little-endian, as in gpu_decode.c. */
#include "gpu_input.h"

/* (Products only grow with their factors: the band's rows lie in the atlas.) */
/*@ lemma mul_le: \forall integer a, b, c; 0 <= a <= b && 0 <= c ==> a * c <= b * c; */

/*@ requires \valid_read(p + (0 .. 3));
    assigns \nothing;
    ensures \result == g32(p);
 */
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] + (uint32_t)p[1] * 0x100u + (uint32_t)p[2] * 0x10000u + (uint32_t)p[3] * 0x1000000u;
}

bool gpu_atlas_parse(const uint8_t *m, uint32_t len, TgAtlas *a) {
    if (len < TG_ATLAS_BYTES) return false;
    uint32_t slot = rd32(m), w = rd32(m + 4), h = rd32(m + 8), y0 = rd32(m + 12), rows = rd32(m + 16);
    if (slot >= GPU_FONT_SLOTS || w == 0 || h == 0 || w > GPU_ATLAS_MAX || h > GPU_ATLAS_MAX || y0 > h ||
        rows > h - y0 || (uint64_t)len - TG_ATLAS_BYTES != (uint64_t)rows * w)
        return false;
    a->slot = slot;
    a->width = w;
    a->height = h;
    a->y0 = y0;
    a->rows = rows;
    return true;
}

bool gpu_image_parse(const uint8_t *m, uint32_t len, TgImage *t) {
    if (len < TG_IMAGE_BYTES) return false;
    uint32_t id = rd32(m), w = rd32(m + 4), h = rd32(m + 8);
    if (id >= GPU_MAX_IMAGES || w == 0 || h == 0 || w > GPU_IMAGE_MAX || h > GPU_IMAGE_MAX ||
        (uint64_t)len - TG_IMAGE_BYTES != (uint64_t)w * h * 4)
        return false;
    t->id = id;
    t->width = w;
    t->height = h;
    return true;
}

int gpu_upload_plan(uint8_t *mark, uint64_t *bytes, const GpuPixels *cur, uint32_t w, uint32_t h, uint32_t bpp,
                    bool keep_old) {
    if (cur->px && cur->w == w && cur->h == h) return GPU_UP_WRITE;
    /*@ assert w * h <= GPU_IMAGE_MAX * h <= GPU_IMAGE_MAX * GPU_IMAGE_MAX; */
    uint64_t need = (uint64_t)w * h * bpp;
    /*@ assert need == w * h * bpp <= GPU_IMAGE_MAX * GPU_IMAGE_MAX * 4; */
    uint64_t old = cur->px ? (uint64_t)cur->w * cur->h * bpp : 0;
    uint64_t base = *bytes - (old < *bytes ? old : *bytes);
    if (need > GPU_CLIENT_BYTES || (keep_old ? *bytes : base) > GPU_CLIENT_BYTES - need) return GPU_UP_DROP;
    /* Unmarked before its pixels go: drawn from again only once put back. */
    *mark = 0;
    *bytes = base;
    return GPU_UP_NEW;
}

bool gpu_atlas_put(GpuClientRes *ns, const GpuPixels *cur, const TgAtlas *a, const uint8_t *m, uint32_t len) {
    (void)len;
    if (!cur->px || cur->w != a->width || cur->h != a->height) return false;
    uint64_t base = (uint64_t)a->y0 * a->width, n = (uint64_t)a->rows * a->width;
    /*@ assert base + n == (a->y0 + a->rows) * a->width; */
    /*@ assert (a->y0 + a->rows) * a->width <= a->height * a->width; */
    uint8_t *px = cur->px;
    /*@ loop invariant 0 <= i <= n;
        loop invariant \forall integer k; 0 <= k < i ==> px[base + k] == m[TG_ATLAS_BYTES + k];
        loop assigns i, px[base .. base + n - 1];
        loop variant n - i;
     */
    for (uint64_t i = 0; i < n; i++) px[base + i] = m[TG_ATLAS_BYTES + i];
    ns->font[a->slot] = 1;
    return true;
}

bool gpu_image_put(GpuClientRes *ns, const GpuPixels *cur, const TgImage *t, const uint8_t *m, uint32_t len) {
    (void)len;
    if (!cur->px || cur->w != t->width || cur->h != t->height) return false;
    uint64_t n = (uint64_t)t->width * t->height * 4;
    uint8_t *px = cur->px;
    /*@ loop invariant 0 <= i <= n;
        loop invariant \forall integer k; 0 <= k < i ==> px[k] == m[TG_IMAGE_BYTES + k];
        loop assigns i, px[0 .. n - 1];
        loop variant n - i;
     */
    for (uint64_t i = 0; i < n; i++) px[i] = m[TG_IMAGE_BYTES + i];
    ns->image[t->id] = 1;
    return true;
}

bool gpu_frame_header(const uint8_t *m, uint32_t len, GpuFrame *f) {
    if (len < WIRE_FRAME_BYTES) return false;
    uint32_t ncmds = rd32(m + 12), nmasks = rd32(m + 16);
    if (ncmds > GPU_MAX_CMDS || nmasks > GPU_MAX_MASKS) return false;
    f->frame = rd32(m);
    f->width = rd32(m + 4);
    f->height = rd32(m + 8);
    f->ncmds = ncmds;
    f->nmasks = nmasks;
    f->origin_y = rd32(m + 20);
    f->view_h = rd32(m + 24);
    f->scroll_y = rd32(m + 28);
    f->scroll_seq = rd32(m + 32);
    f->content_h = rd32(m + 36);
    return true;
}

uint32_t gpu_scrolls_seen(GpuScroll *s, uint32_t n, uint32_t seq) {
    uint32_t k = 0;
    /*@ loop invariant 0 <= k <= i <= n;
        loop assigns i, k, s[0 .. n - 1];
        loop variant n - i;
     */
    for (uint32_t i = 0; i < n; i++) {
        uint32_t d = s[i].seq - seq;        /* after seq: 1 .. 2^31 - 1 ahead */
        if (d != 0 && d < 0x80000000u) s[k++] = s[i];
    }
    return k;
}

int64_t gpu_view_y(const GpuFrame *at, const GpuScroll *s, uint32_t n) {
    int64_t max = at->content_h > at->view_h ? (int64_t)at->content_h - at->view_h : 0;
    int64_t y = at->scroll_y < max ? at->scroll_y : max;
    /*@ loop invariant 0 <= i <= n;
        loop invariant 0 <= y <= max;
        loop assigns i, y;
        loop variant n - i;
     */
    for (uint32_t i = 0; i < n; i++) {
        y += s[i].dy;
        y = y < 0 ? 0 : y > max ? max : y;
    }
    return y;
}

int32_t gpu_layer_row(const GpuFrame *at, const GpuScroll *s, uint32_t n) {
    int64_t r = gpu_view_y(at, s, n) - (int64_t)at->origin_y;
    return r < -GPU_MAX_H ? -GPU_MAX_H : r > GPU_MAX_H ? GPU_MAX_H : (int32_t)r;
}

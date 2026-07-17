/*
 * cyber_profile.cpp - lightweight phase profiler producing profile.json for
 * the flame-graph renderer.  See cyber_profile.h for usage.
 *
 * Events carry (tid, name, cat, ts_us, dur_us, parent_id).  Per-thread open
 * stacks give parent/child nesting.  Storage is a mutex-protected dynamic
 * array; event ids are assigned under the same lock, so begin/end from many
 * threads stay consistent.
 */
#include "cyber_profile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* Implementation (not just declarations) of stb_image_write lives here in
 * the library so both cyberbrowser.exe and flamegraph_viewer.exe link it;
 * stb_truetype's implementation stays in text_shaper.cpp. */
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include "stb_truetype.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#include <sys/syscall.h>
#endif

#define CP_MAX_DEPTH 64
#define CP_INITIAL_CAP 1024

typedef struct {
    int id;
    int parent;
    int kind;               /* 0 = span, 1 = instant */
    unsigned long long tid;
    double ts_us;
    double dur_us;
    char *name;             /* strdup'd; owned by the buffer, freed at process exit */
    const char *cat;
} CpEvent;

typedef struct {
    CpEvent *events;
    int count;
    int cap;
    int next_id;
#ifdef _WIN32
    CRITICAL_SECTION lock;
    int lock_init;
#else
    pthread_mutex_t lock;
    int lock_init;
#endif
} CpBuffer;

static CpBuffer g_cp = {0};
static int g_cp_enabled = -1;

/* Per-thread open-event stack (TLS). */
static __thread int cp_stack[CP_MAX_DEPTH];
static __thread int cp_depth = 0;

static double cp_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000000.0 + (double)ts.tv_nsec / 1000.0;
}

static void cp_lock_init_once(void) {
    if (g_cp.lock_init) return;
#ifdef _WIN32
    InitializeCriticalSection(&g_cp.lock);
#else
    pthread_mutex_init(&g_cp.lock, NULL);
#endif
    g_cp.lock_init = 1;
    g_cp.cap = CP_INITIAL_CAP;
    g_cp.events = (CpEvent *)malloc(sizeof(CpEvent) * (size_t)g_cp.cap);
    g_cp.count = 0;
    g_cp.next_id = 1;
}

static void cp_lock(void) {
    cp_lock_init_once();
#ifdef _WIN32
    EnterCriticalSection(&g_cp.lock);
#else
    pthread_mutex_lock(&g_cp.lock);
#endif
}

static void cp_unlock(void) {
#ifdef _WIN32
    LeaveCriticalSection(&g_cp.lock);
#else
    pthread_mutex_unlock(&g_cp.lock);
#endif
}

int cp_profile_enabled(void) {
    if (g_cp_enabled < 0) {
        const char *e = getenv("CYBER_PROFILE");
        g_cp_enabled = (e && e[0] && strcmp(e, "0") != 0) ? 1 : 0;
    }
    return g_cp_enabled;
}

static unsigned long long cp_tid(void) {
#ifdef _WIN32
    return (unsigned long long)GetCurrentThreadId();
#else
    return (unsigned long long)(uintptr_t)pthread_self();
#endif
}

void cp_profile_begin(const char *name, const char *cat) {
    if (!name) return;
    if (cp_depth >= CP_MAX_DEPTH) return;
    cp_lock();
    if (g_cp.count >= g_cp.cap) {
        int newcap = g_cp.cap * 2;
        CpEvent *ne = (CpEvent *)realloc(g_cp.events, sizeof(CpEvent) * (size_t)newcap);
        if (!ne) { cp_unlock(); return; }
        g_cp.events = ne;
        g_cp.cap = newcap;
    }
    int id = g_cp.next_id++;
    int parent = (cp_depth > 0) ? cp_stack[cp_depth - 1] : -1;
    CpEvent *e = &g_cp.events[g_cp.count++];
    e->id = id;
    e->parent = parent;
    e->kind = 0;
    e->tid = cp_tid();
    e->ts_us = cp_now_us();
    e->dur_us = 0.0;
    e->name = strdup(name);
    e->cat = cat ? cat : "main";
    cp_stack[cp_depth++] = id;
    cp_unlock();
}

void cp_profile_end(const char *name) {
    (void)name;
    if (cp_depth <= 0) return;
    int id = cp_stack[--cp_depth];
    double now = cp_now_us();
    cp_lock();
    /* Find the event by id (linear scan is fine: count is small). */
    for (int i = g_cp.count - 1; i >= 0; i--) {
        if (g_cp.events[i].id == id) {
            g_cp.events[i].dur_us = now - g_cp.events[i].ts_us;
            break;
        }
    }
    cp_unlock();
}

void cp_profile_instant(const char *name, const char *cat) {
    if (!name) return;
    cp_lock();
    if (g_cp.count >= g_cp.cap) {
        int newcap = g_cp.cap * 2;
        CpEvent *ne = (CpEvent *)realloc(g_cp.events, sizeof(CpEvent) * (size_t)newcap);
        if (!ne) { cp_unlock(); return; }
        g_cp.events = ne;
        g_cp.cap = newcap;
    }
    int parent = (cp_depth > 0) ? cp_stack[cp_depth - 1] : -1;
    CpEvent *e = &g_cp.events[g_cp.count++];
    e->id = g_cp.next_id++;
    e->parent = parent;
    e->kind = 1;
    e->tid = cp_tid();
    e->ts_us = cp_now_us();
    e->dur_us = 0.0;
    e->name = strdup(name);
    e->cat = cat ? cat : "main";
    cp_unlock();
}

void cp_profile_flush(const char *path) {
    if (!path) return;
    cp_lock();
    FILE *f = fopen(path, "w");
    if (!f) { cp_unlock(); return; }
    fprintf(f, "{\n  \"events\": [\n");
    for (int i = 0; i < g_cp.count; i++) {
        CpEvent *e = &g_cp.events[i];
        fprintf(f, "    {\"id\": %d, \"parent\": %d, \"tid\": %llu, \"ts\": %.1f, \"dur\": %.1f, \"name\": \"%s\", \"cat\": \"%s\"}%s\n",
                e->id, e->parent, e->tid, e->ts_us, e->dur_us, e->name, e->cat,
                (i + 1 < g_cp.count) ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
    cp_unlock();
}

/* ============================================================================
 * Flame graph rasterizer (stb_image_write + stb_truetype)
 *
 * Layout: one horizontal lane per thread (main thread on top), wall-clock
 * time on the x axis, bars stacked by parent/child depth.  Open spans (still
 * on a thread's stack at snapshot time) are extended to "now"; instant events
 * draw as thin vertical lines.
 * ==========================================================================*/

#define FG_WIDTH    1920
#define FG_BAR_H    14
#define FG_LANE_GAP 6
#define FG_GUTTER   130
#define FG_TOP      30
#define FG_RIGHT_PAD 10
#define FG_MAX_LANES 64

static unsigned int fg_hash(const char *s) {
    unsigned int h = 2166136261u;
    while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h;
}

static void fg_hsv2rgb(float h, float s, float v, int *ri, int *gi, int *bi) {
    float c = v * s;
    float hp = h / 60.0f;
    float xm = c * (1.0f - fabsf(fmodf(hp, 2.0f) - 1.0f));
    float r = 0, g = 0, b = 0;
    int seg = ((int)hp) % 6;
    if (seg == 0) { r = c; g = xm; }
    else if (seg == 1) { r = xm; g = c; }
    else if (seg == 2) { g = c; b = xm; }
    else if (seg == 3) { g = xm; b = c; }
    else if (seg == 4) { r = xm; b = c; }
    else { r = c; b = xm; }
    float m = v - c;
    *ri = (int)((r + m) * 255.0f);
    *gi = (int)((g + m) * 255.0f);
    *bi = (int)((b + m) * 255.0f);
}

static void fg_rect(unsigned char *img, int W, int H,
                    int x0, int y0, int x1, int y1, int r, int g, int b) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > W) x1 = W;
    if (y1 > H) y1 = H;
    for (int y = y0; y < y1; y++) {
        unsigned char *row = img + ((size_t)y * (size_t)W + (size_t)x0) * 3;
        for (int x = x0; x < x1; x++) {
            row[0] = (unsigned char)r;
            row[1] = (unsigned char)g;
            row[2] = (unsigned char)b;
            row += 3;
        }
    }
}

typedef struct {
    stbtt_bakedchar cdata[96];
    unsigned char bitmap[256 * 256];
    int loaded;
} FgFont;

static void fg_font_load(FgFont *fnt) {
    static const char *paths[] = {
        "cyberbrowser/third_party/fonts/Roboto-Regular.ttf",
        "third_party/fonts/Roboto-Regular.ttf",
        "../third_party/fonts/Roboto-Regular.ttf",
        "../../third_party/fonts/Roboto-Regular.ttf",
        NULL
    };
    memset(fnt, 0, sizeof(*fnt));
    for (int i = 0; paths[i]; i++) {
        FILE *f = fopen(paths[i], "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        unsigned char *ttf = (unsigned char *)malloc((size_t)sz);
        if (ttf && fread(ttf, 1, (size_t)sz, f) == (size_t)sz) {
            if (stbtt_BakeFontBitmap(ttf, 0, 11.0f, fnt->bitmap, 256, 256,
                                     32, 96, fnt->cdata) > 0) {
                fnt->loaded = 1;
            }
        }
        free(ttf);
        fclose(f);
        if (fnt->loaded) break;
    }
}

/* Draw text with alpha blending; stop at clip_x1.  (x, y) is the baseline. */
static void fg_text(unsigned char *img, int W, int H, const FgFont *fnt,
                    float x, float y, int clip_x1, const char *text,
                    int r, int g, int b) {
    if (!fnt->loaded || !text) return;
    for (const char *p = text; *p; p++) {
        if (*p < 32 || *p > 126) continue;
        stbtt_aligned_quad q;
        stbtt_GetBakedQuad((stbtt_bakedchar *)fnt->cdata, 256, 256, *p - 32,
                           &x, &y, &q, 1);
        if ((int)q.x1 > clip_x1) break;
        int sx0 = (int)(q.s0 * 256.0f), sy0 = (int)(q.t0 * 256.0f);
        for (int py = (int)q.y0; py < (int)q.y1; py++) {
            if (py < 0 || py >= H) continue;
            int sy = sy0 + (py - (int)q.y0);
            if (sy < 0 || sy >= 256) continue;
            for (int px = (int)q.x0; px < (int)q.x1; px++) {
                if (px < 0 || px >= W) continue;
                int sx = sx0 + (px - (int)q.x0);
                if (sx < 0 || sx >= 256) continue;
                int a = fnt->bitmap[sy * 256 + sx];
                if (a == 0) continue;
                unsigned char *pxp = img + ((size_t)py * (size_t)W + (size_t)px) * 3;
                pxp[0] = (unsigned char)((r * a + pxp[0] * (255 - a)) / 255);
                pxp[1] = (unsigned char)((g * a + pxp[1] * (255 - a)) / 255);
                pxp[2] = (unsigned char)((b * a + pxp[2] * (255 - a)) / 255);
            }
        }
    }
}

static int fg_find_index(const CpEvent *ev, int n, int id) {
    /* Ids are assigned sequentially from 1 in append order, so id-1 is the
     * usual slot; fall back to a scan if events were dropped (OOM path). */
    if (id >= 1 && id <= n && ev[id - 1].id == id) return id - 1;
    for (int i = 0; i < n; i++) if (ev[i].id == id) return i;
    return -1;
}

static int fg_depth(const CpEvent *ev, int n, int idx, int *depths, int guard) {
    if (depths[idx] >= 0) return depths[idx];
    if (guard > 256) { depths[idx] = 0; return 0; }
    int d = 0;
    if (ev[idx].parent > 0) {
        int pi = fg_find_index(ev, n, ev[idx].parent);
        if (pi >= 0 && pi != idx) d = fg_depth(ev, n, pi, depths, guard + 1) + 1;
    }
    depths[idx] = d;
    return d;
}

void cp_profile_write_flamegraph(const char *path) {
    if (!path) return;

    /* Snapshot under the lock; event name strings are never freed, so the
     * shallow copy stays valid after unlocking. */
    cp_lock();
    int n = g_cp.count;
    if (n <= 0) { cp_unlock(); return; }
    CpEvent *ev = (CpEvent *)malloc(sizeof(CpEvent) * (size_t)n);
    if (!ev) { cp_unlock(); return; }
    memcpy(ev, g_cp.events, sizeof(CpEvent) * (size_t)n);
    double now = cp_now_us();
    cp_unlock();

    /* Effective durations: open spans extend to the snapshot moment. */
    double t0 = 0.0, t1 = 0.0;
    for (int i = 0; i < n; i++) {
        if (ev[i].kind == 0 && ev[i].dur_us <= 0.0)
            ev[i].dur_us = now - ev[i].ts_us;
        if (i == 0 || ev[i].ts_us < t0) t0 = ev[i].ts_us;
        double end = ev[i].ts_us + ev[i].dur_us;
        if (i == 0 || end > t1) t1 = end;
    }
    if (t1 <= t0) t1 = t0 + 1.0;

    /* Main thread = the one that owns the "load-youtube" root span. */
    unsigned long long main_tid = ev[0].tid;
    for (int i = 0; i < n; i++) {
        if (strcmp(ev[i].name, "load-youtube") == 0) { main_tid = ev[i].tid; break; }
    }

    /* Lane order: main first, then other threads by busy time (descending). */
    unsigned long long lanes[FG_MAX_LANES];
    int nl = 0;
    lanes[nl++] = main_tid;
    for (int i = 0; i < n && nl < FG_MAX_LANES; i++) {
        unsigned long long t = ev[i].tid;
        int found = 0;
        for (int k = 0; k < nl; k++) if (lanes[k] == t) { found = 1; break; }
        if (!found) lanes[nl++] = t;
    }
    {
        double busy[FG_MAX_LANES];
        for (int k = 0; k < nl; k++) busy[k] = 0.0;
        for (int i = 0; i < n; i++) {
            for (int k = 0; k < nl; k++)
                if (lanes[k] == ev[i].tid) { busy[k] += ev[i].dur_us; break; }
        }
        for (int a = 1; a < nl; a++)
            for (int b = a + 1; b < nl; b++)
                if (busy[b] > busy[a]) {
                    double tb = busy[a]; busy[a] = busy[b]; busy[b] = tb;
                    unsigned long long tl = lanes[a]; lanes[a] = lanes[b]; lanes[b] = tl;
                }
    }

    /* Depths and per-lane geometry. */
    int *depths = (int *)malloc(sizeof(int) * (size_t)n);
    int lane_maxdepth[FG_MAX_LANES];
    int lane_y[FG_MAX_LANES];
    if (!depths) { free(ev); return; }
    for (int i = 0; i < n; i++) depths[i] = -1;
    for (int k = 0; k < nl; k++) lane_maxdepth[k] = 0;
    for (int i = 0; i < n; i++) {
        int d = fg_depth(ev, n, i, depths, 0);
        int lane = 0;
        for (int k = 0; k < nl; k++) if (lanes[k] == ev[i].tid) { lane = k; break; }
        if (d > lane_maxdepth[lane]) lane_maxdepth[lane] = d;
    }
    int y = FG_TOP;
    for (int k = 0; k < nl; k++) {
        lane_y[k] = y;
        y += (lane_maxdepth[k] + 1) * FG_BAR_H + FG_LANE_GAP;
    }
    int H = y + 8;
    int W = FG_WIDTH;

    unsigned char *img = (unsigned char *)malloc((size_t)W * (size_t)H * 3);
    if (!img) { free(depths); free(ev); return; }
    memset(img, 18, (size_t)W * (size_t)H * 3);

    FgFont fnt;
    fg_font_load(&fnt);

    double span = t1 - t0;
    double px_per_us = (double)(W - FG_GUTTER - FG_RIGHT_PAD) / span;

    /* Lane separators + labels. */
    for (int k = 0; k < nl; k++) {
        int lh = (lane_maxdepth[k] + 1) * FG_BAR_H;
        fg_rect(img, W, H, 0, lane_y[k], W, lane_y[k] + lh, 26, 26, 32);
        fg_rect(img, W, H, 0, lane_y[k] + lh, W, lane_y[k] + lh + 1, 50, 50, 60);
        char label[64];
        snprintf(label, sizeof(label), "%llu%s", lanes[k], (k == 0) ? " (main)" : "");
        fg_text(img, W, H, &fnt, 6.0f, (float)(lane_y[k] + 12), FG_GUTTER - 4,
                label, 200, 200, 210);
    }

    /* Time axis ticks (10 divisions). */
    for (int i = 0; i <= 10; i++) {
        double frac = (double)i / 10.0;
        int x = FG_GUTTER + (int)(frac * (double)(W - FG_GUTTER - FG_RIGHT_PAD));
        fg_rect(img, W, H, x, FG_TOP - 14, x + 1, FG_TOP - 4, 120, 120, 130);
        char tl[32];
        double tsec = frac * span / 1000000.0;
        snprintf(tl, sizeof(tl), "%.2fs", tsec);
        fg_text(img, W, H, &fnt, (float)(x + 2), (float)(FG_TOP - 6), W - 2,
                tl, 150, 150, 160);
    }

    /* Bars. */
    for (int i = 0; i < n; i++) {
        int lane = 0;
        for (int k = 0; k < nl; k++) if (lanes[k] == ev[i].tid) { lane = k; break; }
        int x0 = FG_GUTTER + (int)((ev[i].ts_us - t0) * px_per_us);
        int x1 = FG_GUTTER + (int)((ev[i].ts_us + ev[i].dur_us - t0) * px_per_us);
        if (x1 <= x0) x1 = x0 + 1;
        int y0 = lane_y[lane] + depths[i] * FG_BAR_H;
        int y1 = y0 + FG_BAR_H - 1;

        if (ev[i].kind == 1) {
            fg_rect(img, W, H, x0, y0, x0 + 1, y1, 240, 240, 250);
            continue;
        }

        unsigned int hc = fg_hash(ev[i].cat);
        unsigned int hn = fg_hash(ev[i].name);
        float hue = (float)(hc % 360);
        float sat = 0.55f + 0.15f * (float)((hn >> 8) & 0xff) / 255.0f;
        float val = 0.70f + 0.20f * (float)(hn & 0xff) / 255.0f;
        int r, g, b;
        fg_hsv2rgb(hue, sat, val, &r, &g, &b);
        fg_rect(img, W, H, x0, y0, x1, y1, r, g, b);

        int lum = (r * 299 + g * 587 + b * 114) / 1000;
        int tr = lum > 140 ? 20 : 240;
        int tg = lum > 140 ? 20 : 240;
        int tb = lum > 140 ? 24 : 245;
        if (x1 - x0 > 26)
            fg_text(img, W, H, &fnt, (float)(x0 + 3), (float)(y1 - 3), x1 - 2,
                    ev[i].name, tr, tg, tb);
    }

    stbi_write_png(path, W, H, 3, img, W * 3);
    free(img);
    free(depths);
    free(ev);
}

/* webgpu_main.c - cb_webgpu.exe: a site's web-GPU process (SANDBOX_DESIGN.md
 * section 5.7): WebGL on ANGLE (webgl_backend.c) and WebGPU on wgpu-native
 * (webgpu_wgpu.c), for that site's tabs only.
 *
 * Started by the coordinator, one per site that uses them:
 *   cb_webgpu.exe --chan S E0 E1 --site HEX
 *
 * The coordinator grants it a channel per tab of its site (GRANT_TAB,
 * badged; a badge of another site is refused).  Each tab's contexts are
 * its own; its calls come as records, checked here and by the library
 * (ANGLE in WebGL compatibility mode, wgpu's validation), and only results
 * the API defines go back: query answers and the presented canvases'
 * pixels.  The libraries load from webgpu\ next to the executable, by full
 * path, the first time a tab asks; the block list (gpu_blocklist.c) decides
 * then whether the device may be used at all.
 *
 * With no tab to serve for a while (IDLE_MS), it exits; the coordinator
 * starts another when a tab of the site asks again.  That also ends what
 * the libraries keep after a tab is gone: wgpu-native holds on to a
 * device's memory in cases a page can bring about (a command buffer never
 * submitted), so a long-lived process would in time make no more devices.
 *
 * CYBER_WEBGPU_CRASH_TEST=N (tests): exit after N presented frames, as a
 * crashing driver would.  CYBER_WEBGPU_IDLE_MS: the idle time (tests). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "chan.h"
#include "bigmsg.h"
#include "proto.h"
#include "spawn.h"
#include "site.h"
#include "gpu_blocklist.h"
#include "webgl_backend.h"
#include "webgpu_wgpu.h"

#define MAX_CLIENTS 60                  /* + the control channel: under 64 waits */
#define IDLE_MS 30000                   /* with no tabs this long, it exits */
#define MAX_CONTEXTS 64                 /* WebGL contexts per tab */

typedef struct Client {
    uint32_t tab;
    Chan chan;                          /* side 1: the web-GPU process */
    BigRx rx;
    WebglCtx *gl[MAX_CONTEXTS];
    WgpuClient *gpu;                    /* its WebGPU objects, made on first use */
} Client;

static Chan g_ctl;
static char g_site[SITE_MAX];
static Client *g_clients[MAX_CLIENTS];
static wchar_t g_lib_dir[MAX_PATH];
static uint8_t *g_out;                  /* query results, reused */
static uint32_t g_out_cap;
static int g_frames, g_crash_after;

/* ---- the libraries and the block list ---------------------------------------- */

static int g_gl_state;                  /* 0 not tried, 1 usable, -1 not */
static char g_gl_why[256];

/* ANGLE's renderer string: "ANGLE (NVIDIA, NVIDIA GeForce RTX 4070 SUPER
 * (0x00002783) Direct3D11 vs_5_0 ps_5_0, D3D11-32.0.16.1088)". */
static GpuDevice angle_device(const char *r, char *driver, size_t cap) {
    GpuDevice d = { 0, 0, r, "" };
    if (strstr(r, "NVIDIA")) d.vendor = 0x10de;
    else if (strstr(r, "Intel")) d.vendor = 0x8086;
    else if (strstr(r, "AMD") || strstr(r, "Radeon")) d.vendor = 0x1002;
    else if (strstr(r, "Microsoft") || strstr(r, "SwiftShader")) d.vendor = 0x1414;
    const char *dev = strstr(r, "(0x");
    if (dev) d.device = (uint32_t)strtoul(dev + 3, NULL, 16);
    const char *drv = strstr(r, "D3D11-");
    driver[0] = 0;
    if (drv) {
        snprintf(driver, cap, "%s", drv + 6);
        driver[strcspn(driver, ")")] = 0;
    }
    d.driver = driver;
    return d;
}

static bool gl_usable(void) {
    if (g_gl_state) return g_gl_state > 0;
    g_gl_state = -1;
    if (!webgl_init(g_lib_dir, g_gl_why, sizeof(g_gl_why))) {
        fprintf(stderr, "[webgpu] WebGL unavailable: %s\n", g_gl_why);
        return false;
    }
    char driver[64];
    GpuDevice d = angle_device(webgl_renderer(), driver, sizeof(driver));
    GpuVerdict v = gpu_blocklist_check(&d);
    fprintf(stderr, "[webgpu] WebGL on %s (driver %s)%s%s\n", webgl_renderer(), driver[0] ? driver : "?",
            v.blocked ? ": blocked, " : "", v.blocked ? v.why : "");
    if (v.blocked) {
        snprintf(g_gl_why, sizeof(g_gl_why), "blocked: %s", v.why);
        return false;
    }
    g_gl_state = 1;
    return true;
}

/* ---- a tab's requests ---------------------------------------------------------------- */

static void reply(Client *c, uint16_t type, const void *a, uint32_t alen, const void *b, uint32_t blen) {
    big_send(&c->chan, type, a, alen, b, blen, NULL, 0, 5000);
}

static WebglCtx **gl_slot(Client *c, uint32_t ctx, bool free_one) {
    for (int i = 0; i < MAX_CONTEXTS; i++)
        if (c->gl[i] && c->gl[i]->id == ctx) return &c->gl[i];
    if (!free_one) return NULL;
    for (int i = 0; i < MAX_CONTEXTS; i++)
        if (!c->gl[i]) return &c->gl[i];
    return NULL;
}

static void gl_create(Client *c, const uint8_t *m, uint32_t len) {
    TwGlCreate q;
    if (len != sizeof(q)) return;
    memcpy(&q, m, sizeof(q));
    WtCreated r = { q.ctx, 0 };
    const char *why = "";
    WebglCtx **slot = q.ctx ? gl_slot(c, q.ctx, false) : NULL;
    if (!q.ctx || slot) why = "bad context id";
    else if (!gl_usable()) why = g_gl_why;
    else if (!(slot = gl_slot(c, 0, true))) why = "too many contexts";
    else if (!(*slot = webgl_create(q.ctx, (int)q.version, q.width, q.height, q.flags))) why = "could not make a context";
    else r.ok = 1;
    reply(c, WT_GL_CREATED, &r, sizeof(r), why, (uint32_t)strlen(why));
}

static void gl_run(Client *c, const uint8_t *m, uint32_t len) {
    TwGlRun q;
    if (len < sizeof(q)) return;
    memcpy(&q, m, sizeof(q));
    WebglCtx **slot = gl_slot(c, q.ctx, false);
    uint32_t out_len = 0;
    WtResult r = { q.ctx, q.seq, 0 };
    if (slot && !(*slot)->lost) r.ok = webgl_run(*slot, m + sizeof(q), len - (uint32_t)sizeof(q), &g_out, &out_len, &g_out_cap);
    if (q.reply) reply(c, WT_GL_RESULT, &r, sizeof(r), g_out, r.ok ? out_len : 0);
    if (slot && (*slot)->lost) {
        TwContext x = { q.ctx };
        reply(c, WT_LOST, &x, sizeof(x), NULL, 0);
    }
}

static void present(Client *c, const uint8_t *m, uint32_t len) {
    TwPresent q;
    if (len != sizeof(q)) return;
    memcpy(&q, m, sizeof(q));
    WtFrame f = { q.ctx, q.seq, 0, 0 };
    static uint8_t *px;
    static size_t px_cap;
    WebglCtx **slot = gl_slot(c, q.ctx, false);
    if (slot) {
        size_t need = (size_t)(*slot)->width * (*slot)->height * 4;
        if (need > px_cap) {
            free(px);
            px = (uint8_t *)malloc(need);
            px_cap = px ? need : 0;
        }
        if (px && webgl_read_frame(*slot, px)) {
            f.width = (*slot)->width;
            f.height = (*slot)->height;
        }
    } else if (c->gpu) {
        wgpu_present(c->gpu, q.ctx, &px, &px_cap, &f.width, &f.height);
    }
    reply(c, WT_FRAME, &f, sizeof(f), px, f.width * f.height * 4);
    if (g_crash_after > 0 && ++g_frames >= g_crash_after) {
        fprintf(stderr, "[webgpu] CYBER_WEBGPU_CRASH_TEST: ending after %d frames\n", g_frames);
        fflush(stderr);
        ExitProcess(3);
    }
}

static void client_message(Client *c) {
    const uint8_t *m = c->rx.buf;
    uint32_t len = c->rx.total;
    switch (c->rx.type) {
    case TW_GL_CREATE: gl_create(c, m, len); break;
    case TW_GL_RESIZE: {
        TwGlResize q;
        if (len != sizeof(q)) break;
        memcpy(&q, m, sizeof(q));
        WebglCtx **slot = gl_slot(c, q.ctx, false);
        if (slot) webgl_resize(*slot, q.width, q.height);
        else if (c->gpu) wgpu_resize(c->gpu, q.ctx, q.width, q.height);
        break;
    }
    case TW_GL_RUN: gl_run(c, m, len); break;
    case TW_PRESENT: present(c, m, len); break;
    case TW_DESTROY: {
        TwContext q;
        if (len != sizeof(q)) break;
        memcpy(&q, m, sizeof(q));
        WebglCtx **slot = gl_slot(c, q.ctx, false);
        if (slot) { webgl_destroy(*slot); *slot = NULL; }
        break;
    }
    case TW_GPU_RUN: {
        TwGpuRun q;
        if (len < sizeof(q)) break;
        memcpy(&q, m, sizeof(q));
        if (!c->gpu) c->gpu = wgpu_client_new(g_lib_dir);
        uint32_t out_len = 0;
        WtResult r = { 0, q.seq, 0 };
        if (c->gpu) r.ok = wgpu_run(c->gpu, m + sizeof(q), len - (uint32_t)sizeof(q), &g_out, &out_len, &g_out_cap);
        if (q.reply) reply(c, WT_GPU_RESULT, &r, sizeof(r), g_out, r.ok ? out_len : 0);
        break;
    }
    default: break;
    }
}

/* ---- the coordinator's grants ---------------------------------------------------------- */

static void client_free(Client *c) {
    for (int i = 0; i < MAX_CONTEXTS; i++) webgl_destroy(c->gl[i]);
    if (c->gpu) wgpu_client_free(c->gpu);
    big_free(&c->rx);
    chan_close(&c->chan);
    free(c);
}

static void grant(const CsGrant *g) {
    HANDLE s = (HANDLE)(uintptr_t)g->chan_section, e0 = (HANDLE)(uintptr_t)g->chan_ev0,
           e1 = (HANDLE)(uintptr_t)g->chan_ev1;
    int slot = -1;
    for (int i = 0; i < MAX_CLIENTS && slot < 0; i++)
        if (!g_clients[i]) slot = i;
    /* Only tabs of its own site. */
    bool own = g->kind == GRANT_TAB && !g->site[PROTO_SITE_MAX - 1] && g_site[0] && !strcmp(g->site, g_site);
    Client *c = own && slot >= 0 ? (Client *)calloc(1, sizeof(Client)) : NULL;
    if (!c || !chan_attach(&c->chan, s, e0, e1, 1)) {
        if (!own) fprintf(stderr, "[webgpu] refused a channel badged %.64s\n", g->site);
        free(c);
        CloseHandle(s); CloseHandle(e0); CloseHandle(e1);
        return;
    }
    c->tab = g->client;
    c->rx.limit = BIG_MAX;  /* WebGL/WebGPU uploads (bufferData, texImage2D, writeBuffer) have no smaller cap */
    g_clients[slot] = c;
    fprintf(stderr, "[webgpu] serving tab %u\n", c->tab);
}

static void revoke(uint32_t tab) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        Client *c = g_clients[i];
        if (!c || c->tab != tab) continue;
        g_clients[i] = NULL;
        client_free(c);
        fprintf(stderr, "[webgpu] stopped serving tab %u\n", tab);
    }
}

int main(int argc, char **argv) {
    if (argc < 7 || strcmp(argv[1], "--chan") || strcmp(argv[5], "--site") ||
        !chan_attach(&g_ctl, spawn_handle_from_arg(argv[2]), spawn_handle_from_arg(argv[3]),
                     spawn_handle_from_arg(argv[4]), 1)) {
        fprintf(stderr, "cb_webgpu: bad arguments\n");
        return 1;
    }
    size_t n = strlen(argv[6]) / 2;
    for (size_t k = 0; k < n && k < SITE_MAX - 1; k++) {
        unsigned v = 0;
        sscanf(argv[6] + 2 * k, "%2x", &v);
        g_site[k] = (char)v;
    }
    if (!site_label_ok(g_site) || !g_site[0]) {
        fprintf(stderr, "cb_webgpu: bad site\n");
        return 1;
    }
    _snwprintf(g_lib_dir, MAX_PATH, L"%lswebgpu", spawn_exe_dir());
    const char *crash = getenv("CYBER_WEBGPU_CRASH_TEST");
    g_crash_after = crash ? atoi(crash) : 0;
    fprintf(stderr, "[webgpu] for %s\n", g_site);
    const char *idle_env = getenv("CYBER_WEBGPU_IDLE_MS");
    unsigned long long idle_ms = idle_env ? strtoull(idle_env, NULL, 10) : IDLE_MS;
    unsigned long long idle_since = GetTickCount64();
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
        for (int i = 0; i < MAX_CLIENTS; i++) {
            Client *c = g_clients[i];
            int got;
            while (c && (got = big_recv(&c->chan, &c->rx)) > 0) client_message(c);
            if (c && got < 0) {
                fprintf(stderr, "[webgpu] tab %u broke its channel\n", c->tab);
                revoke(c->tab);
            }
        }
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (g_clients[i] && g_clients[i]->gpu) wgpu_poll(g_clients[i]->gpu);
        int serving = 0;
        for (int i = 0; i < MAX_CLIENTS; i++) serving += g_clients[i] != NULL;
        unsigned long long now = GetTickCount64();
        if (serving) idle_since = now;
        else if (now - idle_since >= idle_ms) {
            fprintf(stderr, "[webgpu] no tabs for %llu ms: exiting\n", idle_ms);
            fflush(stderr);
            return 0;
        }
        fflush(stderr);
        HANDLE waits[MAX_CLIENTS + 1];
        int k = 0;
        bool ready = !chan_arm(&g_ctl);
        waits[k++] = chan_event(&g_ctl);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!g_clients[i]) continue;
            if (!chan_arm(&g_clients[i]->chan)) ready = true;
            waits[k++] = chan_event(&g_clients[i]->chan);
        }
        /* WebGPU's callbacks (a buffer mapped, work done) come while
         * polled: wake every few ms while any is pending. */
        DWORD ms = serving ? INFINITE : (DWORD)(idle_ms - (now - idle_since) + 1);
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (g_clients[i] && g_clients[i]->gpu && wgpu_pending(g_clients[i]->gpu)) ms = 2;
        if (!ready) WaitForMultipleObjects((DWORD)k, waits, FALSE, ms);
        chan_disarm(&g_ctl);
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (g_clients[i]) chan_disarm(&g_clients[i]->chan);
    }
}

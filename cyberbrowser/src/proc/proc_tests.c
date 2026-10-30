/* proc_tests.c - tests of the multi-process browser's pure parts: sites,
 * the storage service's rules, the coordinator's core and plans, the relay's
 * core, the GPU process's decoder, the network policy and decoder, the
 * web-GPU process's decoders.
 *   proc_tests.exe        prints failures, exits 1 if any */
#include <stdio.h>
#include <string.h>

#include "site.h"
#include "store_core.h"
#include "coord_core.h"
#include "coord_plan.h"
#include "loop_core.h"
#include "gpu_decode.h"
#include "net_policy.h"
#include "bigmsg.h"
#include "webgl_decode.h"
#include "webgpu_decode.h"
#include "net_decode.h"
#include "gpu_input.h"
#include "ui_core.h"

static int g_fail, g_run;

#define CHECK(cond) do { g_run++; if (!(cond)) { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static const char *site(const char *url) {
    static char out[SITE_MAX];
    char scheme[8], host[SITE_HOST_MAX + 1];
    if (!site_url_host((const uint8_t *)url, (uint32_t)strlen(url), scheme, sizeof(scheme), host, sizeof(host)))
        return "(no host)";
    if (!site_of(scheme, host, out)) return "(no site)";
    return out;
}

static void test_sites(void) {
    CHECK(!strcmp(site("https://www.youtube.com/watch?v=x"), "https+youtube.com"));
    CHECK(!strcmp(site("https://en.wikipedia.org/wiki/United_States"), "https+wikipedia.org"));
    CHECK(!strcmp(site("https://www.bbc.co.uk/news"), "https+bbc.co.uk"));
    CHECK(!strcmp(site("https://a.b.c.bbc.co.uk/"), "https+bbc.co.uk"));
    CHECK(!strcmp(site("https://user.github.io/x"), "https+user.github.io"));
    CHECK(!strcmp(site("https://other.github.io/"), "https+other.github.io"));
    CHECK(!strcmp(site("http://example.com"), "http+example.com"));
    CHECK(!strcmp(site("https://EXAMPLE.Com:8443/a"), "https+example.com"));
    CHECK(!strcmp(site("https://user:pw@login.example.com/"), "https+example.com"));
    CHECK(!strcmp(site("https://example.com./"), "https+example.com"));
    CHECK(!strcmp(site("http://localhost:8080/"), "http+localhost"));
    CHECK(!strcmp(site("http://127.0.0.1:3000/"), "http+127.0.0.1"));
    CHECK(!strcmp(site("https://co.uk/"), "https+co.uk"));
    /* The whole list: private suffixes, punycode, wildcards and exceptions. */
    CHECK(!strcmp(site("https://shop-a.myshopify.com/"), "https+shop-a.myshopify.com"));
    CHECK(!strcmp(site("https://www.example.xn--55qx5d.cn/"), "https+example.xn--55qx5d.cn"));
    CHECK(!strcmp(site("https://a.b.c.compute.amazonaws.com/"), "https+b.c.compute.amazonaws.com"));
    CHECK(!strcmp(site("https://x.foo.ck/"), "https+x.foo.ck"));
    CHECK(!strcmp(site("https://foo.ck/"), "https+foo.ck"));
    CHECK(!strcmp(site("https://www.ck/"), "https+www.ck"));
    CHECK(!strcmp(site("https://a.www.ck/"), "https+www.ck"));
    CHECK(!strcmp(site("https://a.city.kobe.jp/"), "https+city.kobe.jp"));
    CHECK(!strcmp(site("https://a.b.kobe.jp/"), "https+a.b.kobe.jp"));
    CHECK(!strcmp(site("https://www.example.zzzz/"), "https+example.zzzz"));
    CHECK(!strcmp(site("https://a.b.c.d.e.f.g.example.com/"), "https+example.com"));
    CHECK(!strcmp(site("https://[::1]/"), "(no host)"));
    CHECK(!strcmp(site("ftp://example.com/"), "(no host)"));
    CHECK(!strcmp(site("https://exa_mple.com/"), "(no host)"));
    CHECK(!strcmp(site("https:///path"), "(no host)"));
    CHECK(!strcmp(site("https://a..b/"), "(no host)"));
    CHECK(!strcmp(site("https://example.com:80x/"), "(no host)"));
    CHECK(site_label_ok("https+youtube.com"));
    CHECK(!site_label_ok("https+"));
    CHECK(!site_label_ok("https+a+b"));
    CHECK(!site_label_ok("https+../x"));
    CHECK(!site_label_ok("https+A.com"));
}

static bool rel_ok(const char *p) { return store_rel_path_ok(p, (unsigned)strlen(p)); }

static void test_store(void) {
    CHECK(rel_ok("cookies"));
    CHECK(rel_ok("local_storage/https_www.youtube.com_443"));
    CHECK(rel_ok("a/b/c/d/e/f/g/h"));
    CHECK(!rel_ok("a/b/c/d/e/f/g/h/i"));
    CHECK(!rel_ok(""));
    CHECK(!rel_ok("."));
    CHECK(!rel_ok(".."));
    CHECK(!rel_ok("../x"));
    CHECK(!rel_ok("a/../x"));
    CHECK(!rel_ok(".hidden"));
    CHECK(!rel_ok("a//b"));
    CHECK(!rel_ok("/a"));
    CHECK(!rel_ok("a/"));
    CHECK(!rel_ok("a\\b"));
    CHECK(!rel_ok("C:x"));
    CHECK(!rel_ok("a:stream"));
    CHECK(!rel_ok("Upper"));
    CHECK(!rel_ok("con ~"));
    char long_c[70];
    memset(long_c, 'a', 65);
    long_c[65] = 0;
    CHECK(!rel_ok(long_c));
    long_c[64] = 0;
    CHECK(rel_ok(long_c));
    char out[STORE_PATH_MAX + SITE_MAX + 2];
    CHECK(store_join("https+youtube.com", "cookies", 7, out, sizeof(out)) && !strcmp(out, "https+youtube.com/cookies"));
    CHECK(!store_join("https+../x", "cookies", 7, out, sizeof(out)));
    CHECK(!store_join("https+youtube.com", "cookies", 7, out, 10));
    CHECK(store_allowed("https+a.com", "https+a.com", "x", 1));
    CHECK(!store_allowed("https+a.com", "https+b.com", "x", 1));
    CHECK(store_allowed(NULL, "https+b.com", "cookies", 7));
    CHECK(!store_allowed(NULL, "https+b.com", "local_storage/x", 15));
    CHECK(!store_allowed("https+a.com", "https+a.com", "cookies", 7));          /* HttpOnly cookies */
    CHECK(store_allowed("https+a.com", "https+a.com", "x/cookies", 9));
    /* The HTTP cache: the network process's, never a tab's; the bytecode
     * cache: the tab's. */
    CHECK(store_allowed(NULL, "https+a.com", "cache/http/0123abcd", 19));
    CHECK(!store_allowed("https+a.com", "https+a.com", "cache/http/0123abcd", 19));
    CHECK(!store_allowed(NULL, "https+a.com", "cache/code/0123abcd", 19));
    CHECK(store_allowed("https+a.com", "https+a.com", "cache/code/0123abcd", 19));
    CHECK(!store_allowed(NULL, "https+a.com", "cache/http", 10));
    CHECK(!rel_ok("cookies."));                                                    /* = "cookies" on Windows */
    CHECK(!rel_ok("a./b"));
    CHECK(rel_ok("a.b/c.d"));
    CHECK(!rel_ok("nul"));
    CHECK(!rel_ok("x/con.txt"));
    CHECK(!rel_ok("com1"));
    CHECK(!rel_ok("lpt9.log"));
    CHECK(rel_ok("console"));
    CHECK(rel_ok("com10"));
    CHECK(rel_ok("nully"));
    CHECK(store_quota_ok(0, 0, 100));
    CHECK(store_quota_ok(STORE_QUOTA_BYTES, 100, 100));
    CHECK(!store_quota_ok(STORE_QUOTA_BYTES, 100, 101));
    CHECK(!store_quota_ok(10, 11, 1));
    CHECK(!store_quota_ok(0, 0, STORE_QUOTA_BYTES + 1));
}

static CoordState g_s;
static CoordActions g_a;

static void ui(uint16_t type, const void *data, uint32_t len) {
    CoordEvent e;
    memset(&e, 0, sizeof(e));
    e.kind = CE_UI_MESSAGE;
    e.type = type;
    e.data = (const uint8_t *)data;
    e.len = len;
    coord_step(&g_s, &e, &g_a);
}

static void open_tab(uint32_t tab, const char *url) {
    uint8_t msg[sizeof(UcOpenTab) + PROTO_URL_MAX];
    UcOpenTab o = { tab, 800, 600 };
    memcpy(msg, &o, sizeof(o));
    memcpy(msg + sizeof(o), url, strlen(url));
    ui(UC_OPEN_TAB, msg, (uint32_t)(sizeof(o) + strlen(url)));
}

static void test_coord(void) {
    coord_init(&g_s);
    open_tab(1, "https://www.youtube.com/");
    CHECK(g_a.n == 1 && g_a.a[0].kind == CA_START_TAB && !strcmp(g_a.a[0].site, "https+youtube.com"));
    open_tab(1, "https://example.com/");                    /* the same id twice */
    CHECK(g_a.n == 1 && g_a.a[0].kind == CA_SEND_UI && g_a.a[0].type == CU_TAB_FAILED);
    open_tab(2, "file:///c:/windows/win.ini");
    CHECK(g_a.n == 1 && g_a.a[0].kind == CA_SEND_UI && g_a.a[0].type == CU_TAB_FAILED);
    open_tab(3, "about:blank");
    CHECK(g_a.n == 1 && g_a.a[0].kind == CA_START_TAB && g_a.a[0].site[0] == 0);   /* opaque */
    ui(UC_OPEN_TAB, "\x01\x02", 2);                         /* too short */
    CHECK(g_a.n == 0);
    UcTab c = { 1 };
    ui(UC_CLOSE_TAB, &c, sizeof(c));
    CHECK(g_a.n == 1 && g_a.a[0].kind == CA_KILL_TAB);
    CoordEvent e;
    memset(&e, 0, sizeof(e));
    e.kind = CE_TAB_EXITED;
    e.slot = g_a.a[0].slot;
    coord_step(&g_s, &e, &g_a);
    CHECK(g_a.n == 1 && g_a.a[0].kind == CA_TAB_GONE);      /* closed: the UI isn't told, the servers are */
    e.slot = 1000;
    coord_step(&g_s, &e, &g_a);
    CHECK(g_a.n == 0);
}

static void step(CoordEventKind kind, int slot) {
    CoordEvent e;
    memset(&e, 0, sizeof(e));
    e.kind = kind;
    e.slot = slot;
    coord_step(&g_s, &e, &g_a);
}

/* A tab opened and started: its slot. */
static int started_tab(uint32_t tab, const char *url) {
    open_tab(tab, url);
    if (g_a.n != 1 || g_a.a[0].kind != CA_START_TAB) return -1;
    int slot = g_a.a[0].slot;
    step(CE_TAB_STARTED, slot);
    return slot;
}

static void want_webgpu(uint32_t tab) {
    UcTab t = { tab };
    ui(UC_WEBGPU, &t, sizeof(t));
}

/* The web-GPU processes (section 5.7): one per site, started when a tab of
 * it first asks, granted only to that site's tabs, ended with its last. */
static void test_webgpu(void) {
    coord_init(&g_s);
    int yt1 = started_tab(1, "https://www.youtube.com/");
    int ex = started_tab(2, "https://example.com/");
    CHECK(yt1 >= 0 && ex >= 0);
    want_webgpu(1);
    CHECK(g_a.n == 1 && g_a.a[0].kind == CA_START_WEBGPU && !strcmp(g_a.a[0].site, "https+youtube.com"));
    int w1 = g_a.a[0].webgpu;
    want_webgpu(1);                                          /* asked again while it starts */
    CHECK(g_a.n == 0);
    want_webgpu(2);                                          /* another site: a process of its own */
    CHECK(g_a.n == 1 && g_a.a[0].kind == CA_START_WEBGPU && !strcmp(g_a.a[0].site, "https+example.com") &&
          g_a.a[0].webgpu != w1);
    int w2 = g_a.a[0].webgpu;
    step(CE_WEBGPU_STARTED, w1);                             /* granted to the youtube tab only */
    CHECK(g_a.n == 1 && g_a.a[0].kind == CA_GRANT_WEBGPU && g_a.a[0].slot == yt1 && g_a.a[0].tab == 1 &&
          g_a.a[0].webgpu == w1 && !strcmp(g_a.a[0].site, "https+youtube.com"));
    int yt3 = started_tab(3, "https://music.youtube.com/");   /* the same site: the same process, at once */
    want_webgpu(3);
    CHECK(g_a.n == 1 && g_a.a[0].kind == CA_GRANT_WEBGPU && g_a.a[0].slot == yt3 && g_a.a[0].webgpu == w1);
    step(CE_WEBGPU_EXITED, w2);                              /* example.com's failed to start */
    CHECK(g_a.n == 1 && g_a.a[0].kind == CA_WEBGPU_FAILED && g_a.a[0].slot == ex && g_a.a[0].tab == 2);
    step(CE_WEBGPU_STARTED, w2);                             /* (a start nobody asked for) */
    CHECK(g_a.n == 0);
    int blank = started_tab(4, "about:blank");               /* an opaque origin: none */
    CHECK(blank >= 0);
    want_webgpu(4);
    CHECK(g_a.n == 1 && g_a.a[0].kind == CA_WEBGPU_FAILED && g_a.a[0].slot == blank);
    want_webgpu(99);                                         /* no such tab */
    CHECK(g_a.n == 0);
    ui(UC_WEBGPU, "\x01", 1);                                /* too short */
    CHECK(g_a.n == 0);
    step(CE_TAB_EXITED, yt1);                                /* one youtube tab left: it stays */
    bool killed = false;
    for (int k = 0; k < g_a.n; k++) killed |= g_a.a[k].kind == CA_KILL_WEBGPU;
    CHECK(!killed);
    step(CE_TAB_EXITED, yt3);                                /* the last: it ends */
    killed = false;
    for (int k = 0; k < g_a.n; k++) killed |= g_a.a[k].kind == CA_KILL_WEBGPU && g_a.a[k].webgpu == w1;
    CHECK(killed);
    step(CE_WEBGPU_EXITED, w1);
    CHECK(g_a.n == 0);
    int yt5 = started_tab(5, "https://youtube.com/");        /* and a new one for the site's next tab */
    want_webgpu(5);
    CHECK(yt5 >= 0 && g_a.n == 1 && g_a.a[0].kind == CA_START_WEBGPU && !strcmp(g_a.a[0].site, "https+youtube.com"));
    /* A crash while tabs use it: they ask again, and get a new one. */
    step(CE_WEBGPU_STARTED, g_a.a[0].webgpu);
    int w5 = g_a.a[0].webgpu;
    step(CE_WEBGPU_EXITED, w5);
    want_webgpu(5);
    CHECK(g_a.n == 1 && g_a.a[0].kind == CA_START_WEBGPU);
}

/* The shim's entries, for coord_plan.c: coord_do records what it is asked to
 * do, and fails the call numbered g_fail_at; the exit scans find the
 * processes a test says ended. */
static CoordOp g_done[2 * COORD_MAX_OPS];
static int g_ndone, g_fail_at = -1;
static bool g_tab_ended[COORD_MAX_TABS], g_webgpu_ended[COORD_MAX_WEBGPU];

bool coord_do(const CoordAction *a, const CoordOp *o) {
    (void)a;
    if (g_ndone < 2 * COORD_MAX_OPS) g_done[g_ndone] = *o;
    return g_ndone++ != g_fail_at;
}

bool coord_tab_signaled(int i, uint32_t *code) {
    if (!g_tab_ended[i]) return false;
    g_tab_ended[i] = false;
    *code = 7;
    return true;
}

bool coord_webgpu_signaled(int w, uint32_t *code) {
    if (!g_webgpu_ended[w]) return false;
    g_webgpu_ended[w] = false;
    *code = 0;
    return true;
}

static CoordTables g_t;
static CoordPlan g_p;

/* Plan and perform action a, failing the fail_at'th call: whether an event
 * follows, in *r. */
static bool run(const CoordAction *a, int fail_at, CoordEvent *r) {
    coord_plan(&g_s, &g_t, a, &g_p);
    g_ndone = 0;
    g_fail_at = fail_at;
    return coord_run(&g_t, a, &g_p, r);
}

static int count_ops(const CoordPlan *p, CoordOpKind kind) {
    int n = 0;
    for (int k = 0; k < p->n; k++) n += p->o[k].kind == kind;
    return n;
}

/* The coordinator's plans (coord_plan.c): which handle goes into which
 * process, with which badge; what is undone when a step fails; exits paired
 * with their slots. */
static void test_plan(void) {
    coord_init(&g_s);
    coord_tables_init(&g_t, true);
    CoordEvent r;

    /* A site's tab: its channels, its process, each server's end badged
     * with its site, the UI's end, then the slot's. */
    open_tab(1, "https://www.youtube.com/");
    CoordAction start = g_a.a[0];
    int yt = start.slot;
    CHECK(run(&start, -1, &r) && r.kind == CE_TAB_STARTED && r.slot == yt);
    CHECK(g_p.n == 15 && count_ops(&g_p, OP_CHAN) == 5 && count_ops(&g_p, OP_GRANT) == 3);
    CHECK(g_ndone == 11 && g_done[5].kind == OP_SPAWN_TAB && g_done[5].slot == yt);
    CHECK(g_done[6].kind == OP_GRANT && g_done[6].srv == SRV_NET && g_done[6].ch == CH_NET);
    CHECK(g_done[7].kind == OP_GRANT && g_done[7].srv == SRV_STORE && g_done[7].ch == CH_STORE);
    CHECK(g_done[8].kind == OP_GRANT && g_done[8].srv == SRV_GPU && g_done[8].ch == CH_DRAW);
    CHECK(g_done[9].kind == OP_GIVE_UI && g_done[9].ch == CH_UI && g_done[10].kind == OP_COMMIT_TAB);
    CHECK(g_t.tab_proc[yt] && g_t.tab_ctl[yt] && !strcmp(g_t.tab_site[yt], "https+youtube.com"));
    step(CE_TAB_STARTED, yt);

    /* An opaque tab: no storage channel. */
    open_tab(2, "about:blank");
    CoordAction blank = g_a.a[0];
    CHECK(run(&blank, -1, &r) && r.kind == CE_TAB_STARTED);
    CHECK(count_ops(&g_p, OP_CHAN) == 4 && count_ops(&g_p, OP_GRANT) == 2);
    for (int k = 0; k < g_p.n; k++) CHECK(g_p.o[k].ch != CH_STORE || g_p.o[k].kind == OP_REVOKE);
    step(CE_TAB_STARTED, blank.slot);

    /* The GPU process's grant fails: the grants are revoked and the process
     * ended; the slot gets none. */
    open_tab(3, "https://example.com/");
    CoordAction ex = g_a.a[0];
    CHECK(run(&ex, 8, &r) && r.kind == CE_TAB_START_FAILED && r.slot == ex.slot);
    CHECK(g_ndone == 13 && g_done[9].kind == OP_REVOKE && g_done[12].kind == OP_DISCARD_TAB);
    CHECK(!g_t.tab_proc[ex.slot]);
    /* A channel fails before the start: nothing to undo. */
    CHECK(run(&ex, 2, &r) && r.kind == CE_TAB_START_FAILED && g_ndone == 3);
    step(CE_TAB_START_FAILED, ex.slot);

    /* Actions the state doesn't bear out get nothing: another site's label
     * for a slot, a slot out of range, a slot that already has a process. */
    CoordAction forged = start;
    strcpy(forged.site, "https+example.com");
    CHECK(run(&forged, -1, &r) && r.kind == CE_TAB_START_FAILED && g_p.n == 0 && g_ndone == 0);
    forged = start;
    forged.slot = COORD_MAX_TABS;
    CHECK(run(&forged, -1, &r) && r.kind == CE_TAB_START_FAILED && r.slot == COORD_MAX_TABS && g_p.n == 0);
    forged = start;
    forged.kind = CA_KILL_TAB;
    forged.slot = ex.slot;                                  /* free: no process */
    CHECK(!run(&forged, -1, &r) && g_p.n == 0);

    /* The site's web-GPU process, and its channel to the site's tab only. */
    want_webgpu(1);
    CoordAction wstart = g_a.a[0];
    int w = wstart.webgpu;
    CHECK(run(&wstart, -1, &r) && r.kind == CE_WEBGPU_STARTED && r.slot == w && g_t.webgpu_proc[w]);
    step(CE_WEBGPU_STARTED, w);
    CoordAction grant = g_a.a[0];
    CHECK(grant.kind == CA_GRANT_WEBGPU && grant.slot == yt);
    CHECK(!run(&grant, -1, &r) && g_p.n == 5 && g_ndone == 3);
    CHECK(g_done[1].kind == OP_GRANT && g_done[1].srv == SRV_WEBGPU && g_done[1].w == w);
    CHECK(g_done[2].kind == OP_GIVE_TAB && g_done[2].slot == yt && g_done[2].msg == CT_WEBGPU);
    CHECK(g_t.tab_webgpu[yt] == w + 1);
    /* The copy into the tab fails: revoked, and the tab told. */
    CHECK(!run(&grant, 2, &r) && g_ndone == 5 && g_done[3].kind == OP_REVOKE && g_done[4].kind == OP_SEND_TAB);
    /* The opaque tab, or the youtube one with another label, gets nothing. */
    forged = grant;
    forged.slot = blank.slot;
    forged.tab = 2;
    CHECK(!run(&forged, -1, &r) && g_p.n == 0);
    forged = grant;
    strcpy(forged.site, "https+example.com");
    CHECK(!run(&forged, -1, &r) && g_p.n == 0);

    /* The web-GPU process ends: the tab it served is told, and the event
     * names its index. */
    g_webgpu_ended[w] = true;
    CHECK(coord_next_webgpu_exit(&g_s, &g_t, 0, &g_p, &r) == w && r.kind == CE_WEBGPU_EXITED && r.slot == w);
    CHECK(g_p.n == 1 && g_p.o[0].kind == OP_SEND_TAB && g_p.o[0].slot == yt && g_p.o[0].msg == CT_WEBGPU_FAILED);
    CHECK(!g_t.webgpu_proc[w] && g_t.tab_webgpu[yt] == 0);
    CHECK(coord_next_webgpu_exit(&g_s, &g_t, w + 1, &g_p, &r) == -1 && g_p.n == 0);
    step(CE_WEBGPU_EXITED, w);

    /* The tab ends: found in its entry, its event names its slot; then its
     * channel closes and every server revokes it. */
    g_tab_ended[yt] = true;
    CHECK(coord_next_tab_exit(&g_t, 0, &r) == yt && r.kind == CE_TAB_EXITED && r.slot == yt && r.exit_code == 7);
    CHECK(!g_t.tab_proc[yt] && coord_next_tab_exit(&g_t, yt + 1, &r) == -1);
    coord_step(&g_s, &r, &g_a);
    CoordAction gone = g_a.a[g_a.n - 1];
    for (int k = 0; k < g_a.n; k++)
        if (g_a.a[k].kind == CA_TAB_GONE) gone = g_a.a[k];
    CHECK(gone.kind == CA_TAB_GONE && gone.slot == yt);
    CHECK(!run(&gone, 0, &r) && g_ndone == 5 && g_done[0].kind == OP_CLOSE_CTL && g_done[0].slot == yt);
    CHECK(count_ops(&g_p, OP_REVOKE) == 3 && !g_t.tab_ctl[yt]);    /* all of them, though the first failed */
    CHECK(coord_next_tab_exit(&g_t, -5, &r) == -1 && coord_next_tab_exit(&g_t, 1000, &r) == -1);
}

static LoopState g_ls;
static LoopActions g_la;

static void net_msg(uint16_t type, const void *data, uint32_t len) {
    LoopEvent e;
    memset(&e, 0, sizeof(e));
    e.kind = LE_NET_MESSAGE;
    e.type = type;
    e.data = (const uint8_t *)data;
    e.len = len;
    loop_step(&g_ls, &e, &g_la);
}

static void sock_event(LoopEventKind kind, int slot, bool ok, const void *data, uint32_t len) {
    LoopEvent e;
    memset(&e, 0, sizeof(e));
    e.kind = kind;
    e.slot = slot;
    e.ok = ok;
    e.data = (const uint8_t *)data;
    e.len = len;
    loop_step(&g_ls, &e, &g_la);
}

static void test_loop(void) {
    loop_init(&g_ls);
    NlConnect c = { 7, LOOP_V4, 8080 };
    net_msg(NL_CONNECT, &c, sizeof(c));
    CHECK(g_la.n == 1 && g_la.a[0].kind == LA_CONNECT && g_la.a[0].family == LOOP_V4 && g_la.a[0].port == 8080);
    int slot = g_la.a[0].slot;
    net_msg(NL_CONNECT, &c, sizeof(c));                     /* the same id twice */
    CHECK(g_la.n == 1 && g_la.a[0].kind == LA_NET_SEND && g_la.a[0].type == LN_CONNECTED &&
          g_la.a[0].data[4] == 0);
    NlConnect bad = { 8, 5, 80 };                           /* not loopback: no such family */
    net_msg(NL_CONNECT, &bad, sizeof(bad));
    CHECK(g_la.n == 1 && g_la.a[0].kind == LA_NET_SEND && g_la.a[0].type == LN_CONNECTED);
    bad.family = LOOP_V6;
    bad.port = 70000;
    net_msg(NL_CONNECT, &bad, sizeof(bad));
    CHECK(g_la.n == 1 && g_la.a[0].kind == LA_NET_SEND && g_la.a[0].type == LN_CONNECTED);
    sock_event(LE_CONNECTED, slot, true, NULL, 0);
    CHECK(g_la.n == 1 && g_la.a[0].type == LN_CONNECTED && g_la.a[0].data[4] == 1);
    /* Bytes to the socket, unchanged, one NL_DATA at a time. */
    uint8_t d[8] = { 7, 0, 0, 0, 'G', 'E', 'T', ' ' };
    net_msg(NL_DATA, d, sizeof(d));
    CHECK(g_la.n == 1 && g_la.a[0].kind == LA_SOCK_SEND && g_la.a[0].len == 4 && !memcmp(g_la.a[0].data, "GET ", 4));
    net_msg(NL_DATA, d, sizeof(d));                         /* before LN_SENT: the connection ends */
    CHECK(g_la.n == 2 && g_la.a[0].kind == LA_SOCK_CLOSE && g_la.a[1].type == LN_CLOSED);
    /* Again, with the window. */
    c.id = 9;
    net_msg(NL_CONNECT, &c, sizeof(c));
    slot = g_la.a[0].slot;
    sock_event(LE_CONNECTED, slot, true, NULL, 0);
    static uint8_t big[LOOP_DATA_MAX];
    int reads = 0;
    while (loop_want_read(&g_ls, slot)) {
        sock_event(LE_SOCK_DATA, slot, true, big, sizeof(big));
        CHECK(g_la.n == 1 && g_la.a[0].type == LN_DATA && g_la.a[0].len == 4 + sizeof(big) && g_la.a[0].data[0] == 9);
        reads++;
    }
    CHECK(reads == (int)(LOOP_WINDOW / LOOP_DATA_MAX));
    NlCredit cr = { 9, LOOP_WINDOW };
    net_msg(NL_CREDIT, &cr, sizeof(cr));
    CHECK(loop_want_read(&g_ls, slot));
    sock_event(LE_SOCK_CLOSED, slot, false, NULL, 0);
    CHECK(g_la.n == 2 && g_la.a[0].kind == LA_SOCK_CLOSE && g_la.a[1].type == LN_CLOSED);
    CHECK(!loop_want_read(&g_ls, slot));
}

/* A TG_FRAME of the given records (counts as given, or the true ones). */
static uint8_t g_msg[1 << 20];
static uint32_t frame_msg(const WireCmd *c, uint32_t n, const WireMask *m, uint32_t nm) {
    GpuFrame f = { 1, 800, 600, n, nm, 100, 400, 250, 7, 5000 };
    memcpy(g_msg, &f, sizeof(f));
    memcpy(g_msg + sizeof(f), c, n * sizeof(WireCmd));
    memcpy(g_msg + sizeof(f) + n * sizeof(WireCmd), m, nm * sizeof(WireMask));
    return (uint32_t)(sizeof(f) + n * sizeof(WireCmd) + nm * sizeof(WireMask));
}

static void test_gpu_decode(void) {
    static GpuClientRes res;
    static WireCmd in[300], out[400];
    static uint8_t levels[400];
    static WireMask masks[4], mout[4];
    GpuFrame f;
    uint32_t n, nm;
    memset(&res, 0, sizeof(res));
    res.font[3] = 1;
    res.image[9] = 1;
    memset(in, 0, sizeof(in));
    /* Only what this tab uploaded resolves. */
    in[0].type = WIRE_GLYPH; in[0].ref = 3;
    in[1].type = WIRE_GLYPH; in[1].ref = 4;             /* another tab's slot */
    in[2].type = WIRE_IMAGE; in[2].ref = 9;
    in[3].type = WIRE_IMAGE; in[3].ref = 10;            /* another tab's image */
    in[4].type = WIRE_IMAGE; in[4].ref = 0xffffffffu;
    in[5].type = 99;                                    /* no such record */
    in[6].type = WIRE_RECT; in[6].x = (int32_t)0x80000000; in[6].w = 0x7fffffff; in[6].u0 = 0xffffffffu;
    uint32_t len = frame_msg(in, 7, NULL, 0);
    CHECK(gpu_decode(g_msg, len, &res, &f, out, levels, 400, &n, mout, 4, &nm));
    CHECK(n == 3 && out[0].type == WIRE_GLYPH && out[1].type == WIRE_IMAGE && out[1].ref == 9 && out[2].type == WIRE_RECT);
    CHECK(out[2].x == -GPU_COORD_MAX && out[2].w == GPU_COORD_MAX && out[2].u0 == WIRE_UV_ONE);
    CHECK(f.width == 800 && f.height == 600 && f.ncmds == 7);   /* the layer's place is read by gpu_main.c */
    /* Lengths that lie are refused whole. */
    CHECK(!gpu_decode(g_msg, len - 1, &res, &f, out, levels, 400, &n, mout, 4, &nm));
    ((GpuFrame *)g_msg)->ncmds = 0xffffffffu;
    CHECK(!gpu_decode(g_msg, len, &res, &f, out, levels, 400, &n, mout, 4, &nm));
    /* Brackets: balanced, at most GPU_MAX_DEPTH deep, a mask of a good kind. */
    memset(masks, 0, sizeof(masks));
    masks[0].kind = 3;
    masks[1].kind = 77;                                 /* no such kind */
    for (int i = 0; i < 200; i++) { in[i].type = WIRE_MASK_BEGIN; in[i].ref = 0; }
    in[200].type = WIRE_MASK_BEGIN; in[200].ref = 1;
    in[201].type = WIRE_MASK_END;
    len = frame_msg(in, 202, masks, 2);
    CHECK(gpu_decode(g_msg, len, &res, &f, out, levels, 400, &n, mout, 4, &nm));
    int depth = 0, max = 0, bad = 0;
    for (uint32_t i = 0; i < n; i++) {
        depth += out[i].type == WIRE_MASK_BEGIN ? 1 : out[i].type == WIRE_MASK_END ? -1 : 0;
        if (depth < 0 || levels[i] != depth) bad++;
        if (depth > max) max = depth;
    }
    CHECK(bad == 0 && depth == 0 && max == GPU_MAX_DEPTH && mout[1].kind == 0);
    in[0].type = WIRE_MASK_END;                         /* closes of nothing */
    len = frame_msg(in, 1, NULL, 0);
    CHECK(gpu_decode(g_msg, len, &res, &f, out, levels, 400, &n, mout, 4, &nm) && n == 0);
}

/* What a page may reach (PNA) and read across origins (ORB). */
static int orb(const char *mime, bool nosniff, int status, const char *body) {
    return orb_verdict((const uint8_t *)mime, (uint32_t)strlen(mime), nosniff, status, (const uint8_t *)body,
                       (uint32_t)strlen(body));
}

static void test_net_policy(void) {
    static const uint8_t lo[4] = { 127, 0, 0, 1 }, ten[4] = { 10, 1, 2, 3 }, cg[4] = { 172, 20, 0, 1 },
                         cg_out[4] = { 172, 32, 0, 1 }, home[4] = { 192, 168, 1, 1 }, pub[4] = { 8, 8, 8, 8 },
                         zero[4] = { 0, 0, 0, 0 };
    CHECK(net_space_v4(lo) == NET_SPACE_LOCAL && net_space_v4(zero) == NET_SPACE_LOCAL);
    CHECK(net_space_v4(ten) == NET_SPACE_PRIVATE && net_space_v4(cg) == NET_SPACE_PRIVATE &&
          net_space_v4(home) == NET_SPACE_PRIVATE);
    CHECK(net_space_v4(cg_out) == NET_SPACE_PUBLIC && net_space_v4(pub) == NET_SPACE_PUBLIC);
    uint8_t v6[16];
    memset(v6, 0, sizeof(v6));
    v6[15] = 1;
    CHECK(net_space_v6(v6) == NET_SPACE_LOCAL);                         /* ::1 */
    v6[10] = v6[11] = 0xff; v6[12] = 192; v6[13] = 168; v6[14] = 0; v6[15] = 9;
    CHECK(net_space_v6(v6) == NET_SPACE_PRIVATE);                       /* ::ffff:192.168.0.9 */
    memset(v6, 0, sizeof(v6));
    v6[0] = 0xfd;
    CHECK(net_space_v6(v6) == NET_SPACE_PRIVATE);                       /* fd00::/8 */
    v6[0] = 0x20; v6[1] = 0x01;
    CHECK(net_space_v6(v6) == NET_SPACE_PUBLIC);                        /* 2001:: */
    CHECK(net_space_allows(NET_SPACE_LOCAL, NET_SPACE_PUBLIC) && net_space_allows(NET_SPACE_PRIVATE, NET_SPACE_PRIVATE));
    CHECK(!net_space_allows(NET_SPACE_PUBLIC, NET_SPACE_PRIVATE) && !net_space_allows(NET_SPACE_PRIVATE, NET_SPACE_LOCAL));
    CHECK(!net_space_allows(7, NET_SPACE_PUBLIC) && !net_space_allows(NET_SPACE_LOCAL, -1));
    /* ORB: what embeds passes, documents don't. */
    CHECK(orb("image/png", true, 404, "<html>") == ORB_ALLOW);
    CHECK(orb("text/javascript; charset=utf-8", false, 200, "var a;") == ORB_ALLOW);
    CHECK(orb(" Text/CSS", false, 200, "") == ORB_ALLOW);
    CHECK(orb("application/pdf", false, 200, "%PDF") == ORB_BLOCK);
    CHECK(orb("text/html", true, 200, "var a;") == ORB_BLOCK);
    CHECK(orb("application/json", false, 200, "  {\"secret\": 1}") == ORB_BLOCK);
    CHECK(orb("text/plain", false, 200, "\xef\xbb\xbf<!DOCTYPE html><p>") == ORB_BLOCK);
    CHECK(orb("", false, 200, ")]}'\n[1]") == ORB_BLOCK);
    CHECK(orb("text/plain", false, 200, "{ \"a\\\"b\" : 1 }") == ORB_BLOCK);
    CHECK(orb("text/html", false, 500, "var a;") == ORB_BLOCK);
    CHECK(orb("application/octet-stream", false, 200, "var x = {a: 1};") == ORB_ALLOW);
    CHECK(orb("text/plain", false, 200, "{a: 1}") == ORB_ALLOW);          /* a block, not JSON */
    CHECK(orb("application/xml", false, 200, "<?xml version='1.0'?>") == ORB_BLOCK);
    CHECK(orb("text/html", false, 200, "<html") == ORB_ALLOW);           /* cut short: no tag yet */
}

/* A first part: the total, little-endian, then bytes. */
static uint32_t first_part(uint8_t *p, uint32_t total, const char *bytes) {
    p[0] = (uint8_t)total; p[1] = (uint8_t)(total >> 8); p[2] = (uint8_t)(total >> 16); p[3] = (uint8_t)(total >> 24);
    uint32_t n = (uint32_t)strlen(bytes);
    memcpy(p + 4, bytes, n);
    return 4 + n;
}

static void test_chan_framing(void) {
    /* The shared header's checks. */
    CHECK(chan_ring_header_ok(CHAN_MAGIC, 1u << 16) && chan_ring_header_ok(CHAN_MAGIC, CHAN_RING_MAX));
    CHECK(!chan_ring_header_ok(CHAN_MAGIC + 1, 1u << 16));
    CHECK(!chan_ring_header_ok(CHAN_MAGIC, 1u << 15) && !chan_ring_header_ok(CHAN_MAGIC, CHAN_RING_MAX * 2u));
    CHECK(!chan_ring_header_ok(CHAN_MAGIC, (1u << 16) + 8) && !chan_ring_header_ok(CHAN_MAGIC, 0));
    CHECK(big_core_limit(0) == BIG_MAX && big_core_limit(5) == 5 && big_core_limit(BIG_MAX + 1) == BIG_MAX);

    /* A message in two parts, the buffer grown on demand. */
    BigAsm s = { 0 };
    uint8_t part[64], buf[32];
    uint32_t need = 0, n = first_part(part, 10, "abcd");
    CHECK(big_core_part(&s, 0, 7, part, n, NULL, 0, &need) == BIG_GROW && need == 5 && !s.busy && s.have == 0);
    CHECK(big_core_part(&s, 0, 7, part, n, buf, sizeof(buf), &need) == BIG_PART && s.busy && s.have == 4 &&
          s.total == 10 && s.type == 7);
    CHECK(big_core_part(&s, 0, BIG_MORE, (const uint8_t *)"efghij", 6, buf, 10, &need) == BIG_GROW && need == 11);
    CHECK(big_core_part(&s, 0, BIG_MORE, (const uint8_t *)"efghij", 6, buf, sizeof(buf), &need) == BIG_DONE &&
          !s.busy && s.have == 10 && !memcmp(buf, "abcdefghij", 11));
    /* An empty message is whole at its first part. */
    n = first_part(part, 0, "");
    CHECK(big_core_part(&s, 0, 3, part, n, buf, sizeof(buf), &need) == BIG_DONE && s.total == 0 && buf[0] == 0);

    /* Broken sequences. */
    BigAsm idle = { 0 }, t;
    t = idle;
    CHECK(big_core_part(&t, 0, BIG_MORE, (const uint8_t *)"x", 1, buf, sizeof(buf), &need) == BIG_REFUSED);  /* no start */
    t = idle;
    CHECK(big_core_part(&t, 0, 1, part, 3, buf, sizeof(buf), &need) == BIG_REFUSED);       /* shorter than 4 */
    n = first_part(part, 2, "abc");
    CHECK(big_core_part(&t, 0, 1, part, n, buf, sizeof(buf), &need) == BIG_REFUSED);       /* more than announced */
    n = first_part(part, 100, "abc");
    CHECK(big_core_part(&t, 99, 1, part, n, buf, sizeof(buf), &need) == BIG_REFUSED);      /* over the limit */
    CHECK(big_core_part(&t, 100, 1, part, n, buf, sizeof(buf), &need) == BIG_PART && t.busy);
    CHECK(big_core_part(&t, 100, 1, part, n, buf, sizeof(buf), &need) == BIG_REFUSED);     /* a start while busy */
    t.have = 3; t.total = 4;
    CHECK(big_core_part(&t, 100, BIG_MORE, (const uint8_t *)"yz", 2, buf, sizeof(buf), &need) == BIG_REFUSED);
    t.have = 5; t.total = 4;                                                              /* a broken state */
    CHECK(big_core_part(&t, 100, BIG_MORE, (const uint8_t *)"", 0, buf, sizeof(buf), &need) == BIG_REFUSED);
    t.have = 3; t.total = BIG_MAX;
    CHECK(big_core_part(&t, 100, BIG_MORE, (const uint8_t *)"", 0, buf, sizeof(buf), &need) == BIG_REFUSED);

    /* Over a real channel, through big_send and big_recv. */
    Chan a, b;
    HANDLE sec, e0, e1, self = GetCurrentProcess();
    CHECK(chan_create(&a, 1u << 20));
    DuplicateHandle(self, a.section, self, &sec, 0, FALSE, DUPLICATE_SAME_ACCESS);
    DuplicateHandle(self, a.ev[0], self, &e0, 0, FALSE, DUPLICATE_SAME_ACCESS);
    DuplicateHandle(self, a.ev[1], self, &e1, 0, FALSE, DUPLICATE_SAME_ACCESS);
    CHECK(chan_attach(&b, sec, e0, e1, 1));
    static uint8_t big[100000];
    for (uint32_t i = 0; i < sizeof(big); i++) big[i] = (uint8_t)(i * 7 + i / 251);
    BigRx rx;
    memset(&rx, 0, sizeof(rx));
    CHECK(big_send(&a, 5, big, 40000, NULL, 0, big + 40000, sizeof(big) - 40000, 1000));
    CHECK(big_recv(&b, &rx) == 1 && rx.type == 5 && rx.total == sizeof(big) && !memcmp(rx.buf, big, sizeof(big)) &&
          rx.buf[sizeof(big)] == 0 && rx.cap <= 2 * sizeof(big));
    CHECK(big_send(&a, 6, NULL, 0, NULL, 0, NULL, 0, 1000) && big_recv(&b, &rx) == 1 && rx.type == 6 && rx.total == 0);
    CHECK(big_recv(&b, &rx) == 0);
    rx.limit = sizeof(big) - 1;                                                          /* one byte too many */
    CHECK(big_send(&a, 5, big, sizeof(big), NULL, 0, NULL, 0, 1000) && big_recv(&b, &rx) == -1);
    big_free(&rx);
    chan_close(&b);
    chan_close(&a);
}

/* ---- the web-GPU process's decoders (webgl_decode.c, webgpu_decode.c) ---- */

static uint8_t g_rec[1 << 16];
static uint32_t g_rec_len;
static void rec_u32(uint32_t v) { memcpy(g_rec + g_rec_len, &v, 4); g_rec_len += 4; }
static void rec_bytes(const void *p, uint32_t n) { memcpy(g_rec + g_rec_len, p, n); g_rec_len += n; }
static void rec_u8(uint8_t v) { g_rec[g_rec_len++] = v; }

/* A batch of one record: op, then words, then tail bytes. */
static void gl_one(uint32_t op, const uint32_t *w, uint32_t nw, const void *tail, uint32_t nt) {
    g_rec_len = 0;
    rec_u32(op);
    rec_u32(nw * 4 + nt);
    for (uint32_t i = 0; i < nw; i++) rec_u32(w[i]);
    if (nt) rec_bytes(tail, nt);
}

/* webgl_next on the batch's first record, with `replied` taken. */
static int gl_walk(uint32_t replied, WebglRec *r) { return webgl_next(g_rec, g_rec_len, 0, replied, r); }

static void test_webgl_decode(void) {
    WebglRec r;
    g_rec_len = 0;
    CHECK(gl_walk(0, &r) == 0);                                         /* the end */
    /* Plain calls: exactly the generated length. */
    uint32_t w[32] = { 0x0DE1 };
    gl_one(WEBGL_OP_BINDTEXTURE, w, 2, NULL, 0);
    CHECK(gl_walk(0, &r) == 1 && r.op == WEBGL_OP_BINDTEXTURE && r.at == 8 && r.n == 8 && r.head == 8 && r.reply == 0);
    gl_one(WEBGL_OP_BINDTEXTURE, w, 3, NULL, 0);
    CHECK(gl_walk(0, &r) == -1);
    gl_one(WEBGL_OP_FINISH, w, 0, NULL, 0);
    CHECK(gl_walk(0, &r) == 1);
    for (uint32_t op = 1; op <= WEBGL_PLAIN_OPS; op++) {
        gl_one(op, w, webgl_plain_len[op] / 4, NULL, 0);
        CHECK(gl_walk(0, &r) == 1 && r.n == webgl_plain_len[op]);
    }
    /* Ops that aren't. */
    static const uint32_t bad_ops[] = { 0, WEBGL_PLAIN_OPS + 1, 255, 277, 299, OP_END, 0xffffffffu };
    for (size_t i = 0; i < sizeof(bad_ops) / 4; i++) {
        gl_one(bad_ops[i], w, 0, NULL, 0);
        CHECK(gl_walk(0, &r) == -1);
    }
    /* A length past the batch, a record cut short, a tail after the last. */
    gl_one(WEBGL_OP_FINISH, w, 0, NULL, 0);
    memcpy(g_rec + 4, &(uint32_t){ 4 }, 4);
    CHECK(gl_walk(0, &r) == -1);
    g_rec_len = 0;
    rec_u32(WEBGL_OP_FINISH);
    CHECK(gl_walk(0, &r) == -1);
    gl_one(WEBGL_OP_FINISH, w, 0, NULL, 0);
    rec_u8(0);
    CHECK(webgl_next(g_rec, g_rec_len, 8, 0, &r) == -1);
    CHECK(webgl_next(g_rec, g_rec_len, 9, 0, &r) == 0);
    /* Exact specials. */
    uint32_t create[3] = { WEBGL_BUFFER, 5, 0 };
    gl_one(OP_CREATE, create, 3, NULL, 0);
    CHECK(gl_walk(0, &r) == 1 && r.head == 12);
    gl_one(OP_CREATE, create, 2, NULL, 0);
    CHECK(gl_walk(0, &r) == -1);
    gl_one(OP_CREATE, create, 3, "x", 1);
    CHECK(gl_walk(0, &r) == -1);
    /* Strings: no NUL. */
    gl_one(OP_SHADER_SOURCE, w, 1, "void main(){}", 13);
    CHECK(gl_walk(0, &r) == 1 && r.head == 4 && r.n == 17);
    gl_one(OP_SHADER_SOURCE, w, 1, "void\0main", 9);
    CHECK(gl_walk(0, &r) == -1);
    gl_one(OP_REQUEST_EXTENSION, w, 0, "", 0);
    CHECK(gl_walk(0, &r) == 1 && r.reply == 4);
    /* Names: counted, capped. */
    gl_one(OP_GET_UNIFORM_INDICES, w, 1, "a\0bb\0c", 6);
    CHECK(gl_walk(0, &r) == 1 && r.count == 3 && r.reply == 4 + 12);
    gl_one(OP_GET_UNIFORM_INDICES, w, 1, "a\0", 2);
    CHECK(gl_walk(0, &r) == 1 && r.count == 1);
    char many[200];
    memset(many, 0, sizeof(many));
    gl_one(OP_TRANSFORM_FEEDBACK_VARYINGS, w, 2, many, 64);
    CHECK(gl_walk(0, &r) == 1 && r.count == 64);
    gl_one(OP_TRANSFORM_FEEDBACK_VARYINGS, w, 2, many, 65);
    CHECK(gl_walk(0, &r) == -1);
    /* Words. */
    gl_one(OP_DRAW_BUFFERS, w, 16, NULL, 0);
    CHECK(gl_walk(0, &r) == 1 && r.count == 16);
    gl_one(OP_DRAW_BUFFERS, w, 0, "abc", 3);
    CHECK(gl_walk(0, &r) == -1);
    gl_one(OP_INVALIDATE_FRAMEBUFFER, w, 1 + 17, NULL, 0);
    CHECK(gl_walk(0, &r) == -1);
    /* Counted queries: the count capped, the reply counted. */
    uint32_t gi[2] = { 0x0BA2, 16 };
    gl_one(OP_GET_INTEGER, gi, 2, NULL, 0);
    CHECK(gl_walk(0, &r) == 1 && r.count == 16 && r.reply == 64);
    gi[1] = 17;
    gl_one(OP_GET_INTEGER, gi, 2, NULL, 0);
    CHECK(gl_walk(0, &r) == -1);
    /* Uploads with a flag: data only with it. */
    uint32_t tex[9] = { 0x0DE1, 0, 0x1908, 1, 1, 0, 0x1908, 0x1401, 0 };
    gl_one(OP_TEX_IMAGE_2D, tex, 9, NULL, 0);
    CHECK(gl_walk(0, &r) == 1);
    gl_one(OP_TEX_IMAGE_2D, tex, 9, "abcd", 4);
    CHECK(gl_walk(0, &r) == -1);
    tex[8] = 1;
    gl_one(OP_TEX_IMAGE_2D, tex, 9, "abcd", 4);
    CHECK(gl_walk(0, &r) == 1 && r.head == 36 && r.n == 40);
    uint32_t bd[5] = { 0x8892, 0x88E4, 4, 0, 1 };      /* target, usage, size 4 (i64), has */
    gl_one(OP_BUFFER_DATA, bd, 5, "abcd", 4);
    CHECK(gl_walk(0, &r) == 1);
    gl_one(OP_BUFFER_DATA, bd, 5, "abc", 3);
    CHECK(gl_walk(0, &r) == -1);
    bd[3] = 1;                                          /* a size of 2^32 + 4 */
    gl_one(OP_BUFFER_DATA, bd, 5, "abcd", 4);
    CHECK(gl_walk(0, &r) == -1);
    bd[2] = 0xffffffffu; bd[3] = 0xffffffffu; bd[4] = 0;    /* -1 and no data: GL's error, not ours */
    gl_one(OP_BUFFER_DATA, bd, 5, NULL, 0);
    CHECK(gl_walk(0, &r) == 1);
    /* Uniforms: whole values of the kind. */
    uint32_t un[3] = { 1, 4, 0 };                      /* vec4 */
    gl_one(OP_UNIFORM_V, un, 3, many, 32);
    CHECK(gl_walk(0, &r) == 1 && r.count == 2);
    gl_one(OP_UNIFORM_V, un, 3, many, 20);
    CHECK(gl_walk(0, &r) == -1);
    un[1] = 15;                                         /* mat4 */
    gl_one(OP_UNIFORM_V, un, 3, many, 128);
    CHECK(gl_walk(0, &r) == 1 && r.count == 2);
    un[1] = 22;
    gl_one(OP_UNIFORM_V, un, 3, many, 4);
    CHECK(gl_walk(0, &r) == -1);
    un[1] = 0;
    gl_one(OP_UNIFORM_V, un, 3, NULL, 0);
    CHECK(gl_walk(0, &r) == -1);
    uint32_t va[2] = { 0, 3 };
    gl_one(OP_VERTEX_ATTRIB_V, va, 2, many, 12);
    CHECK(gl_walk(0, &r) == 1 && r.count == 3);
    gl_one(OP_VERTEX_ATTRIB_V, va, 2, many, 16);
    CHECK(gl_walk(0, &r) == -1);
    /* Replies: at most WEBGL_REPLY_MAX for a batch. */
    uint32_t rp[7] = { 0, 0, 1, 1, 0x1908, 0x1401, 100u << 20 };
    gl_one(OP_READ_PIXELS, rp, 7, NULL, 0);
    CHECK(gl_walk(0, &r) == 1 && r.reply == 4 + (100u << 20));
    CHECK(gl_walk(28u << 20, &r) == -1);
    CHECK(gl_walk(WEBGL_REPLY_MAX - 4 - (100u << 20), &r) == 1);
    rp[6] = WEBGL_DATA_MAX + 1;
    gl_one(OP_READ_PIXELS, rp, 7, NULL, 0);
    CHECK(gl_walk(0, &r) == -1);
    gl_one(OP_GET_ERROR, w, 0, NULL, 0);
    CHECK(gl_walk(WEBGL_REPLY_MAX, &r) == -1 && gl_walk(WEBGL_REPLY_MAX - 4, &r) == 1);
    /* Two records, walked in turn. */
    g_rec_len = 0;
    rec_u32(WEBGL_OP_FLUSH); rec_u32(0);
    rec_u32(OP_GET_ERROR); rec_u32(0);
    CHECK(webgl_next(g_rec, g_rec_len, 0, 0, &r) == 1 && r.at + r.n == 8);
    CHECK(webgl_next(g_rec, g_rec_len, 8, 0, &r) == 1 && r.op == OP_GET_ERROR && r.at + r.n == 16);
    CHECK(webgl_next(g_rec, g_rec_len, 16, 0, &r) == 0);
    /* Random bytes: whatever is accepted lies inside the batch. */
    uint32_t seed = 12345;
    for (int round = 0; round < 20000; round++) {
        g_rec_len = 8 + (round % 120);
        for (uint32_t i = 0; i < g_rec_len; i++) { seed = seed * 1103515245u + 12345u; g_rec[i] = (uint8_t)(seed >> 16); }
        uint32_t op = round % 2 ? 256 + round % 90 : 1 + round % 110, n = g_rec_len - 8;
        if (round % 3) { memcpy(g_rec, &op, 4); memcpy(g_rec + 4, &n, 4); }
        int got = gl_walk(0, &r);
        CHECK(got == -1 || (got == 1 && r.at == 8 && r.at + r.n <= g_rec_len && r.head <= r.n &&
                            r.reply <= WEBGL_REPLY_MAX));
    }

    /* Ids: only this context's table, never out of it. */
    static uint32_t buffers[WEBGL_MAX_IDS], programs[WEBGL_MAX_IDS];
    uint32_t *names[WEBGL_KINDS] = { 0 };
    names[WEBGL_BUFFER] = buffers;
    names[WEBGL_PROGRAM] = programs;
    buffers[7] = 42;
    programs[7] = 43;
    CHECK(webgl_name_of(names, WEBGL_BUFFER, 7) == 42 && webgl_name_of(names, WEBGL_PROGRAM, 7) == 43);
    CHECK(webgl_name_of(names, WEBGL_BUFFER, 0) == 0 && webgl_name_of(names, 99, 0) == 0);
    CHECK(webgl_name_of(names, WEBGL_BUFFER, 8) == WEBGL_NO_NAME);              /* never made */
    CHECK(webgl_name_of(names, WEBGL_TEXTURE, 7) == WEBGL_NO_NAME);             /* no table yet */
    CHECK(webgl_name_of(names, WEBGL_KINDS, 7) == WEBGL_NO_NAME);
    CHECK(webgl_name_of(names, WEBGL_BUFFER, WEBGL_MAX_IDS) == WEBGL_NO_NAME);
    CHECK(webgl_name_of(names, WEBGL_BUFFER, 0xffffffffu) == WEBGL_NO_NAME);
    CHECK(webgl_id_ok(1) && webgl_id_ok(WEBGL_MAX_IDS - 1) && !webgl_id_ok(0) && !webgl_id_ok(WEBGL_MAX_IDS));
    static WebglLocation locs[WEBGL_MAX_IDS];
    locs[3].program = 43;
    locs[3].location = 2;
    CHECK(webgl_location_of(locs, 3) == 2 && webgl_location_of(locs, 4) == -1 && webgl_location_of(NULL, 3) == -1);
    CHECK(webgl_location_of(locs, 0) == -1 && webgl_location_of(locs, WEBGL_MAX_IDS) == -1);
}

/* The parser M8 had (recursive), as the reference the iterative one must
 * agree with. */
static int ref_parse(WireTree *t, const uint8_t *in, uint32_t len, uint32_t *at, int depth) {
    if (depth > WIRE_MAX_DEPTH || *at >= len || t->n >= WIRE_MAX_NODES) return -1;
    int i = t->n++;
    WireNode *v = &t->nodes[i];
    memset(v, 0, sizeof(*v));
    v->first = v->next = -1;
    v->tag = in[(*at)++];
    switch (v->tag) {
    case V_NULL: case V_FALSE: case V_TRUE: return i;
    case V_NUMBER: if (len - *at < 8) return -1; memcpy(&v->bits, in + *at, 8); *at += 8; return i;
    case V_ID: if (len - *at < 4) return -1; memcpy(&v->n, in + *at, 4); *at += 4; return i;
    case V_STRING: case V_BYTES:
        if (len - *at < 4) return -1;
        memcpy(&v->n, in + *at, 4);
        *at += 4;
        if (v->n > len - *at || v->n > WIRE_MAX_STRING) return -1;
        v->off = *at;
        *at += v->n;
        return i;
    case V_ARRAY: case V_OBJECT: {
        if (len - *at < 4) return -1;
        uint32_t n;
        memcpy(&n, in + *at, 4);
        *at += 4;
        if (n > WIRE_MAX_NODES) return -1;
        t->nodes[i].n = n;
        int last = -1;
        for (uint32_t k = 0; k < n; k++) {
            uint32_t ko = 0, kl = 0;
            if (t->nodes[i].tag == V_OBJECT) {
                if (len - *at < 4) return -1;
                memcpy(&kl, in + *at, 4);
                *at += 4;
                if (kl > WIRE_MAX_KEY || kl > len - *at) return -1;
                ko = *at;
                *at += kl;
            }
            int c = ref_parse(t, in, len, at, depth + 1);
            if (c < 0) return -1;
            t->nodes[c].key_off = ko;
            t->nodes[c].key_len = kl;
            if (last < 0) t->nodes[i].first = c;
            else t->nodes[last].next = c;
            last = c;
        }
        return i;
    }
    default: return -1;
    }
}

static WireTree g_t1, g_t2;
static WgpuObj g_objs[WGPU_MAX_IDS];

/* `deep` arrays, one in another, around a null. */
static void nested(uint32_t deep) {
    g_rec_len = 0;
    for (uint32_t k = 0; k < deep; k++) { rec_u8(V_ARRAY); rec_u32(1); }
    rec_u8(V_NULL);
}

static int32_t gpu_parse(void) {
    uint32_t at = 0;
    g_t1.n = 0;
    return wire_parse(g_rec, g_rec_len, &at, &g_t1);
}

static uint64_t f64_bits(double d) { uint64_t b; memcpy(&b, &d, 8); return b; }
static bool whole(double d, uint64_t want) { uint64_t v = 7; return wire_whole(f64_bits(d), &v) && v == want; }
static bool not_whole(double d) { uint64_t v; return !wire_whole(f64_bits(d), &v); }

static void test_wgpu_decode(void) {
    /* {"size": 64, "label": "b", "usage": [1, id 9, true]} */
    g_rec_len = 0;
    rec_u8(V_OBJECT); rec_u32(3);
    rec_u32(4); rec_bytes("size", 4); rec_u8(V_NUMBER); rec_bytes(&(double){ 64 }, 8);
    rec_u32(5); rec_bytes("label", 5); rec_u8(V_STRING); rec_u32(1); rec_bytes("b", 1);
    rec_u32(5); rec_bytes("usage", 5); rec_u8(V_ARRAY); rec_u32(3);
    rec_u8(V_NUMBER); rec_bytes(&(double){ 1 }, 8); rec_u8(V_ID); rec_u32(9); rec_u8(V_TRUE);
    uint32_t at = 0;
    g_t1.n = 0;
    CHECK(wire_parse(g_rec, g_rec_len, &at, &g_t1) == 0 && at == g_rec_len && g_t1.n == 7);
    const WireNode *nd = g_t1.nodes;
    CHECK(nd[0].tag == V_OBJECT && nd[0].n == 3 && nd[0].first == 1);
    CHECK(nd[1].key_len == 4 && !memcmp(g_rec + nd[1].key_off, "size", 4) && nd[1].bits == f64_bits(64) && nd[1].next == 2);
    CHECK(nd[2].tag == V_STRING && nd[2].n == 1 && g_rec[nd[2].off] == 'b' && nd[2].next == 3);
    CHECK(nd[3].tag == V_ARRAY && nd[3].first == 4 && nd[3].next == -1 && nd[4].next == 5 && nd[5].next == 6);
    CHECK(nd[5].tag == V_ID && nd[5].n == 9 && nd[6].tag == V_TRUE && nd[6].next == -1);
    /* A second value after it goes on the same tree. */
    uint32_t end = g_rec_len;
    rec_u8(V_BYTES); rec_u32(2); rec_bytes("xy", 2);
    CHECK(wire_parse(g_rec, g_rec_len, &at, &g_t1) == 7 && at == g_rec_len && g_t1.n == 8 && g_t1.nodes[7].off == end + 5);
    CHECK(wire_parse(g_rec, g_rec_len, &at, &g_t1) == -1);             /* nothing left */
    /* Depth: a null 12 levels down is allowed, 13 isn't. */
    nested(WIRE_MAX_DEPTH);
    CHECK(gpu_parse() == 0 && g_t1.n == WIRE_MAX_DEPTH + 1);
    nested(WIRE_MAX_DEPTH + 1);
    CHECK(gpu_parse() == -1);
    /* Nodes: at most WIRE_MAX_NODES, the container counted. */
    g_rec_len = 0;
    rec_u8(V_ARRAY); rec_u32(WIRE_MAX_NODES - 1);
    for (int k = 0; k < WIRE_MAX_NODES - 1; k++) rec_u8(V_NULL);
    CHECK(gpu_parse() == 0 && g_t1.n == WIRE_MAX_NODES);
    g_rec_len = 0;
    rec_u8(V_ARRAY); rec_u32(WIRE_MAX_NODES);
    for (int k = 0; k < WIRE_MAX_NODES; k++) rec_u8(V_NULL);
    CHECK(gpu_parse() == -1);
    /* Keys and strings inside the input. */
    g_rec_len = 0;
    rec_u8(V_OBJECT); rec_u32(1); rec_u32(WIRE_MAX_KEY + 1);
    for (int k = 0; k < WIRE_MAX_KEY + 1; k++) rec_u8('k');
    rec_u8(V_NULL);
    CHECK(gpu_parse() == -1);
    g_rec_len = 0;
    rec_u8(V_STRING); rec_u32(5); rec_bytes("abcd", 4);
    CHECK(gpu_parse() == -1);
    g_rec_len = 0;
    rec_u8(9);
    CHECK(gpu_parse() == -1);
    /* Against the M8 parser: every prefix of a tree, and random trees. */
    uint32_t seed = 777;
    for (int round = 0; round < 4000; round++) {
        g_rec_len = 0;
        uint32_t want = 1 + round % 60;
        while (g_rec_len < want * 6 && g_rec_len < 2000) {
            seed = seed * 1103515245u + 12345u;
            uint32_t r = seed >> 8;
            switch (r % 9) {
            case 0: rec_u8(V_ARRAY); rec_u32(r / 9 % 4); break;
            case 1: rec_u8(V_OBJECT); rec_u32(r / 9 % 3); rec_u32(r / 99 % 3); rec_bytes("abc", r / 99 % 3); break;
            case 2: rec_u8(V_STRING); rec_u32(r / 9 % 5); rec_bytes("hello", r / 9 % 5); break;
            case 3: rec_u8(V_NUMBER); rec_bytes(&(double){ (double)(r % 1000) }, 8); break;
            case 4: rec_u8(V_ID); rec_u32(r % 70000); break;
            case 5: rec_u32(r % 9); break;                      /* a key's length, or garbage */
            case 6: rec_u8((uint8_t)(r % 12)); break;
            default: rec_u8((uint8_t)(r % 3)); break;
            }
        }
        for (uint32_t len = round % 7 ? g_rec_len : 0; len <= g_rec_len; len++) {
            uint32_t a1 = 0, a2 = 0;
            g_t1.n = 0;
            g_t2.n = 0;
            int32_t r1 = wire_parse(g_rec, len, &a1, &g_t1);
            int r2 = ref_parse(&g_t2, g_rec, len, &a2, 0);
            bool same = r1 == r2;
            if (same && r1 >= 0) {
                same = a1 == a2 && g_t1.n == g_t2.n;
                for (int32_t i = 0; same && i < g_t1.n; i++) {
                    const WireNode *x = &g_t1.nodes[i], *y = &g_t2.nodes[i];
                    same = x->tag == y->tag && x->n == y->n && x->bits == y->bits && x->off == y->off &&
                           x->key_off == y->key_off && x->key_len == y->key_len && x->first == y->first &&
                           x->next == y->next;
                }
            }
            CHECK(same);
            /* And what it returns lies inside the input, children after parents. */
            for (int32_t i = 0; r1 >= 0 && i < g_t1.n; i++) {
                const WireNode *x = &g_t1.nodes[i];
                bool bytes = x->tag == V_STRING || x->tag == V_BYTES;
                CHECK(x->key_off + x->key_len <= len && (!bytes || x->off + x->n <= len));
                CHECK((x->first == -1 || (x->first > i && x->first < g_t1.n)) &&
                      (x->next == -1 || (x->next > i && x->next < g_t1.n)));
            }
        }
    }

    /* Records' framing. */
    uint32_t op, off, n;
    g_rec_len = 0;
    rec_u32(G_RELEASE); rec_u32(4); rec_u32(5);
    CHECK(wire_next(g_rec, g_rec_len, 0, &op, &off, &n) == 1 && op == G_RELEASE && off == 8 && n == 4);
    CHECK(wire_next(g_rec, g_rec_len, 12, &op, &off, &n) == 0);
    CHECK(wire_next(g_rec, g_rec_len - 1, 0, &op, &off, &n) == -1);
    rec_u8(1);
    CHECK(wire_next(g_rec, g_rec_len, 12, &op, &off, &n) == -1);

    /* Ids: this client's live objects of the kind, or none. */
    memset(g_objs, 0, sizeof(g_objs));
    g_objs[2] = (WgpuObj){ .kind = O_DEVICE, .h = (void *)1, .device = 2 };
    g_objs[3] = (WgpuObj){ .kind = O_BUFFER, .h = (void *)1, .device = 2 };
    g_objs[4] = (WgpuObj){ .kind = O_BUFFER, .h = NULL, .device = 2 };
    CHECK(wgpu_obj(g_objs, 3, O_BUFFER) == &g_objs[3] && wgpu_obj(g_objs, 2, O_DEVICE) == &g_objs[2]);
    CHECK(!wgpu_obj(g_objs, 3, O_TEXTURE) && !wgpu_obj(g_objs, 4, O_BUFFER) && !wgpu_obj(g_objs, 5, O_BUFFER));
    CHECK(!wgpu_obj(g_objs, 0, O_NONE) && !wgpu_obj(g_objs, WGPU_MAX_IDS, O_BUFFER) &&
          !wgpu_obj(g_objs, 0xffffffffu, O_BUFFER));
    g_objs[2].lost = true;                                  /* device.destroy() */
    CHECK(!wgpu_obj(g_objs, 3, O_BUFFER) && !wgpu_obj(g_objs, 2, O_DEVICE));
    CHECK(wgpu_id_free(g_objs, 5) && !wgpu_id_free(g_objs, 3) && !wgpu_id_free(g_objs, 0) &&
          !wgpu_id_free(g_objs, WGPU_MAX_IDS));

    /* The page's numbers, and the ranges into mappings. */
    CHECK(whole(0, 0) && whole(-0.0, 0) && whole(1.5, 1) && whole(0.25, 0) && whole(4096, 4096));
    CHECK(whole(9007199254740991.0, 9007199254740991ull) && whole(1e-300, 0) && whole(123456789.99, 123456789));
    CHECK(not_whole(-1) && not_whole(-0.5) && not_whole(9007199254740992.0) && not_whole(1e300));
    double inf = 1e308 * 10, nan = inf - inf;
    CHECK(not_whole(inf) && not_whole(-inf) && not_whole(nan));
    CHECK(wgpu_map_ok(0, 16, 16) && wgpu_map_ok(8, 8, 16) && !wgpu_map_ok(8, 12, 16) && !wgpu_map_ok(4, 4, 16));
    CHECK(!wgpu_map_ok(0, 6, 16) && !wgpu_map_ok(0, WGPU_MAP_MAX + 4, 1ull << 40) && wgpu_map_ok(0, WGPU_MAP_MAX, 1ull << 40));
    CHECK(!wgpu_map_ok(0xfffffffffffffff8ull, 16, 16) && !wgpu_map_ok(24, 0, 16));
    CHECK(wgpu_write_ok(8, 8, 8, 8) && wgpu_write_ok(16, 4, 8, 16) && !wgpu_write_ok(16, 8, 8, 8));
    CHECK(!wgpu_write_ok(0, 8, 8, 8) && !wgpu_write_ok(8, 0, 8, 8) && !wgpu_write_ok(12, 4, 8, 16) && !wgpu_write_ok(8, 6, 8, 16));
    CHECK(!wgpu_write_ok(0xfffffffffffffff8ull, 8, 8, 0xfffffffffffffff0ull));
}

/* M9: the documents a tab is given (P10), initiators, CORS, and the decoding
 * of what a tab sends the network process. */
#define S(s) (const uint8_t *)(s), (uint32_t)strlen(s)

static int doc(int mode, const char *tab, const char *site) { return net_doc_verdict(mode, S(tab), S(site)); }

static NetFrames g_frames;

/* net_may_read for a response at final_origin (of final_site) to a tab of
 * https+a.com, from initiator (of init_site). */
static int may(int mode, const char *init, const char *init_site, const char *final_origin, const char *final_site,
               bool cred, const char *acao, const char *acao_site, const char *acac, const char *mime, const char *body) {
    NetRead r = { mode, S("https+a.com"), S(init), S(init_site), &g_frames, S(final_origin), S(final_site), cred,
                  acao != NULL, S(acao ? acao : ""), S(acao_site), S(acac), S(mime), false, 200, S(body) };
    return net_may_read(&r);
}

/* A TN_REQUEST: the fixed part, then strs (n bytes, NULs included). */
static uint32_t req(uint8_t *out, uint32_t ml, uint32_t ul, uint32_t hl, uint32_t nh, uint32_t bl, uint32_t mode,
                    uint32_t il, const void *strs, uint32_t n) {
    uint32_t f[8] = { 7, ml, ul, hl, nh, bl, mode, il };
    memcpy(out, f, sizeof(f));
    memcpy(out + sizeof(f), strs, n);
    return (uint32_t)sizeof(f) + n;
}

static void test_net_core(void) {
    /* P10 */
    CHECK(doc(NET_MODE_NAVIGATE, "https+a.com", "https+a.com") == NET_DOC_OK);
    CHECK(doc(NET_MODE_NAVIGATE, "https+a.com", "https+b.com") == NET_DOC_ELSEWHERE);
    CHECK(doc(NET_MODE_NAVIGATE, "https+a.com", "http+a.com") == NET_DOC_ELSEWHERE);
    CHECK(doc(NET_MODE_NAVIGATE, "https+a.com", "") == NET_DOC_ELSEWHERE);
    CHECK(doc(NET_MODE_NAVIGATE, "", "") == NET_DOC_ELSEWHERE);                 /* opaque: no documents */
    CHECK(doc(NET_MODE_FRAME, "https+a.com", "https+b.com") == NET_DOC_OK);     /* frames: any site */
    CHECK(doc(NET_MODE_NO_CORS, "https+a.com", "https+b.com") == NET_DOC_OK);
    CHECK(net_same(S("ab"), S("ab")) && !net_same(S("ab"), S("abc")) && !net_same(S("ab"), S("aB")));
    /* Frames and initiators */
    memset(&g_frames, 0, sizeof(g_frames));
    CHECK(net_frames_add(&g_frames, S("https://f.com")) && net_frames_add(&g_frames, S("https://f.com")) &&
          g_frames.n == 1);
    CHECK(net_frames_has(&g_frames, S("https://f.com")) && !net_frames_has(&g_frames, S("https://f.co")));
    static char longo[NET_ORIGIN_MAX + 2];
    memset(longo, 'a', NET_ORIGIN_MAX + 1);
    CHECK(!net_frames_add(&g_frames, S(longo)) && g_frames.n == 1);
    char o[32];
    for (int i = 1; i < NET_FRAMES_MAX; i++) {
        snprintf(o, sizeof(o), "https://f%d.com", i);
        net_frames_add(&g_frames, S(o));
    }
    CHECK(g_frames.n == NET_FRAMES_MAX && !net_frames_add(&g_frames, S("https://full.com")));
    CHECK(net_initiator_ok(&g_frames, S("https+a.com"), S("https://www.a.com"), S("https+a.com")));
    CHECK(net_initiator_ok(&g_frames, S("https+a.com"), S("https://f.com"), S("https+f.com")));
    CHECK(!net_initiator_ok(&g_frames, S("https+a.com"), S("https://b.com"), S("https+b.com")));
    CHECK(!net_initiator_ok(&g_frames, S("https+a.com"), S("null"), S("")));
    /* Reading: documents and same-site responses pass. */
    CHECK(may(NET_MODE_NAVIGATE, "", "", "https://b.com", "https+b.com", true, NULL, "", "", "text/html", "<html>") ==
          NET_READ_OK);
    CHECK(may(NET_MODE_FRAME, "", "", "https://b.com", "https+b.com", true, NULL, "", "", "text/html", "<html>") ==
          NET_READ_OK);
    CHECK(may(NET_MODE_NO_CORS, "", "", "https://x.a.com", "https+a.com", true, NULL, "", "", "text/html", "<html>") ==
          NET_READ_OK);
    CHECK(may(7, "", "", "https://x.a.com", "https+a.com", true, NULL, "", "", "", "") == NET_READ_MODE);
    /* ORB across sites */
    CHECK(may(NET_MODE_NO_CORS, "", "", "https://b.com", "https+b.com", true, NULL, "", "", "application/json",
              "{\"a\": 1}") == NET_READ_ORB);
    CHECK(may(NET_MODE_NO_CORS, "", "", "https://b.com", "https+b.com", true, NULL, "", "", "image/png", "x") ==
          NET_READ_OK);
    /* An initiator it may name: the origin counts, not the site. */
    CHECK(may(NET_MODE_NO_CORS, "https://f.com", "https+f.com", "https://f.com", "https+f.com", true, NULL, "", "",
              "text/html", "<html>") == NET_READ_OK);
    CHECK(may(NET_MODE_NO_CORS, "https://www.a.com", "https+a.com", "https://a.com", "https+a.com", true, NULL, "",
              "", "text/html", "<html>") == NET_READ_ORB);
    /* ...one it may not is the page: b.com's own pages stay cross. */
    CHECK(may(NET_MODE_NO_CORS, "https://b.com", "https+b.com", "https://b.com", "https+b.com", true, NULL, "", "",
              "text/html", "<html>") == NET_READ_ORB);
    /* CORS */
    CHECK(may(NET_MODE_CORS, "", "", "https://b.com", "https+b.com", false, NULL, "", "", "", "") == NET_READ_NO_ACAO);
    CHECK(may(NET_MODE_CORS, "", "", "https://b.com", "https+b.com", false, "*", "", "", "", "") == NET_READ_OK);
    CHECK(may(NET_MODE_CORS, "", "", "https://b.com", "https+b.com", true, "*", "", "true", "", "") ==
          NET_READ_CREDENTIALS);
    CHECK(may(NET_MODE_CORS, "", "", "https://b.com", "https+b.com", false, "https://www.a.com", "https+a.com", "",
              "", "") == NET_READ_OK);
    CHECK(may(NET_MODE_CORS, "", "", "https://b.com", "https+b.com", true, "https://www.a.com", "https+a.com", "",
              "", "") == NET_READ_CREDENTIALS);
    CHECK(may(NET_MODE_CORS, "", "", "https://b.com", "https+b.com", true, "https://www.a.com", "https+a.com", "True",
              "", "") == NET_READ_OK);
    CHECK(may(NET_MODE_CORS, "", "", "https://b.com", "https+b.com", false, "https://c.com", "https+c.com", "", "",
              "") == NET_READ_ACAO);
    CHECK(may(NET_MODE_CORS, "https://f.com", "https+f.com", "https://b.com", "https+b.com", false, "https://f.com",
              "https+f.com", "", "", "") == NET_READ_OK);
    CHECK(may(NET_MODE_CORS, "https://f.com", "https+f.com", "https://b.com", "https+b.com", false,
              "https://www.a.com", "https+a.com", "", "", "") == NET_READ_ACAO);

    /* Decoding TN_REQUEST */
    static uint8_t m[1024];
    uint32_t n;
    NetReq q;
    static const char good[] = "GET\0http://a.com/\0Accept: */*\0X: y\0https://a.com\0body";
    n = req(m, 4, 14, 17, 2, 4, NET_MODE_CORS, 14, good, sizeof(good) - 1);
    CHECK(net_decode_request(m, n, &q) && q.id == 7 && q.mode == NET_MODE_CORS && q.nheaders == 2 &&
          !strcmp((const char *)m + q.method, "GET") && !strcmp((const char *)m + q.url, "http://a.com/") &&
          !strcmp((const char *)m + q.header[0], "Accept: */*") && !strcmp((const char *)m + q.header[1], "X: y") &&
          !strcmp((const char *)m + q.initiator, "https://a.com") && q.initiator_len == 13 && q.body_len == 4 &&
          !memcmp(m + q.body, "body", 4) && q.body + q.body_len == n);
    n = req(m, 4, 14, 17, 2, 4, NET_MODE_CORS, 14, good, sizeof(good));                 /* a byte too many */
    CHECK(!net_decode_request(m, n, &q));
    n = req(m, 4, 14, 17, 1, 4, NET_MODE_CORS, 14, good, sizeof(good) - 1);             /* nheaders lies */
    CHECK(!net_decode_request(m, n, &q));
    n = req(m, 4, 14, 17, 3, 4, NET_MODE_CORS, 14, good, sizeof(good) - 1);
    CHECK(!net_decode_request(m, n, &q));
    n = req(m, 4, 14, 17, 2, 4, 4, 14, good, sizeof(good) - 1);                         /* no such mode */
    CHECK(!net_decode_request(m, n, &q));
    n = req(m, 5, 13, 17, 2, 4, NET_MODE_CORS, 14, good, sizeof(good) - 1);             /* a NUL in the URL's place */
    CHECK(!net_decode_request(m, n, &q));
    n = req(m, 4, 0xffffffffu, 17, 2, 4, NET_MODE_CORS, 14, good, sizeof(good) - 1);    /* lengths past the end */
    CHECK(!net_decode_request(m, n, &q));
    static const char noinit[] = "GET\0http://a.com/\0";
    n = req(m, 4, 14, 0, 0, 0, NET_MODE_NAVIGATE, 0, noinit, sizeof(noinit) - 1);
    CHECK(net_decode_request(m, n, &q) && q.initiator_len == 0 && m[q.initiator] == 0 && q.body == n);
    static char bigi[600];
    memcpy(bigi, "GET\0http://a.com/\0", 18);
    memset(bigi + 18, 'o', 520);
    n = req(m, 4, 14, 0, 0, 0, NET_MODE_CORS, 521, bigi, 18 + 521);                     /* initiator over 512 */
    CHECK(!net_decode_request(m, n, &q));
    CHECK(!net_decode_request(m, 31, &q));
    static const char empty[] = "\0http://a.com/\0";                                   /* no method */
    n = req(m, 1, 14, 0, 0, 0, 0, 0, empty, sizeof(empty) - 1);
    CHECK(!net_decode_request(m, n, &q));
    /* TN_COOKIE_GET, TN_COOKIE_SET */
    uint32_t id, ul, ll;
    memcpy(m, "\x05\0\0\0http://a.com/", 17);
    CHECK(net_decode_cookie_get(m, 17, &id, &ul) && id == 5 && ul == 13);
    m[17] = 0;
    CHECK(net_decode_cookie_get(m, 18, &id, &ul) && ul == 13);                          /* with its NUL */
    m[18] = 0;
    CHECK(!net_decode_cookie_get(m, 19, &id, &ul) && !net_decode_cookie_get(m, 4, &id, &ul));
    memcpy(m, "http://a.com/\0a=1; path=/", 25);
    CHECK(net_decode_cookie_set(m, 25, &ul, &ll) && ul == 13 && ll == 11);
    m[25] = 0;
    CHECK(net_decode_cookie_set(m, 26, &ul, &ll) && ll == 11);
    m[26] = 0;
    CHECK(!net_decode_cookie_set(m, 27, &ul, &ll));
    CHECK(!net_decode_cookie_set(m, 13, &ul, &ll) && !net_decode_cookie_set((const uint8_t *)"\0a=1", 4, &ul, &ll));
}

/* ---- M9: the servers' decoding of a tab's messages ----------------------------- */

/* A StoreRequest message: header, site, path, data. */
static uint32_t store_msg(uint8_t *out, uint32_t id, const char *site, const char *path, const char *data) {
    StoreRequest r = { id, (uint32_t)strlen(site), (uint32_t)strlen(path), (uint32_t)strlen(data) };
    memcpy(out, &r, sizeof(r));
    memcpy(out + sizeof(r), site, r.site_len);
    memcpy(out + sizeof(r) + r.site_len, path, r.path_len);
    memcpy(out + sizeof(r) + r.site_len + r.path_len, data, r.data_len);
    return (uint32_t)sizeof(r) + r.site_len + r.path_len + r.data_len;
}

static bool wide_is(const uint16_t *w, const char *s) {
    for (; *s; s++, w++)
        if (*w != (uint16_t)(uint8_t)*s) return false;
    return *w == 0;
}

static void test_store_decode(void) {
    static uint8_t m[2048];
    static StoreReq q;
    static uint16_t path[520];
    static const uint16_t root[] = { 'C', ':', '\\', 'r' };
    uint32_t n = store_msg(m, 5, "", "local_storage/x", "");
    CHECK(store_decode(m, n, "https+a.com", root, 4, &q, path, 520) == STORE_REQ_OK);
    CHECK(q.id == 5 && !strcmp(q.target, "https+a.com") && !strcmp(q.rel, "local_storage/x") && q.data_len == 0);
    CHECK(wide_is(path, "C:\\r\\https+a.com\\local_storage\\x"));
    n = store_msg(m, 6, "", "f", "hello");
    CHECK(store_decode(m, n, "https+a.com", root, 4, &q, path, 520) == STORE_REQ_OK);
    CHECK(q.data_len == 5 && !memcmp(m + q.data_off, "hello", 5));
    /* The network process names the site, and only for its cookies. */
    n = store_msg(m, 7, "https+b.com", "cookies", "");
    CHECK(store_decode(m, n, NULL, root, 4, &q, path, 520) == STORE_REQ_OK && wide_is(path, "C:\\r\\https+b.com\\cookies"));
    CHECK(store_decode(m, n, "https+a.com", root, 4, &q, path, 520) == STORE_REQ_REFUSED);   /* a tab naming another */
    n = store_msg(m, 8, "", "cookies", "");
    CHECK(store_decode(m, n, NULL, root, 4, &q, path, 520) == STORE_REQ_REFUSED);            /* no site */
    CHECK(store_decode(m, n, "https+a.com", root, 4, &q, path, 520) == STORE_REQ_REFUSED);   /* a tab's cookies */
    n = store_msg(m, 9, "", "x", "");
    CHECK(store_decode(m, n, "", root, 4, &q, path, 520) == STORE_REQ_REFUSED);              /* opaque */
    n = store_msg(m, 10, "", "../x", "");
    CHECK(store_decode(m, n, "https+a.com", root, 4, &q, path, 520) == STORE_REQ_REFUSED);
    /* A NUL in a named site. */
    n = store_msg(m, 11, "https+b.com", "cookies", "");
    m[sizeof(StoreRequest) + 3] = 0;
    CHECK(store_decode(m, n, NULL, root, 4, &q, path, 520) == STORE_REQ_REFUSED);
    /* Lengths that don't add up, a short message. */
    n = store_msg(m, 12, "", "x", "ab");
    CHECK(store_decode(m, n - 1, "https+a.com", root, 4, &q, path, 520) == STORE_REQ_BAD && q.id == 12);
    CHECK(store_decode(m, n + 1, "https+a.com", root, 4, &q, path, 520) == STORE_REQ_BAD);
    CHECK(store_decode(m, 15, "https+a.com", root, 4, &q, path, 520) == STORE_REQ_SHORT);
    StoreRequest huge = { 13, 0, 0xfffffff0u, 0x20u };          /* sums past 2^32 */
    memcpy(m, &huge, sizeof(huge));
    CHECK(store_decode(m, 16, "https+a.com", root, 4, &q, path, 520) == STORE_REQ_BAD);
    /* Too long for the path buffer: refused, not cut short. */
    n = store_msg(m, 14, "", "aaaaaaaaaa/bbbbbbbbbb", "");
    CHECK(store_decode(m, n, "https+a.com", root, 4, &q, path, 4 + 1 + 11 + 1 + 21) == STORE_REQ_REFUSED);
    CHECK(store_decode(m, n, "https+a.com", root, 4, &q, path, 4 + 1 + 11 + 1 + 21 + 1) == STORE_REQ_OK);
}

static void test_gpu_input(void) {
    static uint8_t m[64 + 16 * 4];
    static GpuClientRes ns;
    TgAtlas a = { 3, 4, 4, 1, 2 }, pa;
    memcpy(m, &a, sizeof(a));
    for (int i = 0; i < 8; i++) m[20 + i] = (uint8_t)(i + 1);
    CHECK(gpu_atlas_parse(m, 28, &pa) && pa.slot == 3 && pa.y0 == 1 && pa.rows == 2);
    CHECK(!gpu_atlas_parse(m, 27, &pa) && !gpu_atlas_parse(m, 29, &pa) && !gpu_atlas_parse(m, 19, &pa));
    TgAtlas bad = { GPU_FONT_SLOTS, 4, 4, 0, 0 };
    memcpy(m, &bad, sizeof(bad));
    CHECK(!gpu_atlas_parse(m, 20, &pa));
    TgAtlas over = { 0, 4, 4, 3, 2 };                       /* past the bottom */
    memcpy(m, &over, sizeof(over));
    CHECK(!gpu_atlas_parse(m, 28, &pa));
    memcpy(m, &a, sizeof(a));
    gpu_atlas_parse(m, 28, &pa);
    /* A new slot: unmarked until its pixels are in. */
    GpuPixels p = { NULL, 0, 0 };
    uint64_t bytes = 0;
    ns.font[3] = 1;
    CHECK(gpu_upload_plan(&ns.font[3], &bytes, &p, 4, 4, 1, true) == GPU_UP_NEW && ns.font[3] == 0);
    CHECK(!gpu_atlas_put(&ns, &p, &pa, m, 28) && ns.font[3] == 0);   /* the allocation failed */
    static uint8_t px[16];
    p.px = px;
    p.w = p.h = 4;
    CHECK(gpu_atlas_put(&ns, &p, &pa, m, 28) && ns.font[3] == 1 && px[4] == 1 && px[11] == 8 && px[12] == 0);
    CHECK(gpu_upload_plan(&ns.font[3], &bytes, &p, 4, 4, 1, true) == GPU_UP_WRITE && ns.font[3] == 1);
    /* Another size: unmarked, its bytes given back. */
    bytes = 16;
    CHECK(gpu_upload_plan(&ns.font[3], &bytes, &p, 8, 8, 1, true) == GPU_UP_NEW && ns.font[3] == 0 && bytes == 0);
    p.w = 8;
    CHECK(!gpu_atlas_put(&ns, &p, &pa, m, 28) && ns.font[3] == 0);   /* not that atlas's size */
    /* Over the limit: dropped, the old atlas kept. */
    ns.font[3] = 1;
    p.w = 4;
    bytes = GPU_CLIENT_BYTES - 10;
    CHECK(gpu_upload_plan(&ns.font[3], &bytes, &p, 8, 8, 1, true) == GPU_UP_DROP && ns.font[3] == 1 &&
          bytes == GPU_CLIENT_BYTES - 10);
    /* Images: the old one's bytes don't count against the new. */
    TgImage t = { 9, 2, 2 }, pt;
    memcpy(m, &t, sizeof(t));
    for (int i = 0; i < 16; i++) m[12 + i] = (uint8_t)(100 + i);
    CHECK(gpu_image_parse(m, 28, &pt) && pt.id == 9 && !gpu_image_parse(m, 27, &pt));
    TgImage bad_id = { GPU_MAX_IMAGES, 2, 2 };
    memcpy(m, &bad_id, sizeof(bad_id));
    CHECK(!gpu_image_parse(m, 28, &pt));
    memcpy(m, &t, sizeof(t));
    gpu_image_parse(m, 28, &pt);
    static uint8_t ipx[16];
    GpuPixels ip = { ipx, 4, 1 };
    bytes = GPU_CLIENT_BYTES;
    ns.image[9] = 1;
    CHECK(gpu_upload_plan(&ns.image[9], &bytes, &ip, 2, 2, 4, false) == GPU_UP_NEW && ns.image[9] == 0 &&
          bytes == GPU_CLIENT_BYTES - 16);
    ip.w = ip.h = 2;
    CHECK(gpu_image_put(&ns, &ip, &pt, m, 28) && ns.image[9] == 1 && ipx[0] == 100 && ipx[15] == 115);
    /* The frame header, and where the viewport is. */
    GpuFrame f = { 1, 800, 600, 3, 0, 100, 400, 250, 7, 5000 }, h;
    memcpy(m, &f, sizeof(f));
    CHECK(gpu_frame_header(m, 40, &h) && !memcmp(&h, &f, sizeof(f)) && !gpu_frame_header(m, 39, &h));
    f.ncmds = GPU_MAX_CMDS + 1;
    memcpy(m, &f, sizeof(f));
    CHECK(!gpu_frame_header(m, 40, &h));
    GpuFrame at = { 1, 800, 600, 0, 0, 100, 400, 250, 7, 5000 };
    GpuScroll s[GPU_MAX_SCROLLS] = { { 8, 100 }, { 9, -1000000 }, { 10, 30 } };
    CHECK(gpu_view_y(&at, s, 0) == 250 && gpu_layer_row(&at, s, 0) == 150);
    CHECK(gpu_view_y(&at, s, 1) == 350 && gpu_view_y(&at, s, 2) == 0 && gpu_view_y(&at, s, 3) == 30);
    at.origin_y = 0xffffffffu;                                /* far below: clamped */
    CHECK(gpu_layer_row(&at, s, 0) == -GPU_MAX_H);
    at.scroll_y = 0xffffffffu;
    at.origin_y = 0;
    CHECK(gpu_view_y(&at, s, 0) == 4600 && gpu_layer_row(&at, s, 0) == GPU_MAX_H);
    CHECK(gpu_scrolls_seen(s, 3, 8) == 2 && s[0].seq == 9 && s[1].seq == 10);
    GpuScroll w[2] = { { 0xfffffffeu, 1 }, { 2, 1 } };       /* the numbering wraps */
    CHECK(gpu_scrolls_seen(w, 2, 0xffffffffu) == 1 && w[0].seq == 2);
}

static void test_ui_core(void) {
    static UiTabMsg u;
    static uint8_t m[sizeof(TuNavigate) + PROTO_URL_MAX + 8];
    TuFrame f = { 1, 0, 800, 600, 10, 2000 };
    CHECK(ui_tab_message(TU_FRAME, (const uint8_t *)&f, sizeof(f), &u) == TU_FRAME && u.frame.height == 600 &&
          u.frame.content_height == 2000);
    CHECK(ui_tab_message(TU_FRAME, (const uint8_t *)&f, sizeof(f) - 1, &u) == 0);
    f.width = SURFACE_MAX_W + 1;
    CHECK(ui_tab_message(TU_FRAME, (const uint8_t *)&f, sizeof(f), &u) == 0);
    f.width = 1;
    f.height = 0;
    CHECK(ui_tab_message(TU_FRAME, (const uint8_t *)&f, sizeof(f), &u) == 0);
    CHECK(ui_tab_message(TU_TITLE, (const uint8_t *)"Hi", 2, &u) == TU_TITLE && !strcmp(u.text, "Hi"));
    CHECK(ui_tab_message(TU_TITLE, m, 0, &u) == TU_TITLE && u.text[0] == 0);
    CHECK(ui_tab_message(TU_TITLE, m, PROTO_TITLE_MAX + 1, &u) == 0);
    TuState s = { TAB_LOAD_FAILED };
    CHECK(ui_tab_message(TU_STATE, (const uint8_t *)&s, sizeof(s), &u) == TU_STATE && u.state == TAB_LOAD_FAILED);
    s.state = 77;
    CHECK(ui_tab_message(TU_STATE, (const uint8_t *)&s, sizeof(s), &u) == TU_STATE && u.state == TAB_LOADING);
    CHECK(ui_tab_message(TU_WANT_WEBGPU, m, 0, &u) == TU_WANT_WEBGPU && ui_tab_message(TU_WANT_WEBGPU, m, 1, &u) == 0);
    TuNavigate nv = { 5 };
    memcpy(m, &nv, sizeof(nv));
    memcpy(m + sizeof(nv), "https://a.com/", 14);
    CHECK(ui_tab_message(TU_NAVIGATE, m, sizeof(nv) + 14, &u) == TU_NAVIGATE && u.new_tab == 1 &&
          !strcmp(u.text, "https://a.com/"));
    CHECK(ui_tab_message(TU_NAVIGATE, m, sizeof(nv), &u) == 0);                      /* no URL */
    m[sizeof(nv) + 5] = 0;
    CHECK(ui_tab_message(TU_NAVIGATE, m, sizeof(nv) + 14, &u) == 0);                 /* a NUL in it */
    CHECK(ui_tab_message(TU_NAVIGATE, m, sizeof(nv) + PROTO_URL_MAX + 1, &u) == 0);  /* too long */
    CHECK(ui_tab_message(99, m, 4, &u) == 0);
    /* Opening tabs: once per click, within UI_GESTURE_MS. */
    UiGesture g = { 0, 0 };
    CHECK(!ui_may_open_tab(&g, 100));
    ui_gesture(&g, 100);
    CHECK(ui_may_open_tab(&g, 100 + UI_GESTURE_MS) && !ui_may_open_tab(&g, 101));
    ui_gesture(&g, 100);
    CHECK(!ui_may_open_tab(&g, 101 + UI_GESTURE_MS));
    ui_gesture(&g, 0xfffffff0u);                              /* the tick count wraps */
    CHECK(ui_may_open_tab(&g, 5));
}

/* loop_step's new promise: what follows an action leads to no other. */
static void test_loop_follow(void) {
    loop_init(&g_ls);
    NlConnect c = { 21, LOOP_V6, 443 };
    net_msg(NL_CONNECT, &c, sizeof(c));
    int slot = g_la.a[0].slot;
    sock_event(LE_CONNECTED, slot, false, NULL, 0);           /* a connect that failed at once */
    CHECK(g_la.n == 2 && g_la.a[0].kind == LA_SOCK_CLOSE && g_la.a[1].kind == LA_NET_SEND);
    net_msg(NL_CONNECT, &c, sizeof(c));
    slot = g_la.a[0].slot;
    sock_event(LE_CONNECTED, slot, true, NULL, 0);
    uint8_t d[5] = { 21, 0, 0, 0, 'x' };
    net_msg(NL_DATA, d, sizeof(d));
    sock_event(LE_SOCK_SENT, slot, true, NULL, 0);
    CHECK(g_la.n == 1 && g_la.a[0].kind == LA_NET_SEND && g_la.a[0].type == LN_SENT);
    sock_event(LE_SOCK_CLOSED, slot, false, NULL, 0);
    for (int k = 0; k < g_la.n; k++) CHECK(g_la.a[k].kind == LA_SOCK_CLOSE || g_la.a[k].kind == LA_NET_SEND);
}

int main(void) {
    test_sites();
    test_store();
    test_coord();
    test_webgpu();
    test_plan();
    test_loop();
    test_gpu_decode();
    test_net_policy();
    test_chan_framing();
    test_webgl_decode();
    test_wgpu_decode();
    test_net_core();
    test_store_decode();
    test_gpu_input();
    test_ui_core();
    test_loop_follow();
    printf("%d checks, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}

/* proc_tests.c - tests of the multi-process browser's pure parts: sites,
 * the storage service's rules, the coordinator's and the relay's cores, the
 * GPU process's decoder.
 *   proc_tests.exe        prints failures, exits 1 if any */
#include <stdio.h>
#include <string.h>

#include "site.h"
#include "store_core.h"
#include "coord_core.h"
#include "loop_core.h"
#include "gpu_decode.h"
#include "net_policy.h"

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

int main(void) {
    test_sites();
    test_store();
    test_coord();
    test_loop();
    test_gpu_decode();
    test_net_policy();
    printf("%d checks, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}

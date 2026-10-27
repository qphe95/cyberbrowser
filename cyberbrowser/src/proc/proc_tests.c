/* proc_tests.c - tests of the multi-process browser's pure parts: sites,
 * the storage service's rules, the coordinator's core.
 *   proc_tests.exe        prints failures, exits 1 if any */
#include <stdio.h>
#include <string.h>

#include "site.h"
#include "store_core.h"
#include "coord_core.h"

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

int main(void) {
    test_sites();
    test_store();
    test_coord();
    printf("%d checks, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}

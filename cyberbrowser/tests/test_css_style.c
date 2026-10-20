/*
 * Tests for the parallel style and layout pipeline (PARALLEL_STYLE_LAYOUT.md):
 * job system, frame arenas, atoms, the compiled matcher (against the legacy
 * string matcher), chunked parallel parsing (against the serial parser),
 * thread-safe text measurement, and layout determinism across scheduling
 * and caching switches.
 */
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_runner.h"
#include "css_layout.h"
#include "css_parser.h"
#include "css_style.h"
#include "display_list.h"
#include "frame_arena.h"
#include "html_dom.h"
#include "job_system.h"
#include "text_shaper.h"

#ifdef _WIN32
static int setenv(const char *k, const char *v, int o) { (void)o; return _putenv_s(k, v); }
static int unsetenv(const char *k) { return _putenv_s(k, ""); }
#endif

void run_css_style_tests(void);

/* ---------------------------------------------------------------------------
 * Deterministic random source
 * ------------------------------------------------------------------------- */

static uint32_t g_rng = 1;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static uint32_t rnd_n(uint32_t n) { return rnd() % n; }

typedef struct Buf { char *s; size_t n, cap; } Buf;
#include <stdarg.h>
static void buf_add(Buf *b, const char *fmt, ...)
{
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        int w = vsnprintf(b->s ? b->s + b->n : NULL, b->s ? b->cap - b->n : 0, fmt, ap);
        va_end(ap);
        if (w >= 0 && b->s && b->n + (size_t)w < b->cap) { b->n += (size_t)w; return; }
        b->cap = (b->cap ? b->cap * 2 : 4096) + (size_t)(w > 0 ? w : 0);
        b->s = realloc(b->s, b->cap);
        if (b->n == 0) b->s[0] = '\0';
    }
}

/* ---------------------------------------------------------------------------
 * Job system
 * ------------------------------------------------------------------------- */

typedef struct SumNode { uint64_t lo, hi, result; struct SumNode *left, *right; } SumNode;

static void sum_cont(Job *j)
{
    SumNode *n = (SumNode *)j->ctx;
    n->result = n->left->result + n->right->result;
}

static void sum_job(Job *j)
{
    SumNode *n = (SumNode *)j->ctx;
    if (n->hi - n->lo <= 64) {
        uint64_t s = 0;
        for (uint64_t i = n->lo; i < n->hi; i++) s += i;
        n->result = s;
        return;
    }
    uint64_t mid = (n->lo + n->hi) / 2;
    n->left = frame_arena_calloc(job_thread_arena(), sizeof(SumNode));
    n->right = frame_arena_calloc(job_thread_arena(), sizeof(SumNode));
    n->left->lo = n->lo; n->left->hi = mid;
    n->right->lo = mid; n->right->hi = n->hi;
    job_spawn(job_new(sum_job, j, n->left, 0, 0));
    job_spawn(job_new(sum_job, j, n->right, 0, 0));
    /* The last child to finish computes this node. */
    job_set_continuation(j, sum_cont);
}

TEST(test_job_continuations_tree) {
    for (int rep = 0; rep < 20; rep++) {
        SumNode root = { 0, 200000 + (uint64_t)rep, 0, NULL, NULL };
        job_graph_run(job_new(sum_job, NULL, &root, 0, 0));
        uint64_t n = root.hi;
        ASSERT_TRUE(root.result == n * (n - 1) / 2);
    }
    uint64_t jobs = 0, steals = 0;
    job_last_stats(&jobs, &steals);
    ASSERT_TRUE(jobs > 1000);
    return true;
}

static _Atomic uint32_t g_leaf_count;

static void group_leaf(Job *j) { (void)j; atomic_fetch_add(&g_leaf_count, 1); }

static void group_inner(Job *j)
{
    /* Nested fork-join from inside a job. */
    Job *g = job_group_new();
    for (uint32_t i = 0; i < j->a; i++) job_group_spawn(g, group_leaf, NULL, 0, 0);
    job_group_wait(g);
    atomic_fetch_add(&g_leaf_count, 1000);
}

static void group_root(Job *j)
{
    Job *g = job_group_new();
    for (int i = 0; i < 50; i++) job_group_spawn(g, group_inner, NULL, (uint32_t)(i % 7 + 1), 0);
    job_group_wait(g);
    /* Every descendant finished before the wait returned. */
    *(uint32_t *)j->ctx = atomic_load(&g_leaf_count);
}

TEST(test_job_groups_nested) {
    uint32_t seen = 0;
    atomic_store(&g_leaf_count, 0);
    job_graph_run(job_new(group_root, NULL, &seen, 0, 0));
    uint32_t leaves = 0;
    for (int i = 0; i < 50; i++) leaves += (uint32_t)(i % 7 + 1);
    ASSERT_EQ((int)(leaves + 50 * 1000), (int)seen);
    return true;
}

/* Graphs run one after another reuse the pool and leave the caller outside it. */
TEST(test_job_graphs_back_to_back) {
    SumNode root = { 0, 5000, 0, NULL, NULL };
    job_graph_run(job_new(sum_job, NULL, &root, 0, 0));
    ASSERT_TRUE(root.result == (uint64_t)5000 * 4999 / 2);
    ASSERT_TRUE(job_thread_index() == -1);   /* back to "not a job thread" */
    return true;
}

/* ---------------------------------------------------------------------------
 * Frame arenas
 * ------------------------------------------------------------------------- */

TEST(test_frame_arena) {
    FrameArena a = {0}, b = {0};
    for (int i = 1; i < 5000; i++) {
        unsigned char *p = frame_arena_alloc(&a, (size_t)(i % 97) + 1);
        ASSERT_TRUE(p != NULL);
        ASSERT_TRUE(((uintptr_t)p & 15) == 0);
        memset(p, 0xAB, (size_t)(i % 97) + 1);
    }
    char *big = frame_arena_alloc(&a, FRAME_ARENA_BLOCK * 2);   /* oversized */
    ASSERT_TRUE(big != NULL);
    memset(big, 1, FRAME_ARENA_BLOCK * 2);
    char *after = frame_arena_strdup(&a, "still in the current block");
    ASSERT_STR_EQ("still in the current block", after);
    char *z = frame_arena_calloc(&b, 64);
    for (int i = 0; i < 64; i++) ASSERT_TRUE(z[i] == 0);
    size_t bytes = a.bytes + b.bytes;
    frame_arena_absorb(&a, &b);
    ASSERT_TRUE(b.head == NULL && a.bytes == bytes);
    size_t before = frame_arena_pool_blocks();
    frame_arena_reset(&a);
    ASSERT_TRUE(a.head == NULL);
    ASSERT_TRUE(frame_arena_pool_blocks() > before);   /* standard blocks recycled */
    return true;
}

/* ---------------------------------------------------------------------------
 * Atoms
 * ------------------------------------------------------------------------- */

static const char *g_atom_words[] = { "div", "Span", "mw-parser-output", "HLIST", "a", "x-y", "c12", "section" };
static CssAtom g_atom_ids[8][8];

static void atom_job(Job *j)
{
    for (int rep = 0; rep < 200; rep++)
        for (int w = 0; w < 8; w++)
            g_atom_ids[j->a][w] = css_atom_intern(g_atom_words[w], strlen(g_atom_words[w]));
}

static void atom_root(Job *j)
{
    for (uint32_t t = 0; t < 8; t++) job_spawn(job_new(atom_job, j, NULL, t, 0));
}

TEST(test_atoms_case_and_concurrency) {
    css_atom_reserve(1024);
    CssAtom a1 = css_atom_intern("Foo-Bar", 7);
    CssAtom a2 = css_atom_intern("foo-bar", 7);
    ASSERT_TRUE(a1 == a2 && a1 > CSS_ATOM_EMPTY);
    ASSERT_STR_EQ("foo-bar", css_atom_str(a1));
    ASSERT_TRUE(css_atom_intern("", 0) == CSS_ATOM_EMPTY);
    ASSERT_TRUE(css_atom_find("never-interned-anywhere-xyz", 27) == CSS_ATOM_NONE);
    job_graph_run(job_new(atom_root, NULL, NULL, 0, 0));
    for (int t = 1; t < 8; t++)
        for (int w = 0; w < 8; w++) ASSERT_TRUE(g_atom_ids[t][w] == g_atom_ids[0][w]);
    ASSERT_TRUE(g_atom_ids[0][1] == css_atom_intern("span", 4));
    return true;
}

/* ---------------------------------------------------------------------------
 * Compiled matcher vs the legacy string matcher
 * ------------------------------------------------------------------------- */

static const char *k_tags[] = { "div", "p", "span", "a", "ul", "li", "section", "b", "h2", "td" };
static const char *k_cls[] = { "c0", "c1", "c2", "c3", "c4", "Mixed", "x-y", "c7" };

static void gen_compound(Buf *b)
{
    bool any = false;
    if (rnd_n(2)) { buf_add(b, "%s", rnd_n(8) ? k_tags[rnd_n(10)] : "*"); any = true; }
    for (uint32_t k = rnd_n(3); k > 0; k--) { buf_add(b, ".%s", k_cls[rnd_n(8)]); any = true; }
    if (rnd_n(12) == 0) { buf_add(b, "#id%u", rnd_n(6)); any = true; }
    if (rnd_n(10) == 0) { buf_add(b, "%s", rnd_n(2) ? "[href]" : "[title]"); any = true; }
    if (rnd_n(4) == 0) {
        static const char *ps[] = { ":first-child", ":last-child", ":only-child", ":empty",
            ":nth-child(2n+1)", ":nth-child(3)", ":nth-last-child(2)", ":first-of-type",
            ":last-of-type", ":only-of-type", ":nth-of-type(even)", ":not(.c1)", ":not(span.c2)",
            ":hover", ":link", ":root", ":where(.c3)", ":is(p, .c4)", ":where(div > .c0)",
            "::before", ":after" };
        buf_add(b, "%s", ps[rnd_n(sizeof(ps) / sizeof(ps[0]))]);
        any = true;
    }
    if (!any) buf_add(b, "%s", k_tags[rnd_n(10)]);
}

static void gen_selector(Buf *b)
{
    int parts = (int)rnd_n(4) + 1;
    for (int i = 0; i < parts; i++) {
        if (i) {
            static const char *combs[] = { " ", " > ", " ", " + ", " ~ ", ">" };
            buf_add(b, "%s", combs[rnd_n(6)]);
        }
        gen_compound(b);
    }
}

static void gen_css(Buf *b, int rules)
{
    for (int r = 0; r < rules; r++) {
        bool media = rnd_n(8) == 0;
        if (media) buf_add(b, "@media (%s-width: %upx) { ", rnd_n(2) ? "min" : "max", 400 + rnd_n(4) * 300);
        int n = (int)rnd_n(3) + 1;
        for (int k = 0; k < n; k++) { if (k) buf_add(b, ", "); gen_selector(b); }
        buf_add(b, " { color: #%06x; margin-left: %upx }", rnd() & 0xFFFFFF, rnd_n(20));
        if (media) buf_add(b, " }");
        buf_add(b, "\n");
    }
}

static void gen_dom(Buf *b, int depth, int *budget)
{
    int kids = depth < 3 ? (int)rnd_n(5) + 1 : (depth < 7 ? (int)rnd_n(3) : 0);
    for (int i = 0; i < kids && *budget > 0; i++) {
        if (rnd_n(4) == 0) { buf_add(b, "text "); continue; }
        const char *t = k_tags[rnd_n(10)];
        (*budget)--;
        buf_add(b, "<%s", t);
        if (rnd_n(3)) {
            buf_add(b, " class=\"%s", k_cls[rnd_n(8)]);
            if (rnd_n(2)) buf_add(b, " %s", k_cls[rnd_n(8)]);
            buf_add(b, "\"");
        }
        if (rnd_n(8) == 0) buf_add(b, " id=\"ID%u\"", rnd_n(6));
        if (rnd_n(6) == 0) buf_add(b, " href=\"#\"");
        if (rnd_n(9) == 0) buf_add(b, " title=\"t\"");
        buf_add(b, ">");
        gen_dom(b, depth + 1, budget);
        buf_add(b, "</%s>", t);
    }
}

TEST(test_compiled_matcher_equals_legacy) {
    int checked = 0, matched = 0;
    for (int seed = 1; seed <= 12; seed++) {
        g_rng = 0x9E3779B9u * (uint32_t)seed;
        Buf css = {0}, html = {0};
        gen_css(&css, 120);
        buf_add(&html, "<html><body>");
        int budget = 300;
        gen_dom(&html, 0, &budget);
        buf_add(&html, "</body></html>");

        CssStylesheet *sheet = css_stylesheet_parse(css.s, css.n);
        ASSERT_TRUE(sheet != NULL);
        HtmlDocument *doc = html_parse(html.s, html.n);
        ASSERT_TRUE(doc != NULL);
        double vw = (seed & 1) ? 1024.0 : 500.0;
        CssStyleSet *set = css_styleset_build(&sheet, 1, vw, 800.0);
        CssStyleDoc *sd = css_styledoc_build(doc);
        CssStyleScratch *t = css_style_scratch_new(set);
        int n = (int)po_array_count(&doc->array);
        int *seen = calloc((size_t)sheet->rule_count + 1, sizeof(int));
        for (int dom = 0; dom < n; dom++) {   /* document order = preorder here */
            HtmlNode *node = po_array_payload(&doc->array, dom);
            if (!node || node->type != HTML_NODE_ELEMENT) {
                css_style_bloom_push(t, sd, dom);
                continue;
            }
            css_style_bloom_enter(t, sd, po_array_parent(&doc->array, dom));
            const CssAppliedDecl *ad = NULL;
            int na = css_style_match(set, sd, dom, t, &ad);
            memset(seen, 0, (size_t)sheet->rule_count * sizeof(int));
            for (int k = 0; k < na; k++) seen[ad[k].rule] = ad[k].specificity + 1;
            for (int r = 0; r < sheet->rule_count; r++) {
                CssRule *rule = &sheet->rules[r];
                bool legacy = css_rule_media_matches(rule, vw, 800.0) &&
                              css_rule_matches(rule, doc, node);
                if (legacy != (seen[r] != 0)) {
                    printf("    mismatch seed=%d node=%d rule='%s' legacy=%d\n",
                           seed, dom, rule->selector_text, legacy);
                    return false;
                }
                if (legacy) {
                    int spec = css_specificity_from_selector_text_matching(rule->selector_text, doc, node);
                    ASSERT_EQ(spec, seen[r] - 1);
                    matched++;
                }
                checked++;
            }
            /* Cascade order: sorted by the legacy comparator. */
            for (int k = 1; k < na; k++) ASSERT_TRUE(css_applied_decl_compare(&ad[k - 1], &ad[k]) < 0);
            css_style_bloom_push(t, sd, dom);
        }
        free(seen);
        css_style_scratch_free(t);
        css_styledoc_free(sd);
        css_styleset_free(set);
        css_stylesheet_free(sheet);
        html_document_free(doc);
        free(css.s);
        free(html.s);
    }
    printf("    %d (rule, element) pairs checked, %d matches\n", checked, matched);
    ASSERT_TRUE(matched > 100);
    return true;
}

/* ---------------------------------------------------------------------------
 * Chunked parallel parsing vs the serial parser
 * ------------------------------------------------------------------------- */

static bool sheets_equal(const CssStylesheet *a, const CssStylesheet *b)
{
    int na = a ? a->rule_count : 0, nb = b ? b->rule_count : 0;
    if (na != nb) { printf("    rule count %d vs %d\n", na, nb); return false; }
    for (int r = 0; r < na; r++) {
        const CssRule *x = &a->rules[r], *y = &b->rules[r];
        if (strcmp(x->selector_text ? x->selector_text : "", y->selector_text ? y->selector_text : "")) return false;
        if (strcmp(x->media_query ? x->media_query : "", y->media_query ? y->media_query : "")) return false;
        if (x->declaration_count != y->declaration_count) return false;
        for (int d = 0; d < x->declaration_count; d++) {
            if (strcmp(x->declarations[d].property, y->declarations[d].property)) return false;
            if (strcmp(x->declarations[d].value, y->declarations[d].value)) return false;
            if (x->declarations[d].important != y->declarations[d].important) return false;
        }
    }
    return true;
}

TEST(test_parallel_parse_equals_serial) {
    for (int seed = 1; seed <= 40; seed++) {
        g_rng = 0x51ED270Bu * (uint32_t)seed;
        Buf css = {0};
        for (int r = 0; r < 60; r++) {
            switch (rnd_n(9)) {
            case 0: buf_add(&css, "/* comment with { braces } and \"quotes */ "); break;
            case 1: buf_add(&css, "a[title=\"}{\"] { content: \"}\"; color: red }\n"); break;
            case 2: buf_add(&css, "@media (min-width: 600px) { .m%u { margin: 1px } @media print { p { x: y } } }\n", rnd_n(9)); break;
            case 3: buf_add(&css, "} stray } "); break;
            case 4: buf_add(&css, "@import url(x.css);\n@font-face { font-family: F; src: url(a{b}.woff) }\n"); break;
            case 5: buf_add(&css, ".bad { color: red; ");  break;   /* unterminated block */
            case 6: buf_add(&css, "div { background: url(data:image/png;base64,AA}AA) }\n"); break;
            case 7: buf_add(&css, "'unterminated string { "); break;
            default: gen_selector(&css); buf_add(&css, " { padding: %upx !important; --v: a{b} }\n", rnd_n(9)); break;
            }
        }
        CssStylesheet *serial = css_stylesheet_parse(css.s, css.n);
        size_t chunks[] = { 1, 7, 33, 128, 1000, 0 };
        for (size_t c = 0; c < sizeof(chunks) / sizeof(chunks[0]); c++) {
            CssStylesheet *par = css_layout_parse_sheet_parallel(css.s, css.n, chunks[c]);
            if (!sheets_equal(serial, par)) {
                printf("    seed=%d chunk=%zu differs\n", seed, chunks[c]);
                return false;
            }
            css_stylesheet_free(par);
        }
        css_stylesheet_free(serial);
        free(css.s);
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * Text measurement from many threads
 * ------------------------------------------------------------------------- */

static const char *k_texts[] = {
    "Hello, world", "The quick brown fox jumps over the lazy dog", "Ünïcödé — “quotes” and ‘more’",
    "AVAWAYToTaTe kerning pairs", "1234567890 !@#$%^&*()", "comic-book long-hyphen-word wrap test",
    "日本語のテキスト", "e\xcc\x81\xcc\x82 combining", "tabs\tand  spaces   ", "x"
};
#define N_TEXTS (int)(sizeof(k_texts) / sizeof(k_texts[0]))
static float g_meas[16][N_TEXTS];
static int g_meas_lines[16][N_TEXTS];

static void measure_job(Job *j)
{
    TextShaper *f = display_list_get_default_font();
    for (int i = 0; i < N_TEXTS; i++) {
        float w = 0, h = 0;
        text_shaper_measure(f, k_texts[i], &w, &h);
        TsWrapResult wr;
        memset(&wr, 0, sizeof(wr));
        text_shaper_wrap_measure(f, k_texts[i], 0, 0, 40, 60, 1.0f, 20.0f, 0, 0, 0, &wr);
        g_meas[j->b][i] = w + wr.max_width;
        g_meas_lines[j->b][i] = wr.lines;
    }
}

static void measure_root(Job *j)
{
    for (uint32_t t = 0; t < 16; t++) job_spawn(job_new(measure_job, j, NULL, 0, t));
}

TEST(test_text_measure_threadsafe) {
    TextShaper *f = display_list_get_default_font();
    if (!f) { printf("    (skipped - no font found)"); return true; }
    job_graph_run(job_new(measure_root, NULL, NULL, 0, 0));
    for (int t = 1; t < 16; t++)
        for (int i = 0; i < N_TEXTS; i++) {
            ASSERT_TRUE(g_meas[t][i] == g_meas[0][i]);
            ASSERT_TRUE(g_meas_lines[t][i] == g_meas_lines[0][i]);
        }
    return true;
}

/* ---------------------------------------------------------------------------
 * Layout determinism across scheduling and caching switches
 * ------------------------------------------------------------------------- */

static char *layout_signature(const char *html, const char *css_extra)
{
    Buf page = {0};
    buf_add(&page, "<html><head><style>%s</style></head><body>%s</body></html>", css_extra, html);
    HtmlDocument *doc = html_parse(page.s, page.n);
    free(page.s);
    if (!doc) return NULL;
    LayoutContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    if (!css_layout_run(&ctx, doc, NULL, 900.0, 1200.0)) { html_document_free(doc); return NULL; }
    Buf sig = {0};
    for (int i = 0; i < ctx.tree.count; i++) {
        LayoutBox *b = &ctx.boxes[i];
        buf_add(&sig, "%d %.6f %.6f %.6f %.6f %.4f %d %d %.4f %.4f %.4f %.4f %d %s|", i,
                b->x, b->y, b->width, b->height, b->font_size, (int)b->display, b->font_weight,
                b->color_r, b->color_g, b->margin_left, b->padding_top, (int)b->visibility,
                b->font_family);
    }
    for (int i = 0; i < (int)po_array_count(&doc->array); i++) {
        HtmlNode *n = po_array_payload(&doc->array, i);
        if (n && n->type == HTML_NODE_TEXT && n->text_content) buf_add(&sig, "T%s|", n->text_content);
    }
    css_layout_tree_free(&ctx);
    html_document_free(doc);
    return sig.s;
}

TEST(test_layout_deterministic_across_switches) {
    g_rng = 4242;
    Buf css = {0}, html = {0};
    gen_css(&css, 150);
    buf_add(&css, ".fx { display: flex } .fx > * { flex: 1 } .fl { float: left; width: 30px }"
                  " .hl li::after { content: \" | \" } :root { --w: 12px } .v { padding: var(--w) }");
    for (int s = 0; s < 6; s++) {
        int budget = 120;
        buf_add(&html, "<div class=\"fx c%d\">", s);
        gen_dom(&html, 1, &budget);
        buf_add(&html, "</div><ul class=\"hl\"><li>a</li> <li>b</li> <li>c</li></ul>"
                       "<table><tr><td>x</td><td class=\"v\">y y y</td></tr></table>");
        if (s == 3) buf_add(&html, "<div><div class=\"fl\">f</div>text around a float</div>");
    }
    char *base = layout_signature(html.s, css.s);
    ASSERT_TRUE(base != NULL);
    static const char *cfgs[][2] = {
        { "CSS_SHARE", "0" }, { "CSS_GRAIN", "1" }, { "CSS_LAYOUT_GRAIN", "1" },
        { "CSS_LAYOUT_PAR", "0" }, { "CSS_SHEET_CHUNK", "13" }, { "CSS_SHEET_CACHE", "0" },
    };
    for (size_t c = 0; c < sizeof(cfgs) / sizeof(cfgs[0]); c++) {
        setenv(cfgs[c][0], cfgs[c][1], 1);
        char *sig = layout_signature(html.s, css.s);
        unsetenv(cfgs[c][0]);
        ASSERT_TRUE(sig != NULL);
        if (strcmp(base, sig) != 0) {
            printf("    layout differs with %s=%s\n", cfgs[c][0], cfgs[c][1]);
            free(sig);
            return false;
        }
        free(sig);
    }
    /* Repeated layouts hit the sheet and style-set caches: same result. */
    for (int rep = 0; rep < 3; rep++) {
        char *sig = layout_signature(html.s, css.s);
        ASSERT_TRUE(sig && strcmp(base, sig) == 0);
        free(sig);
    }
    free(base);
    free(css.s);
    free(html.s);
    return true;
}

void run_css_style_tests(void) {
    printf("\n--- Parallel style and layout tests ---\n");
    printf("    job threads: %d\n", job_thread_count());
    RUN_TEST(test_job_continuations_tree);
    RUN_TEST(test_job_groups_nested);
    RUN_TEST(test_job_graphs_back_to_back);
    RUN_TEST(test_frame_arena);
    RUN_TEST(test_atoms_case_and_concurrency);
    RUN_TEST(test_compiled_matcher_equals_legacy);
    RUN_TEST(test_parallel_parse_equals_serial);
    RUN_TEST(test_text_measure_threadsafe);
    RUN_TEST(test_layout_deterministic_across_switches);
}

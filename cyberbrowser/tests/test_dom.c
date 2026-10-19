/*
 * Node store DOM tests (PARALLEL_DOM.md §10).
 *
 *   - parser equivalence with the legacy parser (fixtures and generated
 *     malformed markup), and the same tree for every chunk size;
 *   - lazy wrappers: identity, expandos across GC, tree-level lifetime;
 *   - queries and indexes;
 *   - the script fetch pool;
 *   - the render snapshot (reuse when unchanged, dirty bits cleared).
 */
#include "test_runner.h"
#include <quickjs.h>
#include <quickjs_gc_unified.h>
#include <time.h>
#include "html_dom.h"
#include "dom_store.h"
#include "dom_parse.h"
#include "dom_wrap.h"
#include "dom_js.h"
#include "dom_query.h"
#include "dom_fetch.h"
#include "css_style.h"
#include "legacy_html_parse.h"

extern JSContextHandle get_shared_test_context(void);

/* ============================================================================
 * Tree dumps (kinds, tags, attributes in order, text)
 * ============================================================================ */

typedef struct { char *p; size_t n, cap; } Buf;
static void bput(Buf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        b->p = realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void bstr(Buf *b, const char *s) { bput(b, s, strlen(s)); }
static void besc(Buf *b, const char *s, size_t n) {
    char t[8];
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 32 || c > 126 || c == '\\') { snprintf(t, sizeof t, "\\x%02x", c); bstr(b, t); }
        else bput(b, (const char *)&c, 1);
    }
}
static void bindent(Buf *b, int d) { for (int i = 0; i < d; i++) bstr(b, "  "); }

static void dump_new(Buf *o, HtmlDocument *d, int idx, int dep) {
    for (; idx >= 0; idx = po_array_next_sibling(&d->array, idx)) {
        HtmlNode *n = po_array_payload(&d->array, idx);
        bindent(o, dep);
        if (n->type == HTML_NODE_TEXT) { bstr(o, "T:"); besc(o, n->text_content, n->text_len); bstr(o, "\n"); continue; }
        bstr(o, "E:"); bstr(o, n->tag_name);
        for (HtmlAttribute *a = n->attributes; a; a = a->next) {
            bstr(o, " ["); besc(o, a->name, strlen(a->name)); bstr(o, "="); besc(o, a->value, strlen(a->value)); bstr(o, "]");
        }
        if (n->text_content) { bstr(o, " R:"); besc(o, n->text_content, n->text_len); }
        bstr(o, "\n");
        dump_new(o, d, po_array_first_child(&d->array, idx), dep + 1);
    }
}

static void dump_old(Buf *o, LgDoc *d, int idx, int dep) {
    for (; idx >= 0; idx = po_array_next_sibling(&d->array, idx)) {
        LgNode *n = po_array_payload(&d->array, idx);
        if (!po_array_is_active(&d->array, idx)) continue;
        bindent(o, dep);
        if (n->type == LG_NODE_TEXT) { bstr(o, "T:"); besc(o, n->text_content, n->text_len); bstr(o, "\n"); continue; }
        bstr(o, "E:"); bstr(o, n->tag_name);
        for (LgAttr *a = n->attributes; a; a = a->next) {
            char nm[LG_MAX_ATTR_NAME_LEN];
            snprintf(nm, sizeof nm, "%s", a->name);
            for (char *p = nm; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
            bstr(o, " ["); besc(o, nm, strlen(nm)); bstr(o, "="); besc(o, a->value, strlen(a->value)); bstr(o, "]");
        }
        if (n->text_content) { bstr(o, " R:"); besc(o, n->text_content, n->text_len); }
        bstr(o, "\n");
        dump_old(o, d, po_array_first_child(&d->array, idx), dep + 1);
    }
}

static char *dump_html_parse(const char *html, size_t len) {
    Buf b = { 0 };
    bstr(&b, "");
    HtmlDocument *d = html_parse(html, len);
    if (d) {
        char t[64];
        snprintf(t, sizeof t, "roots %d %d %d\n", d->root_idx >= 0, d->head_idx >= 0, d->body_idx >= 0);
        bstr(&b, t);
        dump_new(&b, d, d->array.first_root, 0);
        html_document_free(d);
    }
    return b.p;
}

static char *dump_legacy(const char *html, size_t len) {
    Buf b = { 0 };
    bstr(&b, "");
    LgDoc *d = lg_parse(html, len);
    if (d) {
        char t[64];
        snprintf(t, sizeof t, "roots %d %d %d\n", d->root_idx >= 0, d->head_idx >= 0, d->body_idx >= 0);
        bstr(&b, t);
        dump_old(&b, d, d->array.first_root, 0);
        lg_document_free(d);
    }
    return b.p;
}

static bool same_as_legacy(const char *html, size_t len, const char *what) {
    char *a = dump_legacy(html, len), *b = dump_html_parse(html, len);
    bool ok = strcmp(a, b) == 0;
    if (!ok) {
        size_t i = 0;
        while (a[i] && a[i] == b[i]) i++;
        size_t s = i > 80 ? i - 80 : 0;
        printf("    %s: trees differ at byte %zu\n    legacy: %.160s\n    new:    %.160s\n", what, i, a + s, b + s);
    }
    free(a); free(b);
    return ok;
}

/* Deterministic generator of malformed markup (the fragments the parser
 * fuzzing used: unclosed tags, stray end tags, '<' in values, comments,
 * CRLF, entities, raw text containing "</scrip"). */
static const char *const FRAGS[] = {
    "<div>", "</div>", "<p class=\"a b\">", "</p>", "<span id=x>", "</span>", "<b>", "</b>", "</bx>",
    "<a href=\"x&amp;y\">", "</a>", "<br>", "<br/>", "<img src=a.png/>",
    "<script>", "</script>", "</scrip", "<script>var a=\"</div>\";</script>", "<style>p{}</style>", "<style/>",
    "<textarea>x<y</textarea>", "<title>T</TITLE>",
    "<!--", "-->", "<!-- c -->", "<!doctype html>", "<!DOCTYPE x>", "< div>", "<>", "< >", "</>", "<", ">",
    "&amp;", "&lt;", "&#65;", "&#300;", "&#0;", "&nbsp;", "&foo;", "&#x41;",
    "text", " ", "  \n\t ", "\r\n", "x y  z", "<x-foo a=1 b=\"2\" c='3' d>", "</x-foo>", "<a b=c/d>",
    "<a b=\"unterminated", "<a b='q'x>", "<a =x>", "<a / b>",
    "<html>", "</html>", "<head>", "</head>", "<body>", "</body>", "<li>", "<ul>", "</ul>",
    "<table><tr><td>", "</td></tr></table>", "<?xml v?>", "<!x>", "</x <y>",
    "<A HREF=Q>", "</A>", "<DIV CLASS=Z>", "</Div>", "\v", "\f", "<a\vb=1>", "<svg viewBox=\"0 0 1 1\">",
    "</svg>", "<input value=\"a>b\">", "</li >", "</ div>",
};
static uint32_t rng_next(uint32_t *s) { *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5; return *s; }

static char *gen_case(uint32_t seed, size_t *len) {
    uint32_t s = seed * 2654435761u + 1;
    Buf b = { 0 };
    bstr(&b, "");
    if (rng_next(&s) % 10 < 3) bstr(&b, " \n");
    int n = 1 + (int)(rng_next(&s) % 120);
    for (int i = 0; i < n; i++) bstr(&b, FRAGS[rng_next(&s) % (sizeof FRAGS / sizeof FRAGS[0])]);
    if (rng_next(&s) % 10 == 0) {
        for (int i = 0; i < 300; i++) bstr(&b, "<div>");
        bstr(&b, "x");
        for (int i = 0; i < 300; i++) bstr(&b, "</div>");
    }
    *len = b.n;
    return b.p;
}

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    fclose(f);
    b[n] = 0;
    *len = (size_t)n;
    return b;
}

/* ============================================================================
 * Parser tests
 * ============================================================================ */

TEST(test_dom_parse_matches_legacy_fixtures) {
    static const char *const cases[] = {
        "",
        "plain text",
        "<html><head><title>T</title></head><body><p>a<b>b</b>c</p></body></html>",
        "<div><p>unclosed<p>again</div>tail",
        "</div>stray<span>x</span></span>",
        "<a href='x' href=\"y\" HREF=z>dup</a>",
        "<script>if (a < b && c > d) { s = '</div>'; }</script><p>after</p>",
        "<script>x = '</scrip'; y = 1</script>",
        "<style>a > b { c: d }</style><textarea><b>raw</b></textarea>",
        "<!-- comment --><div><!-- x --> y </div><!-- unterminated",
        "<p>&amp; &lt; &gt; &quot; &#65; &#x42; &nbsp; &bogus; &#0;</p>",
        "<div\r\nclass=\"a\r\nb\">line\r\nbreak</div>",
        "<br><br/><img src=a.png/><input value=\"a>b\">",
        "<x-foo a=1 b=\"2\" c='3' d><x-bar></x-bar></x-foo>",
        "<ul><li>one<li>two</ul>",
        "<table><tr><td>1</td><td>2</td></tr></table>",
        "<svg viewBox=\"0 0 1 1\"><path d=\"M0 0\"/></svg>",
        "<body><div></body></html><p>after body</p>",
        "<head><meta charset=utf-8></head>text<body class=b>body</body>",
        "< div>not a tag</ div><>x</>",
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char what[32];
        snprintf(what, sizeof what, "fixture %zu", i);
        ASSERT_TRUE(same_as_legacy(cases[i], strlen(cases[i]), what));
    }
    /* Deep nesting is capped at the legacy depth. */
    Buf b = { 0 };
    bstr(&b, "");
    for (int i = 0; i < 400; i++) bstr(&b, "<div>");
    bstr(&b, "deep");
    for (int i = 0; i < 400; i++) bstr(&b, "</div>");
    ASSERT_TRUE(same_as_legacy(b.p, b.n, "deep nesting"));
    free(b.p);
    /* A real page, when the test data is present. */
    size_t len = 0;
    char *page = read_file("youtube_data/youtube_page.html", &len);
    if (page) {
        ASSERT_TRUE(same_as_legacy(page, len, "youtube_page.html"));
        free(page);
    }
    return true;
}

TEST(test_dom_parse_matches_legacy_fuzz) {
    for (uint32_t seed = 1; seed <= 400; seed++) {
        size_t len = 0;
        char *html = gen_case(seed, &len);
        char what[32];
        snprintf(what, sizeof what, "fuzz seed %u", seed);
        bool ok = same_as_legacy(html, len, what);
        free(html);
        ASSERT_TRUE(ok);
    }
    return true;
}

/* Every chunk size gives the serial tree (chunk boundaries forced at bad
 * places, inside scripts and comments). */
TEST(test_dom_parse_chunked_same_tree) {
    Buf b = { 0 };
    bstr(&b, "<html><head><style>p { color: red }</style></head><body>");
    for (int i = 0; i < 200; i++) {
        char t[256];
        snprintf(t, sizeof t, "<div class=\"c%d\" id=\"d%d\"><p>para %d &amp; more</p>"
                 "<script>var s%d = '<div>' + \"</di\" + 'v>';</script><!-- note %d --></div>",
                 i % 7, i, i, i, i);
        bstr(&b, t);
    }
    bstr(&b, "</body></html>");
    dom_parse_set_chunking(0, 0);
    char *serial = dump_html_parse(b.p, b.n);
    static const size_t sizes[] = { 1, 7, 64, 333, 1000, 4096 };
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        dom_parse_set_chunking(sizes[i], 1);
        char *c = dump_html_parse(b.p, b.n);
        DomParseStats st;
        dom_parse_last_stats(&st);
        bool same = strcmp(serial, c) == 0;
        if (!same) printf("    chunk %zu: tree differs (%u chunks, %u redone)\n", sizes[i], st.chunks, st.chunks_redone);
        free(c);
        if (!same) { dom_parse_set_chunking(0, -1); free(serial); free(b.p); return false; }
    }
    /* Prescan boundaries (not forced). */
    dom_parse_set_chunking(512, 0);
    char *c = dump_html_parse(b.p, b.n);
    bool same = strcmp(serial, c) == 0;
    free(c);
    dom_parse_set_chunking(0, -1);
    free(serial);
    free(b.p);
    ASSERT_TRUE(same);
    /* The parsed tree is also the legacy parser's. */
    return true;
}

TEST(test_dom_parse_no_length_limits) {
    /* The legacy parser cut names at 128 and values at 4096 bytes; the store
     * keeps them whole. */
    size_t vlen = 10000;
    char *html = malloc(vlen + 64);
    int n = sprintf(html, "<div data-big=\"");
    memset(html + n, 'v', vlen);
    strcpy(html + n + vlen, "\">x</div>");
    NodeId frag = dom_parse_html(html, strlen(html), DOM_NULL);
    NodeId div = dom_first_child(frag);
    ASSERT_TRUE(dom_kind(div) == DOM_K_ELEMENT);
    const char *v = dom_attr(div, "data-big");
    ASSERT_TRUE(v != NULL);
    ASSERT_EQ(vlen, strlen(v));
    dom_free_subtree(frag);
    free(html);
    return true;
}

/* ============================================================================
 * JS helpers
 * ============================================================================ */

static int32_t js_int(JSContextHandle ctx, const char *src) {
    GCValue v = JS_Eval(ctx, src, strlen(src), "<test_dom>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        GCValue e = JS_GetException(ctx);
        const char *s = JS_ToCString(ctx, e);
        printf("    JS exception: %s\n    in: %.200s\n", s ? s : "?", src);
        return -999999;
    }
    int32_t r = 0;
    if (JS_IsBool(v)) r = JS_ToBool(ctx, v);
    else JS_ToInt32(ctx, &r, v);
    return r;
}

static void full_gc(JSContextHandle ctx) {
    for (int i = 0; i < 3; i++) JS_RunGC(JS_GetRuntime(ctx));
}

static bool dom_ready(JSContextHandle ctx) {
    return SAFE_VALID(ctx) && js_int(ctx, "typeof document === 'object' && !!document.body") == 1;
}

/* ============================================================================
 * Wrapper tests
 * ============================================================================ */

TEST(test_dom_wrapper_identity) {
    JSContextHandle ctx = get_shared_test_context();
    if (!dom_ready(ctx)) { printf("    (skipped - no document)\n"); return true; }
    ASSERT_EQ(1, js_int(ctx,
        "(function(){var d=document.createElement('div'); d.id='wid1';"
        "d.innerHTML='<p class=q>a</p><p>b</p>'; document.body.appendChild(d);"
        "var a=document.getElementById('wid1'), b=document.getElementById('wid1');"
        "var p1=d.firstChild, p2=d.querySelector('p.q'), p3=document.getElementsByClassName('q')[0];"
        "var ok = a===b && a===d && p1===p2 && p2===p3 && p1.parentNode===d && p1.nextSibling.previousSibling===p1;"
        "d.remove(); return ok;})()"));
    return true;
}

TEST(test_dom_wrappers_are_lazy) {
    JSContextHandle ctx = get_shared_test_context();
    if (!dom_ready(ctx)) { printf("    (skipped - no document)\n"); return true; }
    ASSERT_EQ(1, js_int(ctx, "globalThis.__lz = document.createElement('div'); 1"));
    uint32_t live0 = g_dom.live, w0 = dom_wrap_count();
    ASSERT_EQ(1, js_int(ctx, "__lz.innerHTML = '<span><b>x</b></span>'.repeat(500); 1"));
    /* 1500 nodes in the store, no wrappers for them yet. */
    ASSERT_TRUE(g_dom.live >= live0 + 1500);
    ASSERT_TRUE(dom_wrap_count() <= w0 + 2);
    ASSERT_EQ(500, js_int(ctx, "__lz.querySelectorAll('span').length"));
    ASSERT_TRUE(dom_wrap_count() >= w0 + 500);
    ASSERT_EQ(1, js_int(ctx, "__lz = null; 1"));
    return true;
}

TEST(test_dom_expandos_survive_gc) {
    JSContextHandle ctx = get_shared_test_context();
    if (!dom_ready(ctx)) { printf("    (skipped - no document)\n"); return true; }
    ASSERT_EQ(1, js_int(ctx,
        "(function(){var c=document.createElement('section'); c.id='xp1';"
        "c.innerHTML='<i></i><i></i>'; c.lastChild.myExpando={v:41};"
        "c.myFlag='kept'; document.body.appendChild(c); return 1;})()"));
    for (int round = 0; round < 3; round++) {
        ASSERT_EQ(1, js_int(ctx, "(function(){var g=[]; for(var i=0;i<20000;i++) g.push({i:i}); return 1;})()"));
        full_gc(ctx);
    }
    ASSERT_EQ(1, js_int(ctx,
        "(function(){var c=document.getElementById('xp1');"
        "var ok = c.myFlag==='kept' && c.lastChild.myExpando.v===41;"
        "c.remove(); return ok;})()"));
    return true;
}

TEST(test_dom_detached_tree_freed) {
    JSContextHandle ctx = get_shared_test_context();
    if (!dom_ready(ctx)) { printf("    (skipped - no document)\n"); return true; }
    full_gc(ctx);
    uint32_t live0 = g_dom.live;
    ASSERT_EQ(1, js_int(ctx,
        "(function(){for(var k=0;k<5;k++){var d=document.createElement('div');"
        "d.innerHTML='<p>a<b>b</b></p>'.repeat(100); d.firstChild.tag=k;} return 1;})()"));
    ASSERT_TRUE(g_dom.live >= live0 + 5 * 400);
    full_gc(ctx);
    /* Every detached tree died with its wrappers. */
    ASSERT_TRUE(g_dom.live <= live0 + 16);
    return true;
}

TEST(test_dom_unwrapped_removal_frees_now) {
    JSContextHandle ctx = get_shared_test_context();
    if (!dom_ready(ctx)) { printf("    (skipped - no document)\n"); return true; }
    ASSERT_EQ(1, js_int(ctx,
        "globalThis.__fr = document.createElement('div');"
        "__fr.innerHTML = '<em></em>'.repeat(300); 1"));
    uint32_t live0 = g_dom.live;
    ASSERT_EQ(1, js_int(ctx, "__fr.innerHTML = ''; 1"));
    ASSERT_TRUE(g_dom.live + 300 <= live0);
    ASSERT_EQ(1, js_int(ctx, "__fr = null; 1"));
    return true;
}

TEST(test_dom_child_keeps_parent_tree) {
    JSContextHandle ctx = get_shared_test_context();
    if (!dom_ready(ctx)) { printf("    (skipped - no document)\n"); return true; }
    ASSERT_EQ(1, js_int(ctx,
        "globalThis.__kid = (function(){var d=document.createElement('div');"
        "d.innerHTML='<span>x</span><b>y</b>'; d.marker=7; return d.firstChild;})(); 1"));
    for (int round = 0; round < 2; round++) {
        ASSERT_EQ(1, js_int(ctx, "(function(){var g=[]; for(var i=0;i<20000;i++) g.push([i]); return 1;})()"));
        full_gc(ctx);
    }
    ASSERT_EQ(1, js_int(ctx,
        "var p=__kid.parentNode; var ok = p && p.marker===7 && __kid.nextSibling.tagName==='B'"
        " && p.firstChild===__kid; __kid=null; ok"));
    return true;
}

/* Moving nodes between trees keeps them (and their expandos) alive. */
TEST(test_dom_move_between_trees) {
    JSContextHandle ctx = get_shared_test_context();
    if (!dom_ready(ctx)) { printf("    (skipped - no document)\n"); return true; }
    ASSERT_EQ(1, js_int(ctx,
        "globalThis.__a = document.createElement('div'); globalThis.__b = document.createElement('div');"
        "for (var i=0;i<50;i++){var s=document.createElement('span'); s.n=i; __a.appendChild(s);} 1"));
    for (int round = 0; round < 4; round++) {
        ASSERT_EQ(1, js_int(ctx,
            "(function(){var from=__a.firstChild?__a:__b, to=from===__a?__b:__a;"
            "while(from.firstChild) to.appendChild(from.firstChild); return 1;})()"));
        full_gc(ctx);
    }
    ASSERT_EQ(1, js_int(ctx,
        "(function(){var t=__a.firstChild?__a:__b, ok=t.childNodes.length===50;"
        "for(var i=0,c=t.firstChild;c;c=c.nextSibling,i++) ok=ok&&c.n===i;"
        "__a=__b=null; return ok;})()"));
    return true;
}

/* ============================================================================
 * Queries and indexes
 * ============================================================================ */

TEST(test_dom_query_selectors) {
    JSContextHandle ctx = get_shared_test_context();
    if (!dom_ready(ctx)) { printf("    (skipped - no document)\n"); return true; }
    ASSERT_EQ(1, js_int(ctx,
        "globalThis.__q = document.createElement('div'); __q.id='qroot'; document.body.appendChild(__q);"
        "__q.innerHTML = '<ul id=u1><li class=a>1</li><li>2</li><li class=a>3</li><li data-x=abc>4</li></ul>'"
        " + '<p>p1 <span class=s>s1</span></p><div><p>p2</p></div><span class=s>s2</span>'"
        " + '<input type=checkbox checked><input type=text disabled>'; 1"));
    ASSERT_EQ(2, js_int(ctx, "__q.querySelectorAll('li.a').length"));
    ASSERT_EQ(2, js_int(ctx, "__q.querySelectorAll('ul > li.a + li').length"));
    ASSERT_EQ(3, js_int(ctx, "__q.querySelectorAll('li.a ~ li').length"));
    ASSERT_EQ(2, js_int(ctx, "__q.querySelectorAll('li:not(.a)').length"));
    ASSERT_EQ(2, js_int(ctx, "__q.querySelectorAll('li:nth-child(2n+1)').length"));
    ASSERT_EQ(1, js_int(ctx, "__q.querySelectorAll('li:last-child[data-x^=ab]').length"));
    /* Ancestors outside the scope count for combinators (__q is a div). */
    ASSERT_EQ(2, js_int(ctx, "__q.querySelectorAll('div p').length"));
    ASSERT_EQ(1, js_int(ctx, "__q.querySelectorAll(':scope div p').length"));
    ASSERT_EQ(1, js_int(ctx, "__q.querySelectorAll('p > span.s').length"));
    ASSERT_EQ(1, js_int(ctx, "__q.querySelectorAll('input:checked').length"));
    ASSERT_EQ(1, js_int(ctx, "__q.querySelectorAll('input:disabled').length"));
    ASSERT_EQ(1, js_int(ctx, "__q.querySelectorAll(':is(ul) > li:first-child').length"));
    /* Selector lists come back in document order. */
    ASSERT_EQ(1, js_int(ctx,
        "(function(){var r=__q.querySelectorAll('span.s, p'); var t=[];"
        "for(var i=0;i<r.length;i++) t.push(r[i].tagName+(r[i].textContent.slice(0,2)));"
        "return t.join(',')==='Pp1,SPANs1,Pp2,SPANs2';})()"));
    /* Element.querySelector does not test the element itself. */
    ASSERT_EQ(1, js_int(ctx, "__q.querySelector('div') !== __q"));
    ASSERT_EQ(1, js_int(ctx, "__q.querySelector('#qroot') === null"));
    ASSERT_EQ(1, js_int(ctx, "__q.firstChild.matches('ul#u1') && __q.querySelector('span').closest('p').tagName==='P'"));
    /* Unparsable selectors match nothing. */
    ASSERT_EQ(0, js_int(ctx, "(function(){try{return __q.querySelectorAll('li[').length}catch(e){return 0}})()"));
    ASSERT_EQ(1, js_int(ctx, "__q.remove(); __q=null; 1"));
    return true;
}

TEST(test_dom_index_tracks_mutations) {
    JSContextHandle ctx = get_shared_test_context();
    if (!dom_ready(ctx)) { printf("    (skipped - no document)\n"); return true; }
    /* Script-created elements are found once connected, and not after removal. */
    ASSERT_EQ(1, js_int(ctx,
        "(function(){var e=document.createElement('div'); e.id='idx-new'; e.className='idxc';"
        "var before=document.getElementById('idx-new')===null;"
        "document.body.appendChild(e);"
        "var found=document.getElementById('idx-new')===e && document.getElementsByClassName('idxc').length===1;"
        "e.className='idxd'; var moved=document.getElementsByClassName('idxc').length===0 &&"
        " document.getElementsByClassName('idxd')[0]===e;"
        "e.setAttribute('id','idx-renamed'); var renamed=document.getElementById('idx-new')===null &&"
        " document.getElementById('idx-renamed')===e;"
        "e.remove(); var gone=document.getElementById('idx-renamed')===null &&"
        " document.getElementsByTagName('div').length>=0 && document.getElementsByClassName('idxd').length===0;"
        "return before&&found&&moved&&renamed&&gone;})()"));
    return true;
}

/* Index lookups on a large document equal a brute-force scan, in document
 * order (the index build uses range jobs above 8192 nodes). */
TEST(test_dom_index_large_document) {
    JSContextHandle ctx = get_shared_test_context();
    if (!dom_ready(ctx)) { printf("    (skipped - no document)\n"); return true; }
    ASSERT_EQ(1, js_int(ctx,
        "globalThis.__big = document.createElement('div'); document.body.appendChild(__big);"
        "var h=[]; for(var i=0;i<6000;i++) h.push('<p class=\"k'+(i%5)+' all\"><i>'+i+'</i></p>');"
        "__big.innerHTML=h.join(''); 1"));
    DomQueryStats st0;
    dom_query_stats(&st0);
    DomNodeList l = dom_query_by_class(g_dom_document, "k3", 2);
    DomQueryStats st1;
    dom_query_stats(&st1);
    ASSERT_TRUE(st1.index_builds > st0.index_builds);
    /* Brute force over the document. */
    uint32_t r = dom_slot(g_dom_document), n = 0;
    bool same = true;
    for (uint32_t s = r; s; s = dom_next_in_subtree(s, r)) {
        if (g_dom.kind[s] != DOM_K_ELEMENT || !dom_has_class(dom_id_of(s), "k3", 2)) continue;
        if (n >= l.n || l.ids[n] != dom_id_of(s)) same = false;
        n++;
    }
    ASSERT_TRUE(same);
    ASSERT_EQ(n, l.n);
    ASSERT_EQ(1200, (int)l.n);
    dom_node_list_free(&l);
    DomNodeList t = dom_query_by_tag(g_dom_document, "i", 1);
    ASSERT_TRUE(t.n >= 6000);
    dom_node_list_free(&t);
    ASSERT_EQ(1200, js_int(ctx, "document.querySelectorAll('p.k3.all > i').length"));
    ASSERT_EQ(1, js_int(ctx, "__big.remove(); __big=null; 1"));
    return true;
}

/* ============================================================================
 * Script fetch pool
 * ============================================================================ */

static bool fake_fetch(const char *url, HttpBuffer *out, char *err, size_t err_len) {
    (void)err; (void)err_len;
    struct timespec ts = { 0, 60 * 1000000L };
    nanosleep(&ts, NULL);
    char body[256];
    int n = snprintf(body, sizeof body, "body of %s", url);
    out->data = malloc((size_t)n + 1);
    memcpy(out->data, body, (size_t)n + 1);
    out->size = (size_t)n;
    return true;
}

static double mono_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

TEST(test_dom_fetch_pool_overlaps_in_order) {
    dom_fetch_reset();
    dom_fetch_set_fetcher(fake_fetch);
    const int N = 6;
    char urls[6][64];
    double t0 = mono_ms();
    for (int i = 0; i < N; i++) {
        snprintf(urls[i], sizeof urls[i], "https://example.test/s%d.js", i);
        dom_fetch_prefetch(urls[i]);
    }
    for (int i = 0; i < N; i++) {
        HttpBuffer b = { 0 };
        char err[64];
        ASSERT_TRUE(dom_fetch_script(urls[i], &b, err, sizeof err));
        char want[128];
        snprintf(want, sizeof want, "body of %s", urls[i]);
        ASSERT_TRUE(b.data && strcmp(b.data, want) == 0);
        free(b.data);
    }
    double ms = mono_ms() - t0;
    DomFetchStats st;
    dom_fetch_stats(&st);
    ASSERT_EQ(N, (int)st.queued);
    ASSERT_EQ(N, (int)st.completed);
    ASSERT_EQ(0, (int)st.sync_fetches);
    /* Six 60 ms fetches overlapped: well under their 360 ms sum. */
    ASSERT_TRUE(ms < 300.0);
    /* An URL that was never prefetched is fetched synchronously. */
    HttpBuffer b = { 0 };
    char err[64];
    ASSERT_TRUE(dom_fetch_script("https://example.test/late.js", &b, err, sizeof err));
    free(b.data);
    dom_fetch_stats(&st);
    ASSERT_EQ(1, (int)st.sync_fetches);
    dom_fetch_reset();
    dom_fetch_set_fetcher(NULL);
    return true;
}

/* ============================================================================
 * Render snapshot
 * ============================================================================ */

TEST(test_dom_render_snapshot_reuse) {
    JSContextHandle ctx = get_shared_test_context();
    if (!dom_ready(ctx)) { printf("    (skipped - no document)\n"); return true; }
    GCValue global = JS_GetGlobalObject(ctx);
    GCValue doc = JS_GetPropertyStr(ctx, global, "document");
    uint64_t key = 0;
    bool unchanged = true;
    HtmlDocument *d1 = html_document_from_js_dom_if_changed(ctx, doc, &key, &unchanged);
    ASSERT_TRUE(d1 != NULL && !unchanged);
    ASSERT_EQ(0, dom_flags(g_dom_document) & DOMF_DIRTY_MASK);
    HtmlDocument *d2 = html_document_from_js_dom_if_changed(ctx, doc, &key, &unchanged);
    ASSERT_TRUE(d2 == NULL && unchanged);
    ASSERT_EQ(1, js_int(ctx, "globalThis.__rs=document.createElement('b'); __rs.textContent='snap'; document.body.appendChild(__rs); 1"));
    ASSERT_TRUE((dom_flags(g_dom_document) & DOMF_DIRTY_MASK) != 0);
    HtmlDocument *d3 = html_document_from_js_dom_if_changed(ctx, doc, &key, &unchanged);
    ASSERT_TRUE(d3 != NULL && !unchanged);
    /* The snapshot's elements know their store nodes. */
    bool found = false;
    for (size_t i = 0; i < po_array_count(&d3->array); i++) {
        HtmlNode *hn = po_array_payload(&d3->array, (int)i);
        if (hn && hn->type == HTML_NODE_ELEMENT && strcmp(hn->tag_name, "b") == 0 &&
            hn->store_id == dom_node_of(JS_GetPropertyStr(ctx, global, "__rs")))
            found = true;
    }
    ASSERT_TRUE(found);
    /* An inline style change is a render change even though no store
     * version moved. */
    ASSERT_EQ(1, js_int(ctx, "__rs.style.color='red'; 1"));
    HtmlDocument *d4 = html_document_from_js_dom_if_changed(ctx, doc, &key, &unchanged);
    ASSERT_TRUE(d4 != NULL && !unchanged);
    ASSERT_EQ(1, js_int(ctx, "__rs.remove(); __rs=null; 1"));
    html_document_free(d1);
    html_document_free(d3);
    html_document_free(d4);
    return true;
}

void run_dom_tests(void) {
    printf("\n--- Node store DOM Tests ---\n");
    RUN_TEST(test_dom_parse_matches_legacy_fixtures);
    RUN_TEST(test_dom_parse_matches_legacy_fuzz);
    RUN_TEST(test_dom_parse_chunked_same_tree);
    RUN_TEST(test_dom_parse_no_length_limits);
    RUN_TEST(test_dom_wrapper_identity);
    RUN_TEST(test_dom_wrappers_are_lazy);
    RUN_TEST(test_dom_expandos_survive_gc);
    RUN_TEST(test_dom_detached_tree_freed);
    RUN_TEST(test_dom_unwrapped_removal_frees_now);
    RUN_TEST(test_dom_child_keeps_parent_tree);
    RUN_TEST(test_dom_move_between_trees);
    RUN_TEST(test_dom_query_selectors);
    RUN_TEST(test_dom_index_tracks_mutations);
    RUN_TEST(test_dom_index_large_document);
    RUN_TEST(test_dom_fetch_pool_overlaps_in_order);
    RUN_TEST(test_dom_render_snapshot_reuse);
}

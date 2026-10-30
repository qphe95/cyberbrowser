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
#include "media/media_buffer.h"
#include "dom_fetch.h"
#include "css_style.h"
#include "browser/layout_api.h"
#include "legacy_html_parse.h"
#include "html_media_extract.h"

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
    "<table><tr><td>", "</td></tr></table>", "<!y>", "<!x>", "</x <y>",
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

/* A streamed document gets html, head and body where the markup leaves them
 * out (HTML §13.2.6.4.1-6): head content in the head, a later <body>'s
 * attributes on the body, </body> leaving it open. */
static bool streamed_as(const char *html, const char *want) {
    NodeId doc = dom_create_document();
    DomStream *st = dom_stream_begin(doc, doc);
    dom_stream_append(st, html, strlen(html));
    while (dom_stream_pump(st, true)) {}
    dom_stream_end(st);
    /* tag names, depth first: "html(head(meta,title),body(div))" */
    char out[256] = "";
    size_t n = 0;
    NodeId stack[16];
    int depth = 0;
    for (NodeId c = dom_first_child(doc); c; ) {
        if (dom_kind(c) == DOM_K_ELEMENT) {
            n += (size_t)snprintf(out + n, sizeof out - n, "%s%s", out[0] && out[n - 1] != '(' ? "," : "", dom_tag_name(c));
            if (dom_first_child(c) && depth < 16) {
                bool has_el = false;
                for (NodeId k = dom_first_child(c); k; k = dom_next_sibling(k)) if (dom_kind(k) == DOM_K_ELEMENT) has_el = true;
                if (has_el) { n += (size_t)snprintf(out + n, sizeof out - n, "("); stack[depth++] = c; c = dom_first_child(c); continue; }
            }
        }
        while (!dom_next_sibling(c) && depth) { n += (size_t)snprintf(out + n, sizeof out - n, ")"); c = stack[--depth]; }
        c = dom_next_sibling(c);
    }
    bool ok = strcmp(out, want) == 0;
    if (!ok) printf("    %s -> %s, want %s\n", html, out, want);
    dom_free_subtree(doc);
    return ok;
}

TEST(test_dom_stream_implies_html_head_body) {
    bool ok = true;
    ok &= streamed_as("<!doctype html><meta charset=utf-8><title>x</title><div>a</div>", "html(head(meta,title),body(div))");
    ok &= streamed_as("<!doctype html><body onload=\"f()\"><div>a</div>", "html(head,body(div))");
    ok &= streamed_as("<div>a</div>", "html(head,body(div))");
    ok &= streamed_as("<html><div>a</div>", "html(head,body(div))");
    ok &= streamed_as("<title>t</title><body class=b><i>x</i></body></html><b>y</b>", "html(head(title),body(i,b))");
    ok &= streamed_as("<html><head><title>t</title></head><body><p>x</p></body></html>", "html(head(title),body(p))");
    ASSERT_TRUE(ok);
    return true;
}

/* Page markup gets HTML's implied end tags (li, dd/dt, option, td/tr, p
 * before blocks) and end tags that close across the stack. */
TEST(test_dom_stream_implied_end_tags) {
    bool ok = true;
    ok &= streamed_as("<ul><li>a<li>b</ul>", "html(head,body(ul(li,li)))");
    ok &= streamed_as("<select><option>a<option>b<optgroup><option>c</select><p>x", "html(head,body(select(option,option,optgroup(option)),p))");
    ok &= streamed_as("<div><span>x</div><i>y</i>", "html(head,body(div(span),i))");
    ok &= streamed_as("<p>a<div>b</div>", "html(head,body(p,div))");
    ok &= streamed_as("<dl><dt>a<dd>b<dt>c</dl>", "html(head,body(dl(dt,dd,dt)))");
    ok &= streamed_as("<table><tr><td>1<td>2<tr><td>3</table>", "html(head,body(table(tr(td,td),tr(td))))");
    ok &= streamed_as("<p><b>x</p>y", "html(head,body(p(b)))");
    ok &= streamed_as("<div></span>a</div>", "html(head,body(div))");
    ok &= streamed_as("<ytd-permission-role-bottom-bar-renderer></ytd-permission-role-bottom-bar-renderer><i>x</i>",
                       "html(head,body(ytd-permission-role-bottom-bar-renderer,i))");
    ASSERT_TRUE(ok);
    return true;
}

/* An unquoted attribute value runs to a space or '>': '/' is part of it
 * (<script src=/a.js> was src=""). */
TEST(test_dom_parse_unquoted_attr_slash) {
    const char *html = "<script src=/resources/a.js></script><img src=a.png/><a href=x/y/ title=t>";
    NodeId frag = dom_parse_html(html, strlen(html), DOM_NULL);
    NodeId s = dom_first_child(frag), i = dom_next_sibling(s), a = dom_next_sibling(i);
    const char *ss = dom_get_attr_str(s, "src"), *is = dom_get_attr_str(i, "src");
    const char *ah = dom_get_attr_str(a, "href"), *at = dom_get_attr_str(a, "title");
    bool ok = ss && strcmp(ss, "/resources/a.js") == 0 && is && strcmp(is, "a.png/") == 0 &&
              ah && strcmp(ah, "x/y/") == 0 && at && strcmp(at, "t") == 0;
    if (!ok) printf("    src=%s src=%s href=%s title=%s\n", ss ? ss : "-", is ? is : "-", ah ? ah : "-", at ? at : "-");
    dom_free_subtree(frag);
    ASSERT_TRUE(ok);
    return true;
}

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

/* Text keeps its white space (CSS collapses it, not the parser), CR LF
 * becomes LF, the newline after <pre> is dropped, and character references
 * decode to UTF-8. */
TEST(test_dom_parse_text_white_space_and_entities) {
    const char *html =
        "<body><p>a  \tb\r\nc</p><pre>\nx  y\n</pre>"
        "<i>&nbsp;&#233;&#x1F600;&mdash;&#150;&amp;&bogus;</i></body>";
    char *d = dump_html_parse(html, strlen(html));
    ASSERT_TRUE(d != NULL);
    bool ok = strstr(d, "T:a  \\x09b\\x0ac\n") && strstr(d, "T:x  y\\x0a\n") &&
              strstr(d, "T:\\xc2\\xa0\\xc3\\xa9\\xf0\\x9f\\x98\\x80\\xe2\\x80\\x94\\xe2\\x80\\x93&&bogus;\n");
    if (!ok) printf("    tree:\n%s", d);
    free(d);
    return ok;
}

/* A doctype after white space (or a byte-order mark), and any "<!...>" that
 * is not a comment elsewhere, is dropped rather than parsed as an element
 * holding the rest of the page. */
TEST(test_dom_parse_drops_markup_declarations) {
    const char *html = "\xEF\xBB\xBF\n  <!DOCTYPE html>\n<html><head></head><body>"
                       "<p>a<!DOCTYPE x>b<![CDATA[c]]>d<!bogus>e</p></body></html>";
    char *d = dump_html_parse(html, strlen(html));
    ASSERT_TRUE(d != NULL);
    bool ok = strncmp(d, "roots 1 1 1\n", 12) == 0 && strstr(d, "E:html") && !strstr(d, "doctype") &&
              !strstr(d, "DOCTYPE") && !strstr(d, "CDATA") && !strstr(d, "bogus");
    if (!ok) printf("    tree:\n%s", d);
    free(d);
    ok = ok && same_as_legacy(html, strlen(html), "markup declarations");
    return ok;
}

/* Comments are nodes (they were dropped); so are bogus comments, "<?...>"
 * and "<!x>" (HTML 13.2.5.42-44; the legacy parser made "<?xml>" an
 * element, which is why the fuzz cases have none). */
TEST(test_dom_parse_keeps_comments) {
    const char *html = "<p>a<!--c1-->b<?pi x?>c<!---->d<!x>e</p>";
    NodeId frag = dom_parse_html(html, strlen(html), DOM_NULL);
    NodeId p = dom_first_child(frag);
    static const struct { int kind; const char *data; } want[] = {
        { DOM_K_TEXT, "a" }, { DOM_K_COMMENT, "c1" }, { DOM_K_TEXT, "b" }, { DOM_K_COMMENT, "?pi x?" },
        { DOM_K_TEXT, "c" }, { DOM_K_COMMENT, "" }, { DOM_K_TEXT, "d" }, { DOM_K_COMMENT, "x" },
        { DOM_K_TEXT, "e" },
    };
    bool ok = dom_kind(p) == DOM_K_ELEMENT;
    NodeId c = ok ? dom_first_child(p) : DOM_NULL;
    for (size_t i = 0; ok && i < sizeof want / sizeof want[0]; i++, c = dom_next_sibling(c)) {
        uint32_t n = 0;
        const char *s = dom_text(c, &n);
        ok = dom_kind(c) == want[i].kind && n == strlen(want[i].data) && (!n || memcmp(s, want[i].data, n) == 0);
        if (!ok) printf("    child %zu: kind %d '%.*s'\n", i, dom_kind(c), (int)n, s ? s : "");
    }
    ok = ok && !c;
    dom_free_subtree(frag);
    ASSERT_TRUE(ok);
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
    /* Unparsable selectors throw a SyntaxError (querySelector, matches, closest). */
    ASSERT_EQ(1, js_int(ctx, "(function(){try{__q.querySelectorAll('li[');return 0}catch(e){return e.name==='SyntaxError'?1:0}})()"));
    ASSERT_EQ(1, js_int(ctx, "(function(){try{__q.matches('[a i]');return 0}catch(e){return e.name==='SyntaxError'?1:0}})()"));
    ASSERT_EQ(1, js_int(ctx, "(function(){try{__q.closest('');return 0}catch(e){return e.name==='SyntaxError'?1:0}})()"));
    /* Comments, escapes, the i/s flags, namespace wildcards, legacy
     * case-insensitive attributes, forgiving :is() and An+B of S. */
    ASSERT_EQ(2, js_int(ctx, "__q.querySelectorAll('li/* c */.a').length"));
    ASSERT_EQ(1, js_int(ctx, "__q.querySelectorAll('[data-x=ABC/**/i]').length"));
    ASSERT_EQ(1, js_int(ctx, "__q.querySelectorAll('[data-x=\"\\\\61 bc\" s]').length"));
    ASSERT_EQ(0, js_int(ctx, "__q.querySelectorAll('[data-x=ABC s]').length"));
    ASSERT_EQ(1, js_int(ctx, "__q.querySelectorAll('[*|data-x], *|li[|data-x]').length"));
    ASSERT_EQ(1, js_int(ctx, "__q.querySelectorAll('input[type=CHECKBOX]').length"));
    ASSERT_EQ(4, js_int(ctx, "__q.querySelectorAll(':is(li, ##)').length + __q.querySelectorAll(':where()').length"));
    ASSERT_EQ(1, js_int(ctx, "(function(){__q.querySelectorAll('li:nth-child(odd of .a)');return 1})()"));
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

/* An incremental snapshot (copying clean subtrees of the previous one) is
 * the same text as a full one after each kind of change, including one deep
 * in a subtree that was built while detached and then attached.  Element
 * features kept across snapshots (css_styledoc_get) equal a fresh build. */
static bool snapshot_inc_matches_full(JSContextHandle ctx, GCValue doc) {
    uint64_t ki = 0;
    bool u = false;
    HtmlDocument *a = html_document_from_js_dom_incremental(ctx, doc, &ki, &u);
    bool features_same = true;
    if (a) {
        const CssStyleDoc *kept = css_styledoc_get(a);
        CssStyleDoc *fresh = css_styledoc_build(a);
        features_same = css_styledoc_compare(kept, fresh) < 0;
        css_styledoc_free(fresh);
        html_document_free(a);
    }
    if (!features_same) return false;
    /* The layout mirror, updated in place, renders what serialize + parse
     * of the document renders. */
    return ki != 0 && html_mirror_check(ctx, doc) == 0;
}

TEST(test_dom_render_snapshot_incremental) {
    JSContextHandle ctx = get_shared_test_context();
    if (!dom_ready(ctx)) { printf("    (skipped - no document)\n"); return true; }
    GCValue global = JS_GetGlobalObject(ctx);
    GCValue doc = JS_GetPropertyStr(ctx, global, "document");
    ASSERT_EQ(1, js_int(ctx,
        "globalThis.__si=document.createElement('div'); __si.innerHTML="
        "'<p><span>a</span></p><p>b</p><section><em>c</em></section>'; document.body.appendChild(__si); 1"));
    ASSERT_TRUE(snapshot_inc_matches_full(ctx, doc));
    uint64_t built0 = 0, built1 = 0;
    html_mirror_stats(NULL, NULL, &built0);
    ASSERT_TRUE(snapshot_inc_matches_full(ctx, doc));
    html_mirror_stats(NULL, NULL, &built1);
    ASSERT_EQ(built0, built1);   /* nothing changed: nothing was built */
    static const char *const steps[] = {
        "__si.querySelector('span').textContent='a2'",
        "globalThis.__sx=document.createElement('div'); __sx.appendChild(document.createElement('i'));"
        " __sx.firstChild.textContent='t'; __si.appendChild(__sx)",
        "__sx.firstChild.textContent='u'",
        "__sx.firstChild.appendChild(document.createElement('b'))",
        "__si.querySelector('em').style.color='blue'",
        "__si.querySelector('em').setAttribute('title','z')",
        "__si.querySelector('p').className='k1 k2'",
        "__si.querySelector('p').id='pid'; __si.querySelector('p').removeAttribute('class')",
        "__si.querySelector('section').remove()",
        "__si.insertBefore(__sx, __si.firstChild)",
    };
    CssStyleDocStats fs0, fs1;
    css_styledoc_stats(&fs0);
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        char js[512];
        snprintf(js, sizeof(js), "%s; 1", steps[i]);
        ASSERT_EQ(1, js_int(ctx, js));
        ASSERT_TRUE(snapshot_inc_matches_full(ctx, doc));
    }
    css_styledoc_stats(&fs1);
    ASSERT_TRUE(fs1.updated > fs0.updated);   /* attribute-only steps updated in place */
    ASSERT_EQ(1, js_int(ctx, "__si.remove(); __si=null; __sx=null; 1"));
    return true;
}

/* el.style is [PutForwards=cssText]: assigning it (Polymer's style="{{..}}"
 * bindings do, in strict mode) sets the inline style instead of throwing. */
TEST(test_dom_style_assignment_forwards_to_css_text) {
    JSContextHandle ctx = get_shared_test_context();
    if (!dom_ready(ctx)) { printf("    (skipped - no document)\n"); return true; }
    ASSERT_EQ(1, js_int(ctx,
        "(function(){'use strict'; var d=document.createElement('div');"
        "var s=d.style; d.style='color: red; width: 10px';"
        "var ok = d.style===s && d.style.color==='red' && d.style.width==='10px';"
        "d.style=''; ok = ok && d.style.color==='';"
        "return ok ? 1 : 0;})()"));
    return true;
}

/* Layout metrics read the box a forced layout computes (no fonts needed:
 * block boxes with explicit sizes).  Writing a style invalidates it. */
TEST(test_dom_layout_metrics_block_boxes) {
    JSContextHandle ctx = get_shared_test_context();
    if (!dom_ready(ctx)) { printf("    (skipped - no document)\n"); return true; }
    ASSERT_EQ(1, js_int(ctx,
        "(function(){var b=document.body;"
        "var p=document.createElement('div'), c=document.createElement('div'), h=document.createElement('div');"
        "p.style.cssText='position: relative; padding: 10px; border: 5px solid black; width: 400px';"
        "c.style.cssText='width: 100px; height: 50px; padding: 4px; border: 2px solid red; margin-top: 7px';"
        "h.style.display='none'; p.appendChild(c); p.appendChild(h); b.appendChild(p);"
        "var r=c.getBoundingClientRect(), pr=p.getBoundingClientRect();"
        "var ok = p.offsetWidth===430 && p.clientWidth===420 && p.clientLeft===5"
        "  && c.offsetWidth===112 && c.offsetHeight===62 && c.clientWidth===108 && c.clientHeight===58"
        "  && c.offsetParent===p && c.offsetLeft===10 && c.offsetTop===17"
        "  && r.width===112 && r.x===pr.x+15 && r.y===pr.y+22"
        "  && h.offsetWidth===0 && h.offsetParent===null && h.getBoundingClientRect().width===0"
        "  && document.documentElement.clientWidth===innerWidth;"
        "c.style.width='200px'; ok = ok && c.offsetWidth===212;"
        "p.remove(); ok = ok && c.offsetWidth===0;"
        "return ok ? 1 : 0;})()"));
    return true;
}

/* Back-to-back queries reuse the cached layout, but every kind of change
 * between them is still seen: style object writes, attributes, text of a
 * style element and constructed sheets. */
TEST(test_dom_layout_metrics_see_each_change) {
    JSContextHandle ctx = get_shared_test_context();
    if (!dom_ready(ctx)) { printf("    (skipped - no document)\n"); return true; }
    ASSERT_EQ(1, js_int(ctx,
        "(function(){var b=document.body, s=document.createElement('style');"
        "var c=document.createElement('div'); c.style.height='10px'; b.appendChild(c); document.head.appendChild(s);"
        "var r=[];"
        "c.style.width='50px'; r.push(c.offsetWidth===50);"
        "c.style.setProperty('width','60px'); r.push(c.offsetWidth===60);"
        "c.style.cssText='width: 70px; height: 10px'; r.push(c.offsetWidth===70);"
        "c.removeAttribute('style'); c.className='lmx'; s.textContent='.lmx{width:80px;height:5px}';"
        "r.push(c.offsetWidth===80);"
        "s.textContent='.lmx{width:90px;height:5px}'; r.push(c.offsetWidth===90);"
        "c.setAttribute('style','width: 95px'); r.push(c.offsetWidth===95);"
        "c.remove(); s.remove(); var m=0; for(var i=0;i<r.length;i++) if(!r[i]) m|=1<<i; return m ? 100+m : 1;})()"));
    return true;
}

static uint64_t lq_layout_count(void) {
    uint64_t n = 0;
    layout_api_stats(NULL, &n, NULL);
    return n;
}

/* A change that cannot alter the boxes a script asks about keeps the forced
 * layout (as browsers do): an attribute no selector names, anything inside a
 * display:none subtree and paint-only styles keep it for every query; moving
 * an absolutely positioned box keeps it for sizes but not positions, and
 * resizing one keeps it outside the box.  Each answer is the one a new
 * layout gives. */
TEST(test_dom_layout_kept_across_changes_it_cannot_see) {
    JSContextHandle ctx = get_shared_test_context();
    if (!dom_ready(ctx)) { printf("    (skipped - no document)\n"); return true; }
    ASSERT_EQ(1, js_int(ctx,
        "(function(){var b=document.body, p=document.createElement('div');"
        "p.style.cssText='position: relative; width: 400px';"
        "var a=document.createElement('div'); a.style.cssText='position: absolute; left: 5px; top: 3px; width: 30px; height: 20px';"
        "var t=document.createElement('div'); t.style.cssText='width: 100px; height: 10px';"
        "var h=document.createElement('div'); h.style.display='none'; h.innerHTML='<span>x</span>';"
        "p.appendChild(a); p.appendChild(t); p.appendChild(h); b.appendChild(p);"
        "globalThis.__kp={p:p, a:a, t:t, h:h}; return t.offsetWidth===100 ? 1 : 0;})()"));
    static const struct { const char *js; int layouts; } steps[] = {
        /* An attribute no selector names. */
        { "__kp.t.setAttribute('data-kx', '1'); return __kp.t.offsetWidth===100", 0 },
        /* Inside a display:none subtree. */
        { "__kp.h.firstChild.textContent='y'; __kp.h.appendChild(document.createElement('b'));"
          " return __kp.t.offsetWidth===100 && __kp.h.offsetWidth===0", 0 },
        /* Paint only. */
        { "__kp.t.style.color='red'; __kp.t.style.opacity='0.5'; return __kp.t.offsetWidth===100", 0 },
        /* An absolute box moves: sizes hold, its position is laid out. */
        { "__kp.a.style.left='50px'; return __kp.t.offsetWidth===100 && __kp.a.offsetWidth===30", 0 },
        { "return __kp.a.offsetLeft===50", 1 },
        /* It grows: outside it nothing changes; it is laid out. */
        { "__kp.a.style.width='60px'; return __kp.t.offsetWidth===100 && __kp.t.offsetTop===0", 0 },
        { "return __kp.a.offsetWidth===60", 1 },
        /* An in-flow box grows: everything is laid out. */
        { "__kp.t.style.width='120px'; return __kp.a.offsetWidth===60 && __kp.t.offsetWidth===120", 1 },
    };
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        char js[512];
        snprintf(js, sizeof(js), "(function(){ %s })() ? 1 : 0", steps[i].js);
        uint64_t before = lq_layout_count();
        if (js_int(ctx, js) != 1) { printf("    step %zu answered wrong: %s\n", i, steps[i].js); return false; }
        uint64_t ran = lq_layout_count() - before;
        if ((int)ran != steps[i].layouts) {
            printf("    step %zu ran %llu layouts, expected %d: %s\n", i, (unsigned long long)ran, steps[i].layouts,
                   steps[i].js);
            return false;
        }
    }
    ASSERT_EQ(1, js_int(ctx, "__kp.p.remove(); __kp=null; 1"));
    return true;
}

/* A query is laid out again only when a change since the cached layout can
 * reach its box (layout_api's anchors): a change after it in block flow, in
 * a flex item with a set basis, or inside a box its content cannot resize
 * keeps the layout for it; a change to a sibling sized by its content, below
 * a box sized by its content, or (through :has()) to an ancestor's or an
 * earlier sibling's style does not, nor does an element's own restyle unless
 * (restyled on demand) its style comes out as before.  Each answer is a new
 * layout's. */
TEST(test_dom_layout_kept_for_queries_a_change_cannot_reach) {
    JSContextHandle ctx = get_shared_test_context();
    if (!dom_ready(ctx)) { printf("    (skipped - no document)\n"); return true; }
    ASSERT_EQ(1, js_int(ctx,
        "(function(){var d=document, s=d.createElement('style');"
        "s.textContent='.kq-hq:has(.kq-on){width:120px} .kq-sq:has(+ .kq-on){height:33px}"
        " .kq-hid{display:none} .kq-big{font-size:30px} .kq-od.kq-od2{width:300px}';"
        "d.head.appendChild(s);"
        "var r=d.createElement('div'); r.style.width='500px';"
        "r.innerHTML='<div class=kq-hid id=kq-hp><span id=kq-hs>t</span></div>'"
        " + '<div id=kq-od style=\"height: 10px\"></div>'"
        " + '<div id=kq-q style=\"height: 20px\">q</div>'"
        " + '<div id=kq-f style=\"display: flex\"><div id=kq-fa style=\"flex: 0 0 100px; min-width: 0; height: 10px\"></div>'"
        " + '<div id=kq-fb style=\"flex: 0 0 100px; min-width: 0; height: 10px\"></div></div>'"
        " + '<div id=kq-g style=\"display: flex\"><div id=kq-ga>aa</div><div id=kq-gb>bb</div></div>'"
        " + '<div id=kq-box style=\"width: 200px; height: 50px; overflow: hidden\"><span id=kq-bs>x</span></div>'"
        " + '<div id=kq-after style=\"height: 5px\"></div>'"
        " + '<div class=kq-hq id=kq-hq><span id=kq-hc>h</span></div>'"
        " + '<div class=kq-sq id=kq-sq1></div><div id=kq-sq2></div>'"
        " + '<div id=kq-tail></div>';"
        "d.body.appendChild(r);"
        "var e=function(i){return d.getElementById(i);}, k={r:r, s:s};"
        "['q','f','fa','fb','g','ga','gb','box','bs','after','hq','hc','sq1','sq2','hp','hs','od','tail']"
        ".forEach(function(i){k[i]=e('kq-'+i);});"
        "k.qt=k.q.offsetTop; k.fal=k.fa.offsetLeft; k.aft=k.after.offsetTop; k.hqw=k.hq.offsetWidth;"
        "globalThis.__kq=k; return k.q.offsetWidth===500 && k.fa.offsetWidth===100 ? 1 : 0;})()"));
    static const struct { const char *js; int layouts; } steps[] = {
        /* After the query in block flow. */
        { "__kq.tail.appendChild(document.createElement('p'));"
          " return __kq.q.offsetWidth===500 && __kq.q.offsetTop===__kq.qt", 0 },
        { "return __kq.tail.offsetWidth===500", 1 },
        /* A flex item with a set basis and minimum: the others hold. */
        { "__kq.fb.appendChild(document.createTextNode('a much longer text'));"
          " return __kq.fa.offsetWidth===100 && __kq.fa.offsetLeft===__kq.fal", 0 },
        /* Items sized by their content: laid out again. */
        { "__kq.gb.firstChild.textContent='bbbbbbbbbbbbbbbbbbbb'; return __kq.ga.offsetWidth>0", 1 },
        /* Inside a box its content cannot resize: outside it and the box
         * itself hold; inside it is laid out. */
        { "__kq.bs.textContent='changed text'; return __kq.after.offsetTop===__kq.aft", 0 },
        { "return __kq.box.offsetHeight===50 && __kq.box.offsetWidth===200", 0 },
        { "return __kq.bs.offsetWidth>0", 1 },
        /* :has() restyles the box holding the change. */
        { "__kq.hc.className='kq-on'; return __kq.hq.offsetWidth===120", 1 },
        /* ... and, sibling-relative, an earlier sibling. */
        { "__kq.sq2.className='kq-on'; return __kq.sq1.offsetHeight===33", 1 },
        /* Left unstyled while hidden, styled when shown. */
        { "__kq.hs.className='kq-big'; return __kq.q.offsetWidth===500 && __kq.hs.offsetWidth===0", 0 },
        { "__kq.hp.className=''; return __kq.hs.offsetHeight>=30", 1 },
        /* A class a layout rule names, the rule not matching: restyled on
         * demand, the element's own style holds. */
        { "__kq.od.className='kq-od'; return __kq.od.offsetHeight===10 && __kq.od.offsetWidth===500", 0 },
        { "__kq.od.className='kq-od kq-od2'; return __kq.od.offsetWidth===300", 1 },
    };
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        char js[640];
        snprintf(js, sizeof(js), "(function(){ %s })() ? 1 : 0", steps[i].js);
        uint64_t before = lq_layout_count();
        if (js_int(ctx, js) != 1) { printf("    step %zu answered wrong: %s\n", i, steps[i].js); return false; }
        uint64_t ran = lq_layout_count() - before;
        if ((int)ran != steps[i].layouts) {
            printf("    step %zu ran %llu layouts, expected %d: %s\n", i, (unsigned long long)ran, steps[i].layouts,
                   steps[i].js);
            return false;
        }
    }
    ASSERT_EQ(1, js_int(ctx, "__kq.r.remove(); __kq.s.remove(); __kq=null; 1"));
    return true;
}

/* A computed font size inside a display:none subtree, which layouts leave
 * unstyled, is styled on demand: the query lays out once more styling it. */
TEST(test_dom_computed_font_size_inside_hidden_subtree) {
    JSContextHandle ctx = get_shared_test_context();
    if (!dom_ready(ctx)) { printf("    (skipped - no document)\n"); return true; }
    uint64_t before = layout_api_style_all_layouts();
    ASSERT_EQ(1, js_int(ctx,
        "(function(){var d=document, s=d.createElement('style');"
        "s.textContent='.fh-p{display:none;font-size:10px}';"
        "d.head.appendChild(s);"
        "var p=d.createElement('div'); p.className='fh-p'; p.innerHTML='<span style=\"font-size: 2em\">x</span>';"
        "d.body.appendChild(p);"
        "var fs=getComputedStyle(p.firstChild).fontSize;"
        "p.remove(); s.remove(); return fs==='20px' ? 1 : 0;})()"));
    /* (an element inside a display:none subtree is styled on its own: no
     * layout styling every hidden subtree) */
    ASSERT_EQ(0, (int)(layout_api_style_all_layouts() - before));
    return true;
}

/* DOM collections are iterable (for...of, spread), as YouTube's modern
 * build expects: it iterates el.attributes and el.classList in custom
 * element callbacks, and without iterators every one of them threw. */
TEST(test_dom_collections_are_iterable) {
    JSContextHandle ctx = get_shared_test_context();
    if (!dom_ready(ctx)) { printf("    (skipped - no document)\n"); return true; }
    int32_t r = js_int(ctx,
        "(function(){ var d = document.createElement('div'); d.className = 'a b';"
        " d.setAttribute('data-x', '1'); d.innerHTML = '<i></i><b></b>'; document.body.appendChild(d);"
        " var cases = [['classList', d.classList, 2], ['attributes', d.attributes, 2],"
        "  ['childNodes', d.childNodes, 2], ['children', d.children, 2],"
        "  ['querySelectorAll', d.querySelectorAll('*'), 2],"
        "  ['getElementsByTagName', d.getElementsByTagName('i'), 1],"
        "  ['getElementsByClassName', document.getElementsByClassName('a'), 1]];"
        " var bad = 0;"
        " for (var i = 0; i < cases.length; i++) {"
        "   var c = cases[i], n = 0;"
        "   try { for (var x of c[1]) n++; } catch (e) { n = -1; }"
        "   if (n !== c[2]) bad |= 1 << i;"
        " }"
        " d.remove(); return bad ? 100 + bad : 1; })()");
    if (r != 1) printf("    not iterable (bit per case): %d\n", r - 100);
    ASSERT_EQ(1, r);
    return true;
}

/* A timer gives back its storage (callback and arguments) when it is
 * cleared or fires: a page that keeps scheduling never runs out.  Storage
 * used to be handed out once, so after ~2300 values every new timer was
 * silently dropped (setTimeout returned 0) and YouTube's scheduler stalled
 * with jobs queued. */
TEST(test_dom_timers_reuse_storage) {
    JSContextHandle ctx = get_shared_test_context();
    if (!dom_ready(ctx)) { printf("    (skipped - no document)\n"); return true; }
    ASSERT_EQ(1, js_int(ctx,
        "(function(){ for (var i = 0; i < 3000; i++) {"
        "  var id = setTimeout(function(a, b){}, 100000, i, 'x');"
        "  if (!(id > 0)) return 100 + (i % 100); clearTimeout(id); }"
        " var last = setTimeout(function(){}, 100000); clearTimeout(last);"
        " return last > 0 ? 1 : 0; })()"));
    return true;
}

/* MSE: a MediaSource opens when a video element attaches it (sourceopen is a
 * task), and appendBuffer's update events come later with updating true in
 * between.  isTypeSupported answers for the decoders actually built. */
TEST(test_dom_media_source_events) {
    JSContextHandle ctx = get_shared_test_context();
    if (!dom_ready(ctx)) { printf("    (skipped - no document)\n"); return true; }
    ASSERT_EQ(1, js_int(ctx,
        "globalThis.__mse=[];"
        "var ms=new MediaSource(), v=document.createElement('video');"
        "__mse.push('ctor:'+ms.readyState);"
        "ms.addEventListener('sourceopen', function(){"
        "  __mse.push('open:'+ms.readyState);"
        "  var sb=ms.addSourceBuffer('video/mp4; codecs=\"avc1.4d401f\"');"
        "  sb.addEventListener('updatestart', function(){ __mse.push('start:'+sb.updating); });"
        "  sb.addEventListener('updateend', function(){ __mse.push('end:'+sb.updating+':'+sb.buffered.length); });"
        "  sb.appendBuffer(new Uint8Array([0,0,0,8,0x66,0x72,0x65,0x65]));"
        "  __mse.push('appended:'+sb.updating);"
        "});"
        "v.src=URL.createObjectURL(ms);"
        "__mse.push('attached:'+ms.readyState); 1"));
    extern int timer_process_due(JSContextHandle ctx);
    for (int i = 0; i < 4; i++) timer_process_due(ctx);
    GCValue r = JS_Eval(ctx, "__mse.join(',')", 15, "<test_dom>", JS_EVAL_TYPE_GLOBAL);
    const char *s = JS_ToCString(ctx, r);
    const char *want = "ctor:closed,attached:open,open:open,appended:true,start:true,end:false:0";
    if (!s || strcmp(s, want) != 0) {
        printf("    got  '%s'\n    want '%s'\n", s ? s : "?", want);
        return false;
    }
    /* Only what the decoders handle (h264 / aac in MP4, vp9 / opus in WebM). */
    if (media_available()) {
        ASSERT_EQ(1, js_int(ctx,
            "(MediaSource.isTypeSupported('video/mp4; codecs=\"avc1.64001f\"') &&"
            " MediaSource.isTypeSupported('video/webm; codecs=\"vp9\"') &&"
            " MediaSource.isTypeSupported('audio/webm; codecs=\"opus\"') &&"
            " !MediaSource.isTypeSupported('video/mp4; codecs=\"av01.0.05M.08\"') &&"
            " !MediaSource.isTypeSupported('video/webm; codecs=\"avc1.64001f\"') &&"
            " document.createElement('video').canPlayType('application/x-mpegURL')==='') ? 1 : 0"));
    }
    return true;
}

/* Custom element reactions: attributeChangedCallback only for observed
 * attributes, and document.importNode upgrades defined elements before it
 * returns (so a property set right after lands on the class accessor). */
TEST(test_dom_custom_element_reactions) {
    JSContextHandle ctx = get_shared_test_context();
    if (!dom_ready(ctx)) { printf("    (skipped - no document)\n"); return true; }
    ASSERT_EQ(1, js_int(ctx,
        "(function(){var log=[];"
        "class XReact extends HTMLElement {"
        "  static get observedAttributes(){ return ['watched']; }"
        "  attributeChangedCallback(n){ log.push(n); }"
        "  set label(v){ this.__label = 'via-accessor:' + v; } get label(){ return this.__label; }"
        "}"
        "customElements.define('x-react-test', XReact);"
        "var e=document.createElement('x-react-test');"
        "e.setAttribute('watched','1'); e.setAttribute('other','2'); e.hidden=true;"
        "var ok = log.join(',')==='watched' && e.hidden===true && e.hasAttribute('hidden');"
        "var t=document.createElement('template'); t.innerHTML='<div><x-react-test></x-react-test></div>';"
        "var frag=document.importNode(t.content, true), x=frag.querySelector('x-react-test');"
        "x.label='a';"
        "ok = ok && (x instanceof XReact) && x.label==='via-accessor:a' && !Object.prototype.hasOwnProperty.call(x,'label');"
        "return ok ? 1 : 0;})()"));
    return true;
}

/* A fragment script made into a "shadow root" (ShadyDOM: ShadowRoot.prototype
 * and a host) is still a detached fragment: its custom elements are not
 * connected, and moving them into the host connects them without a
 * disconnect first (YouTube's smartimation cleared its children, the
 * subscribe button, on that disconnect). */
TEST(test_dom_custom_elements_in_shady_root_fragment) {
    JSContextHandle ctx = get_shared_test_context();
    if (!dom_ready(ctx)) { printf("    (skipped - no document)\n"); return true; }
    ASSERT_EQ(1, js_int(ctx,
        "(function(){var log=[];"
        "class XShady extends HTMLElement {"
        "  connectedCallback(){ log.push('c' + this.childNodes.length); }"
        "  disconnectedCallback(){ log.push('d'); this.innerText=''; }"
        "}"
        "customElements.define('x-shady-test', XShady);"
        "var host=document.createElement('div'); document.body.appendChild(host);"
        "var t=document.createElement('template'); t.innerHTML='<x-shady-test><b></b><i></i></x-shady-test>';"
        "var frag=document.importNode(t.content, true);"
        "Object.setPrototypeOf(frag, ShadowRoot.prototype); frag.host=host;"
        "var x=frag.firstChild;"
        "var detached = x.isConnected === false && log.length === 0;"
        "Node.prototype.appendChild.call(host, frag);"
        "var ok = detached && log.join(',')==='c2' && x.childNodes.length===2 && x.isConnected;"
        "host.remove();"
        "return ok ? 1 : 0;})()"));
    return true;
}

/* DOM attributes are configurable accessors, as WebIDL defines them:
 * polyfills (ShadyDOM in patch mode, test harnesses) redefine them. */
TEST(test_dom_attributes_are_configurable) {
    JSContextHandle ctx = get_shared_test_context();
    if (!dom_ready(ctx)) { printf("    (skipped - no document)\n"); return true; }
    ASSERT_EQ(1, js_int(ctx,
        "(function(){"
        "var bad=[];"
        "[[Element.prototype,['tagName','localName','id','className','innerHTML','outerHTML','children','firstElementChild','classList','attributes']],"
        " [Node.prototype,['textContent','childNodes','parentNode']], [document.createTextNode('t'),['data']]].forEach(function(e){"
        "  e[1].forEach(function(n){ var o=e[0], d=null; while(o && !(d=Object.getOwnPropertyDescriptor(o,n))) o=Object.getPrototypeOf(o);"
        "    if(!d || !d.configurable) bad.push(n); }); });"
        "var d=document.createElement('div'); d.innerHTML='<b></b>';"
        "var orig=Object.getOwnPropertyDescriptor(Element.prototype,'innerHTML'), seen=0;"
        "Object.defineProperty(Element.prototype,'innerHTML',{configurable:true,enumerable:true,get:orig.get,set:function(v){seen++;orig.set.call(this,v);}});"
        "d.innerHTML='<i></i>'; Object.defineProperty(Element.prototype,'innerHTML',orig);"
        "return bad.length===0 && seen===1 && d.firstChild.localName==='i' ? 1 : 0;})()"));
    return true;
}

/* A MutationObserver reused after disconnect() keeps delivering (YouTube's
 * description expander re-observes the same observer from its callback),
 * disconnect() drops queued records, and one observer can watch two nodes. */
TEST(test_dom_mutation_observer_reobserve) {
    JSContextHandle ctx = get_shared_test_context();
    if (!dom_ready(ctx)) { printf("    (skipped - no document)\n"); return true; }
    ASSERT_EQ(1, js_int(ctx,
        "globalThis.__mo=[];"
        "var a=document.createElement('div'), b=document.createElement('div'), c=document.createElement('div');"
        "var mo=new MutationObserver(function(recs, obs){"
        "  __mo.push(recs.length + ':' + recs[0].target.id);"
        "  if (__mo.length === 1) { obs.disconnect(); obs.observe(a, {childList:true, subtree:true});"
        "                           a.appendChild(document.createElement('i')); }"
        "});"
        "a.id='a'; b.id='b'; c.id='c';"
        "mo.observe(a, {childList:true}); a.appendChild(document.createElement('p'));"
        "var dead=new MutationObserver(function(){ __mo.push('dead'); });"
        "dead.observe(c, {childList:true}); c.appendChild(document.createElement('p')); dead.disconnect();"
        "var two=new MutationObserver(function(recs){ recs.forEach(function(r){ __mo.push('two:' + r.target.id); }); });"
        "two.observe(b, {childList:true}); two.observe(c, {childList:true});"
        "b.appendChild(document.createElement('p')); c.appendChild(document.createElement('p')); 1"));
    JSContextHandle pctx = ctx;
    while (JS_ExecutePendingJob(JS_GetRuntime(ctx), &pctx) > 0) {}
    if (js_int(ctx, "__mo.join(',') === '1:a,two:b,two:c,1:a' ? 1 : 0") != 1) {
        GCValue rc = JS_Eval(ctx, "__mo.join(',')", 14, "<test_dom>", JS_EVAL_TYPE_GLOBAL);
        const char *s = JS_ToCString(ctx, rc);
        printf("    records = '%s'\n", s ? s : "?");
        return false;
    }
    return true;
}

/* Styles set through element.style survive cloneNode / importNode (the clone
 * has them in its style attribute). */
TEST(test_dom_clone_keeps_script_styles) {
    JSContextHandle ctx = get_shared_test_context();
    if (!dom_ready(ctx)) { printf("    (skipped - no document)\n"); return true; }
    ASSERT_EQ(1, js_int(ctx,
        "(function(){var d=document.createElement('div'); d.innerHTML='<span><svg viewBox=\"0 0 24 24\"></svg></span>';"
        "var s=d.querySelector('svg'); s.style.pointerEvents='none'; s.style.width='100%';"
        "var c=d.cloneNode(true).querySelector('svg'), i=document.importNode(d,true).querySelector('svg');"
        "var ok=function(e){ var a=e.getAttribute('style')||''; return a.indexOf('pointer-events: none')>=0 && a.indexOf('width: 100%')>=0; };"
        "return ok(c) && ok(i) && c.style.width==='100%' ? 1 : 0;})()"));
    return true;
}

/* A fetched-style Response and its clone both read the body. */
TEST(test_dom_response_clone_keeps_body) {
    JSContextHandle ctx = get_shared_test_context();
    if (!dom_ready(ctx)) { printf("    (skipped - no document)\n"); return true; }
    ASSERT_EQ(1, js_int(ctx,
        "globalThis.__rc='';"
        "var r=new Response('hi'), c=r.clone();"
        "Promise.all([r.text(), c.text(), new Response('{\"a\":1}').json()])"
        "  .then(function(a){ __rc=a[0]+','+a[1]+','+a[2].a+__rc; });"
        "r.text().then(null, function(e){ __rc+=' used:'+e.name; }); 1"));
    JSContextHandle pctx = ctx;
    while (JS_ExecutePendingJob(JS_GetRuntime(ctx), &pctx) > 0) {}
    if (js_int(ctx, "__rc.indexOf('hi,hi,1')>=0 && __rc.indexOf('used:TypeError')>=0") != 1) {
        GCValue rc = JS_Eval(ctx, "String(__rc)", 12, "<test_dom>", JS_EVAL_TYPE_GLOBAL);
        const char *s = JS_ToCString(ctx, rc);
        printf("    __rc = '%s'\n", s ? s : "?");
        return false;
    }
    return true;
}

/* Classes whose objects hold GC data must be registered with the runtime the
 * page uses, or their gc_mark never runs: a <video>'s DomWrap record (and its
 * style object) was freed by concurrent major cycles while the element lived. */
extern JSClassID js_video_class_id, js_xhr_class_id;
TEST(test_dom_video_wrapper_survives_gc) {
    JSContextHandle ctx = get_shared_test_context();
    if (!dom_ready(ctx)) { printf("    (skipped - no document)\n"); return true; }
    JSRuntimeHandle rt = JS_GetRuntime(ctx);
    ASSERT_TRUE(JS_IsRegisteredClass(rt, js_video_class_id));
    ASSERT_TRUE(JS_IsRegisteredClass(rt, js_xhr_class_id));
    ASSERT_EQ(1, js_int(ctx,
        "(function(){var v=document.createElement('video'); v.id='gcvid'; v.style.color='red';"
        "document.body.appendChild(v); return 1;})()"));
    full_gc(ctx);
    ASSERT_EQ(0, (int)dom_wrap_check(4));
    ASSERT_EQ(1, js_int(ctx,
        "(function(){var v=document.getElementById('gcvid');"
        "var ok = !!v && v.tagName==='VIDEO' && v.style.color==='red';"
        "if (v) v.remove(); return ok ? 1 : 0;})()"));
    return true;
}

/* innerHTML keeps text outside elements and valueless attributes; Text
 * nodes have data/length; innerText, focus/blur, reflected attributes; a
 * ResizeObserver reports a target once, then only on size changes. */
TEST(test_dom_fragment_text_and_html_element_basics) {
    JSContextHandle ctx = get_shared_test_context();
    if (!dom_ready(ctx)) { printf("    (skipped - no document)\n"); return true; }
    ASSERT_EQ(1, js_int(ctx,
        "(function(){var d=document.createElement('div'); document.body.appendChild(d);"
        "d.innerHTML='a<b>x</b>b<span hidden>h</span>';"
        "var k=d.childNodes, ok = k.length===4 && k[0].nodeType===3 && k[0].data==='a' && k[2].data==='b'"
        "  && k[3].hasAttribute('hidden') && k[3].hidden===true && d.innerHTML==='a<b>x</b>b<span hidden=\"\">h</span>';"
        "var t=k[0]; t.data='A'; ok = ok && t.length===1 && d.firstChild.textContent==='A';"
        "var tail=t.splitText(0); ok = ok && tail.data==='A' && t.data==='';"
        "d.innerHTML='<b>hi</b> there<br>next<div>block</div><span hidden>no</span>';"
        "ok = ok && d.innerText==='hi there\\nnext\\nblock';"
        "var i=document.createElement('input'); d.appendChild(i); var ev=[];"
        "i.addEventListener('focus', function(){ ev.push('f'); }); i.addEventListener('blur', function(){ ev.push('b'); });"
        "i.focus(); ok = ok && document.activeElement===i; i.blur();"
        "ok = ok && ev.join('')==='fb' && document.activeElement===document.body;"
        "d.title='t'; ok = ok && d.getAttribute('title')==='t' && d.tabIndex===-1 && i.tabIndex===0;"
        "d.remove(); return ok ? 1 : 0;})()"));
    ASSERT_EQ(1, js_int(ctx,
        "globalThis.__ro=0; var e=document.createElement('div'); e.style.width='10px'; e.style.height='10px';"
        "document.body.appendChild(e); globalThis.__roel=e;"
        "globalThis.__roo=new ResizeObserver(function(entries){ __ro += entries.length; });"
        "__roo.observe(e); 1"));
    JSContextHandle pctx = ctx;
    while (JS_ExecutePendingJob(JS_GetRuntime(ctx), &pctx) > 0) {}
    ASSERT_EQ(1, js_int(ctx, "__ro"));
    /* Observing again with the size unchanged reports nothing new. */
    ASSERT_EQ(1, js_int(ctx, "var o2=document.createElement('div'); document.body.appendChild(o2); __roo.observe(o2); 1"));
    while (JS_ExecutePendingJob(JS_GetRuntime(ctx), &pctx) > 0) {}
    ASSERT_EQ(2, js_int(ctx, "__ro"));
    ASSERT_EQ(1, js_int(ctx, "__roo.disconnect(); __roel.remove(); 1"));
    return true;
}

/* IntersectionObserver uses the layout: in-view targets intersect, targets
 * below the viewport or without a box do not; later layout changes are
 * reported by the per-frame update, and unchanged targets are not repeated. */
int intersection_observers_tick(JSContextHandle ctx);
void platform_sleep_ms(unsigned int ms);
TEST(test_dom_intersection_observer_geometry) {
    JSContextHandle ctx = get_shared_test_context();
    if (!dom_ready(ctx)) { printf("    (skipped - no document)\n"); return true; }
    ASSERT_EQ(1, js_int(ctx,
        "globalThis.__iolog=[]; var b=document.body;"
        "function mk(id, css){ var d=document.createElement('div'); d.id=id; d.style.cssText=css; b.appendChild(d); return d; }"
        "globalThis.__ioin=mk('ioin','height:50px'); globalThis.__iobelow=mk('iobelow','height:50px;margin-top:50000px');"
        "globalThis.__iohid=mk('iohid','height:50px;display:none');"
        "globalThis.__ioo=new IntersectionObserver(function(es){ es.forEach(function(e){ globalThis.__iolast=e; __iolog.push(e.target.id+':'+e.isIntersecting); }); });"
        "__ioo.observe(__ioin); __ioo.observe(__iobelow); __ioo.observe(__iohid); 1"));
    /* Reported at the next rendering opportunity, not as a microtask. */
    JSContextHandle pctx = ctx;
    while (JS_ExecutePendingJob(JS_GetRuntime(ctx), &pctx) > 0) {}
    ASSERT_EQ(1, js_int(ctx, "__iolog.length===0"));
    ASSERT_TRUE(intersection_observers_tick(ctx) >= 1);
    if (js_int(ctx, "__iolog.join(' ')==='ioin:true iobelow:false iohid:false'") != 1) {
        GCValue v = JS_Eval(ctx, "__iolog.join(' ')", 17, "<t>", JS_EVAL_TYPE_GLOBAL);
        const char *s = JS_ToCString(ctx, v);
        printf("    first report: %s\n", s ? s : "?");
        return false;
    }
    /* Entries are IntersectionObserverEntry objects: the polyfills' check
     * for a native observer ("intersectionRatio" in its prototype) passes,
     * so they keep this one instead of polling layout on a timer. */
    ASSERT_EQ(1, js_int(ctx,
        "(__iolast instanceof IntersectionObserverEntry) && ('intersectionRatio' in IntersectionObserverEntry.prototype) &&"
        "typeof __iolast.intersectionRatio==='number' && typeof __iolast.time==='number' &&"
        "__iolast.target && __iolast.boundingClientRect && __iolast.rootBounds &&"
        "Object.keys(__iolast).indexOf('isIntersecting')>=0 &&"
        "new IntersectionObserverEntry({intersectionRatio:0.5, isIntersecting:true}).intersectionRatio===0.5"));
    ASSERT_EQ(1, js_int(ctx, "__iolog=[]; __iohid.style.display='block'; __iobelow.style.marginTop='0px'; 1"));
    platform_sleep_ms(120);
    intersection_observers_tick(ctx);
    while (JS_ExecutePendingJob(JS_GetRuntime(ctx), &pctx) > 0) {}
    if (js_int(ctx, "__iolog.sort().join(' ')==='iobelow:true iohid:true'") != 1) {
        const char *src = "__iolog.join(' ')+' | reg='+(globalThis.__cyber_io_observers||[]).length"
                          "+' belowTop='+__iobelow.getBoundingClientRect().top+' hid='+__iohid.offsetHeight";
        GCValue v = JS_Eval(ctx, src, strlen(src), "<t>", JS_EVAL_TYPE_GLOBAL);
        const char *s = JS_ToCString(ctx, v);
        printf("    after change: %s\n", s ? s : "?");
        return false;
    }
    /* Nothing changed: nothing reported. */
    ASSERT_EQ(1, js_int(ctx, "__iolog=[]; 1"));
    platform_sleep_ms(120);
    intersection_observers_tick(ctx);
    ASSERT_EQ(1, js_int(ctx, "__iolog.length===0"));
    ASSERT_EQ(1, js_int(ctx, "__ioo.disconnect(); __ioin.remove(); __iobelow.remove(); __iohid.remove(); 1"));
    return true;
}

/* querySelector without a match is null (a fake element made YouTube's
 * "is there a continuation trigger?" checks always true). */
TEST(test_dom_query_selector_no_match_is_null) {
    JSContextHandle ctx = get_shared_test_context();
    if (!dom_ready(ctx)) { printf("    (skipped - no document)\n"); return true; }
    ASSERT_EQ(1, js_int(ctx,
        "document.querySelector('x-never-there') === null && document.body.querySelector('x-never-there') === null"
        " && document.querySelectorAll('x-never-there').length === 0"));
    return true;
}

/* <link> href/rel reflect their attributes (the sheet loader reads the
 * attributes; a plain JS property never loaded YouTube's player CSS). */
TEST(test_dom_link_reflects_href_and_rel) {
    JSContextHandle ctx = get_shared_test_context();
    if (!dom_ready(ctx)) { printf("    (skipped - no document)\n"); return true; }
    ASSERT_EQ(1, js_int(ctx,
        "(function(){var l=document.createElement('link'); l.rel='stylesheet'; l.href='https://example.com/a.css';"
        "var ok = l.getAttribute('rel')==='stylesheet' && l.getAttribute('href')==='https://example.com/a.css'"
        "  && !Object.prototype.hasOwnProperty.call(l, 'href') && l.href==='https://example.com/a.css';"
        "l.setAttribute('media', 'print'); ok = ok && l.media==='print';"
        "return ok ? 1 : 0;})()"));
    return true;
}

/* ============================================================================
 * Streamed page (HTML's parsing model)
 * ============================================================================ */

extern NodeId g_dom_index_root;

static bool stream_fetch(const char *url, HttpBuffer *out, char *err, size_t err_len) {
    (void)err; (void)err_len;
    const char *body = NULL;
    if (strstr(url, "defer.js")) body = "L('defer:'+document.readyState+':'+!!document.getElementById('b'))";
    else if (strstr(url, "async.js")) body = "L('async');document.write('<p id=\"bad\">x</p>')";
    else if (strstr(url, "block.js")) body = "L('block:'+!!document.getElementById('b'))";
    else if (strstr(url, "written.js")) body = "L('written-ext')";
    if (!body) return false;
    out->size = strlen(body);
    out->data = malloc(out->size + 1);
    memcpy(out->data, body, out->size + 1);
    return true;
}

static const char k_stream_page[] =
    "\n  <!DOCTYPE html>\n<html><head>\n"
    "<script>var __sl=[];function L(m){__sl.push(m)}L('ready:'+document.readyState);</script>\n"
    "<script>var __smo=new MutationObserver(function(rs){rs.forEach(function(r){for(var i=0;i<r.addedNodes.length;i++)"
    "{var n=r.addedNodes[i];if(n.id)L('mo:'+n.id)}})});__smo.observe(document,{childList:true,subtree:true});</script>\n"
    "<script src=\"https://stream.test/defer.js\" defer></script>\n"
    "<script src=\"https://stream.test/async.js\" async></script>\n"
    "<script>L('short')</script>\n"
    "</head><body>\n<div id=\"a\">a</div>\n"
    "<script>L('seesA:'+!!document.getElementById('a')+' seesB:'+!!document.getElementById('b'));"
    "document.write('<p id=\"w\">written</p><script>L(\"inner:\"+!!document.getElementById(\"w\"))<\\/script>"
    "<script src=\"https://stream.test/written.js\"><\\/script>');"
    "L('afterwrite:'+!!document.getElementById('w'));</script>\n"
    "<div id=\"b\">b</div>\n"
    "<script src=\"https://stream.test/block.js\"></script>\n"
    "<script>L('end')</script>\n"
    "<!x-bogus>\n</body></html>\n";

/* The page fed in pieces of `piece` bytes (0: at once).  Returns the log,
 * async's entry (which may come at any point) left out. */
static char *stream_page_log(JSContextHandle ctx, size_t piece) {
    GCValue doc = JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "document");
    GCValue old_de = JS_GetPropertyStr(ctx, doc, "documentElement");
    GCValue old_head = JS_GetPropertyStr(ctx, doc, "head");
    GCValue old_body = JS_GetPropertyStr(ctx, doc, "body");
    dom_fetch_reset();
    dom_fetch_set_fetcher(stream_fetch);
    HtmlPageStream *ps = html_page_stream_begin();
    if (!ps) return NULL;
    size_t len = strlen(k_stream_page);
    for (size_t at = 0; at < len; ) {
        size_t n = piece && len - at > piece ? piece : len - at;
        html_page_stream_feed(ps, k_stream_page + at, n);
        at += n;
    }
    html_page_stream_finish(ps, NULL);
    const char *q = "(function(){__smo.disconnect();var a=__sl.filter(function(x){return x!=='async'});"
                    "return a.join('|')+' #async='+(__sl.length-a.length)"
                    "+' bad='+!!document.getElementById('bad')"
                    "+' decl='+document.getElementsByTagName('!doctype').length+document.getElementsByTagName('!x-bogus').length"
                    "+' root='+document.documentElement.tagName})()";
    GCValue r = JS_Eval(ctx, q, strlen(q), "<test_dom>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) r = JS_GetException(ctx);
    const char *s = JS_ToCString(ctx, r);
    char *out = strdup(s ? s : "?");
    /* The shared document gets its own root back. */
    NodeId d = dom_node_of(doc), de = dom_node_of(JS_GetPropertyStr(ctx, doc, "documentElement"));
    if (de && dom_parent(de) == d) dom_detach(de, true);
    dom_attach(d, dom_node_of(old_de), DOM_NULL);
    JS_SetPropertyStr(ctx, doc, "documentElement", old_de);
    JS_SetPropertyStr(ctx, doc, "head", old_head);
    JS_SetPropertyStr(ctx, doc, "body", old_body);
    JS_SetPropertyStr(ctx, doc, "readyState", JS_NewString(ctx, "complete"));
    g_dom_index_root = dom_node_of(old_de);
    dom_fetch_reset();
    dom_fetch_set_fetcher(NULL);
    return out;
}

/* Scripts run where the parser reaches them, against what is parsed so far;
 * document.write inserts at the insertion point (its inline scripts run
 * inside write, its external ones after the writer); async scripts run when
 * downloaded and cannot write; deferred ones run once parsing is done, in
 * "interactive"; parser insertions are MutationObserver records; markup
 * declarations are dropped.  The same however the input is cut. */
TEST(test_dom_streamed_page_runs_scripts_in_place) {
    JSContextHandle ctx = get_shared_test_context();
    ASSERT_TRUE(SAFE_VALID(ctx));
    const char *want =
        "ready:loading|short|seesA:true seesB:false|inner:true|afterwrite:true|mo:a|mo:w"
        "|written-ext|block:true|mo:b|end|defer:interactive:true"
        " #async=1 bad=false decl=00 root=HTML";
    static const size_t pieces[] = { 0, 7, 1 };
    for (size_t i = 0; i < sizeof pieces / sizeof pieces[0]; i++) {
        char *got = stream_page_log(ctx, pieces[i]);
        ASSERT_TRUE(got != NULL);
        bool ok = strcmp(got, want) == 0;
        if (!ok) printf("    pieces of %zu:\n    got:  %s\n    want: %s\n", pieces[i], got, want);
        free(got);
        ASSERT_TRUE(ok);
    }
    return true;
}

void run_dom_tests(void) {
    printf("\n--- Node store DOM Tests ---\n");
    RUN_TEST(test_dom_stream_implied_end_tags);
    RUN_TEST(test_dom_parse_matches_legacy_fixtures);
    RUN_TEST(test_dom_parse_unquoted_attr_slash);
    RUN_TEST(test_dom_stream_implies_html_head_body);
    RUN_TEST(test_dom_parse_matches_legacy_fuzz);
    RUN_TEST(test_dom_parse_drops_markup_declarations);
    RUN_TEST(test_dom_parse_keeps_comments);
    RUN_TEST(test_dom_parse_text_white_space_and_entities);
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
    RUN_TEST(test_dom_streamed_page_runs_scripts_in_place);
    RUN_TEST(test_dom_render_snapshot_reuse);
    RUN_TEST(test_dom_render_snapshot_incremental);
    RUN_TEST(test_dom_style_assignment_forwards_to_css_text);
    RUN_TEST(test_dom_layout_metrics_block_boxes);
    RUN_TEST(test_dom_layout_metrics_see_each_change);
    RUN_TEST(test_dom_layout_kept_across_changes_it_cannot_see);
    RUN_TEST(test_dom_layout_kept_for_queries_a_change_cannot_reach);
    RUN_TEST(test_dom_computed_font_size_inside_hidden_subtree);
    RUN_TEST(test_dom_timers_reuse_storage);
    RUN_TEST(test_dom_collections_are_iterable);
    RUN_TEST(test_dom_media_source_events);
    RUN_TEST(test_dom_custom_element_reactions);
    RUN_TEST(test_dom_custom_elements_in_shady_root_fragment);
    RUN_TEST(test_dom_attributes_are_configurable);
    RUN_TEST(test_dom_mutation_observer_reobserve);
    RUN_TEST(test_dom_clone_keeps_script_styles);
    RUN_TEST(test_dom_response_clone_keeps_body);
    RUN_TEST(test_dom_video_wrapper_survives_gc);
    RUN_TEST(test_dom_fragment_text_and_html_element_basics);
    RUN_TEST(test_dom_intersection_observer_geometry);
    RUN_TEST(test_dom_query_selector_no_match_is_null);
    RUN_TEST(test_dom_link_reflects_href_and_rel);
}

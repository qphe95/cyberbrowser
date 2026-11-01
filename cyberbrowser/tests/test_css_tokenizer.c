/* The CSS Syntax 3 tokenizer (src/css_tokenizer.c): token kinds, values with
 * escapes resolved, numbers, strings and urls good and bad, comments. */
#include "test_runner.h"
#include "css_tokenizer.h"
#include <stdio.h>
#include <string.h>

void run_css_tokenizer_tests(void);

/* The tokens of css as one line: kind(value) separated by spaces, white
 * space as "_" (EOF left out). */
static const char *toks(const char *css)
{
    static char buf[2048];
    CssTokens ts;
    memset(&ts, 0, sizeof(ts));
    buf[0] = 0;
    if (!css_tokenize(css, strlen(css), &ts)) return "(oom)";
    size_t o = 0;
    for (int i = 0; i < ts.n && ts.t[i].type != CT_EOF; i++) {
        const CssToken *t = &ts.t[i];
        size_t vl;
        const char *v = css_tok_val(&ts, t, &vl);
        char one[256];
        switch (t->type) {
        case CT_IDENT: snprintf(one, sizeof(one), "ident(%.*s)", (int)vl, v); break;
        case CT_FUNCTION: snprintf(one, sizeof(one), "fn(%.*s)", (int)vl, v); break;
        case CT_AT_KEYWORD: snprintf(one, sizeof(one), "at(%.*s)", (int)vl, v); break;
        case CT_HASH: snprintf(one, sizeof(one), "hash%s(%.*s)", t->flags & CT_F_ID ? "-id" : "", (int)vl, v); break;
        case CT_STRING: snprintf(one, sizeof(one), "str(%.*s)", (int)vl, v); break;
        case CT_BAD_STRING: snprintf(one, sizeof(one), "bad-str"); break;
        case CT_URL: snprintf(one, sizeof(one), "url(%.*s)", (int)vl, v); break;
        case CT_BAD_URL: snprintf(one, sizeof(one), "bad-url"); break;
        case CT_DELIM: snprintf(one, sizeof(one), "delim(%c)", (char)t->delim); break;
        case CT_NUMBER: snprintf(one, sizeof(one), "num%s(%g)", t->flags & CT_F_INTEGER ? "-int" : "", t->num); break;
        case CT_PERCENTAGE: snprintf(one, sizeof(one), "pct(%g)", t->num); break;
        case CT_DIMENSION: snprintf(one, sizeof(one), "dim(%g %.*s)", t->num, (int)vl, v); break;
        case CT_WHITESPACE: snprintf(one, sizeof(one), "_"); break;
        case CT_CDO: snprintf(one, sizeof(one), "CDO"); break;
        case CT_CDC: snprintf(one, sizeof(one), "CDC"); break;
        case CT_COLON: snprintf(one, sizeof(one), ":"); break;
        case CT_SEMICOLON: snprintf(one, sizeof(one), ";"); break;
        case CT_COMMA: snprintf(one, sizeof(one), ","); break;
        case CT_LBRACKET: snprintf(one, sizeof(one), "["); break;
        case CT_RBRACKET: snprintf(one, sizeof(one), "]"); break;
        case CT_LPAREN: snprintf(one, sizeof(one), "("); break;
        case CT_RPAREN: snprintf(one, sizeof(one), ")"); break;
        case CT_LBRACE: snprintf(one, sizeof(one), "{"); break;
        case CT_RBRACE: snprintf(one, sizeof(one), "}"); break;
        default: snprintf(one, sizeof(one), "?"); break;
        }
        int w = snprintf(buf + o, sizeof(buf) - o, "%s%s", o ? " " : "", one);
        if (w < 0 || (size_t)w >= sizeof(buf) - o) break;
        o += (size_t)w;
    }
    css_tokens_free(&ts);
    return buf;
}

#define EXPECT_TOKS(css, want) do { \
    const char *got_ = toks(css); \
    if (strcmp(got_, want) != 0) { printf("    %s\n    got:  %s\n    want: %s\n", css, got_, want); } \
    ASSERT_TRUE(strcmp(got_, want) == 0); \
} while (0)

TEST(test_css_tok_rule) {
    EXPECT_TOKS("div { color: green; }",
                "ident(div) _ { _ ident(color) : _ ident(green) ; _ }");
    EXPECT_TOKS("a/* c */b", "ident(a) ident(b)");
    EXPECT_TOKS("@media (min-width:10px)", "at(media) _ ( ident(min-width) : dim(10 px) )");
    EXPECT_TOKS("<!-- p -->", "CDO _ ident(p) _ CDC");
    EXPECT_TOKS("--x:1", "ident(--x) : num-int(1)");
    return true;
}

TEST(test_css_tok_escapes) {
    EXPECT_TOKS("\\64\\69\\76", "ident(div)");
    EXPECT_TOKS("\\31 23", "ident(123)");
    EXPECT_TOKS(".sm\\:p-4", "delim(.) ident(sm:p-4)");
    /* (an escape takes one white space after it; a second ends the name) */
    EXPECT_TOKS("\\63 \\6F l", "ident(col)");
    EXPECT_TOKS("\\63  x", "ident(c) _ ident(x)");
    EXPECT_TOKS("\\000064iv", "ident(div)");
    EXPECT_TOKS("a\\", "ident(a) delim(\\)");
    EXPECT_TOKS("#\\31 a", "hash-id(1a)");
    return true;
}

TEST(test_css_tok_strings) {
    EXPECT_TOKS("\"\\\"\"", "str(\")");
    EXPECT_TOKS("'a\\\nb'", "str(ab)");
    EXPECT_TOKS("\"abc\ndef\"", "bad-str _ ident(def) str()");
    EXPECT_TOKS("'open", "str(open)");
    EXPECT_TOKS("\"\\41 B\"", "str(AB)");
    return true;
}

TEST(test_css_tok_numbers) {
    EXPECT_TOKS("+.5e1px", "dim(5 px)");
    EXPECT_TOKS("10% -3 1.5", "pct(10) _ num-int(-3) _ num(1.5)");
    EXPECT_TOKS("-.5em", "dim(-0.5 em)");
    EXPECT_TOKS("1e", "dim(1 e)");
    EXPECT_TOKS("#1a #a1", "hash(1a) _ hash-id(a1)");
    return true;
}

TEST(test_css_tok_urls) {
    EXPECT_TOKS("url( a.png )", "url(a.png)");
    EXPECT_TOKS("url(\"a.png\")", "fn(url) str(a.png) )");
    EXPECT_TOKS("url(a b)", "bad-url");
    EXPECT_TOKS("url(a\\)b)", "url(a)b)");
    EXPECT_TOKS("URL(x)", "url(x)");
    EXPECT_TOKS("url(a'b) c", "bad-url _ ident(c)");
    return true;
}

/* CSS_TOK_BENCH=<file>[;<file>...]: each stylesheet tokenized and parsed by
 * today's parser (css_stylesheet_parse), best of 5, to compare. */
#include "css_parser.h"
#include <stdlib.h>
#include <time.h>

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void css_tok_bench(const char *list)
{
    char paths[4096];
    snprintf(paths, sizeof(paths), "%s", list);
    for (char *p = strtok(paths, ";"); p; p = strtok(NULL, ";")) {
        FILE *f = fopen(p, "rb");
        if (!f) { printf("    (no %s)\n", p); continue; }
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *s = (char *)malloc((size_t)n + 1);
        if (!s || fread(s, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(s); continue; }
        fclose(f);
        s[n] = 0;
        double best_tok = 1e9, best_parse = 1e9;
        int ntok = 0;
        for (int r = 0; r < 5; r++) {
            CssTokens ts;
            memset(&ts, 0, sizeof(ts));
            double t0 = now_ms();
            css_tokenize(s, (size_t)n, &ts);
            double t1 = now_ms();
            ntok = ts.n;
            css_tokens_free(&ts);
            CssStylesheet *sh = css_stylesheet_parse(s, (size_t)n);
            double t2 = now_ms();
            css_stylesheet_free(sh);
            if (t1 - t0 < best_tok) best_tok = t1 - t0;
            if (t2 - t1 < best_parse) best_parse = t2 - t1;
        }
        printf("    %s: %ld bytes, %d tokens: tokenize %.2f ms, today's parse %.2f ms\n", p, n, ntok, best_tok,
               best_parse);
        free(s);
    }
}

/* The stream (css_tz_next) gives the tokens the array does. */
TEST(test_css_tok_stream_matches) {
    static const char *const cases[] = {
        "div { color: green; } /* c */ .a\\:b{x:url( y )}", "\\64\\69\\76 #\\31 a 'q\\'q' \"bad\nx",
        "@media (min-width: 1.5e1px) { p { width: -.5em } } <!-- -->", "url(a b) +.5% 10px#x", "a\\",
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        const char *s = cases[c];
        size_t n = strlen(s);
        CssTokens ts;
        memset(&ts, 0, sizeof(ts));
        ASSERT_TRUE(css_tokenize(s, n, &ts));
        CssTz tz;
        css_tz_init(&tz, s, n, 0);
        for (int i = 0; i < ts.n; i++) {
            CssToken t;
            ASSERT_TRUE(css_tz_next(&tz, &t));
            const CssToken *a = &ts.t[i];
            ASSERT_TRUE(t.type == a->type && t.start == a->start && t.end == a->end && t.num == a->num &&
                        t.delim == a->delim && t.val_len == a->val_len);
            size_t la, lb;
            const char *va = css_tok_val(&ts, a, &la), *vb = css_tok_val(&tz.arena, &t, &lb);
            ASSERT_TRUE(la == lb && memcmp(va, vb, la) == 0);
        }
        css_tz_free(&tz);
        css_tokens_free(&ts);
    }
    return true;
}

void run_css_tokenizer_tests(void) {
    printf("\n--- CSS tokenizer tests ---\n");
    RUN_TEST(test_css_tok_stream_matches);
    const char *bench = getenv("CSS_TOK_BENCH");
    if (bench && *bench) css_tok_bench(bench);
    RUN_TEST(test_css_tok_rule);
    RUN_TEST(test_css_tok_escapes);
    RUN_TEST(test_css_tok_strings);
    RUN_TEST(test_css_tok_numbers);
    RUN_TEST(test_css_tok_urls);
}

/* Rule lists (src/css_parser.c, CSS Syntax 3 sections 5.4.1 - 5.4.3): which
 * rules a stylesheet keeps, with their media conditions and declarations,
 * after each kind of error recovery. */
#include "test_runner.h"
#include "css_parser.h"
#include <stdio.h>
#include <string.h>

void run_css_rules_tests(void);

/* The rules of css as one line: [media]selector{prop:value;...} each. */
static const char *rules(const char *css)
{
    static char buf[2048];
    size_t o = 0;
    buf[0] = 0;
    CssStylesheet *sh = css_stylesheet_parse(css, strlen(css));
    for (int i = 0; sh && i < sh->rule_count; i++) {
        const CssRule *r = &sh->rules[i];
        char m[256] = "";
        if (r->media_query) {
            snprintf(m, sizeof(m), "[%s]", r->media_query);
            for (char *p = m; *p; p++)
                if (*p == CSS_MEDIA_AND) *p = '&';
        }
        int w = snprintf(buf + o, sizeof(buf) - o, "%s%s%s{", o ? " " : "", m, r->selector_text);
        if (w < 0 || (size_t)w >= sizeof(buf) - o) break;
        o += (size_t)w;
        for (int d = 0; d < r->declaration_count; d++) {
            w = snprintf(buf + o, sizeof(buf) - o, "%s:%s%s;", r->declarations[d].property, r->declarations[d].value,
                         r->declarations[d].important ? "!" : "");
            if (w < 0 || (size_t)w >= sizeof(buf) - o) break;
            o += (size_t)w;
        }
        if (o + 1 < sizeof(buf)) {
            buf[o++] = '}';
            buf[o] = 0;
        }
    }
    css_stylesheet_free(sh);
    return buf;
}

#define EXPECT_RULES(css, want) do { \
    const char *got_ = rules(css); \
    if (strcmp(got_, want) != 0) { printf("    %s\n    got:  %s\n    want: %s\n", css, got_, want); } \
    ASSERT_TRUE(strcmp(got_, want) == 0); \
} while (0)

TEST(test_css_rules_preludes) {
    EXPECT_RULES("p{a:b} q { c : d }", "p{a:b;} q{c:d;}");
    /* (a ; or a } does not end a prelude: the next rule is part of it) */
    EXPECT_RULES("junk; div{a:b} p{c:d}", "p{c:d;}");
    EXPECT_RULES("} div{a:b} p{c:d}", "p{c:d;}");
    /* (brackets nest: the { inside ( ) is not the block) */
    EXPECT_RULES("p ( { x:y } p { z:w } ) p { a:b } q{c:d}", "q{c:d;}");
    EXPECT_RULES("a[title=\"{\"]{a:b}", "a[title=\"{\"]{a:b;}");
    EXPECT_RULES("body,,div{a:b} 1bad{c:d} .x,{e:f} .ok{g:h}", ".ok{g:h;}");
    EXPECT_RULES("p/* c */q{a:b} r/**/.s{c:d}", "p q{a:b;} r.s{c:d;}");
    EXPECT_RULES("p{a:b", "p{a:b;}");
    EXPECT_RULES("p", "");
    return true;
}

TEST(test_css_rules_at_rules) {
    /* (@ then no identifier: a qualified rule's prelude) */
    EXPECT_RULES("@ import 'x'; div{a:b} *{c:d}", "*{c:d;}");
    EXPECT_RULES("@1import 'x'; div{a:b} p{c:d}", "p{c:d;}");
    EXPECT_RULES("@-1import 'x'; div{a:b} p{c:d}", "p{c:d;}");
    EXPECT_RULES("@badAtRule screen { div {a:b} } p{c:d}", "p{c:d;}");
    EXPECT_RULES("@keyframes k { 0%{a:b} 100%{a:c} } p{c:d}", "p{c:d;}");
    EXPECT_RULES("@font-face { src: url('}') } p{c:d}", "p{c:d;}");
    EXPECT_RULES("@foo ] } ) [; # { x } ] (; } ) '; }' \"; }\" ; p{c:d}", "p{c:d;}");
    EXPECT_RULES("@media screen { @media (min-width: 1px) { p{a:b} } } q{c:d}",
                 "[screen&(min-width: 1px)]p{a:b;} q{c:d;}");
    EXPECT_RULES("@supports (display: grid) { p{a:b} }", "p{a:b;}");
    return true;
}

TEST(test_css_rules_in_blocks) {
    /* (an at-rule in a block ends at its ; or block, or at the } of the
     * block it is in) */
    EXPECT_RULES("@media all { #s{a:b} @foo ] [; # { x } ] (; } ) '; }' ; #s{c:d} }",
                 "[all]#s{a:b;} [all]#s{c:d;}");
    EXPECT_RULES("@media all { #s{a:b} @foo [; ] {; #s{x:y} #s{x:y} } #s{c:d} }",
                 "[all]#s{a:b;} [all]#s{c:d;}");
    EXPECT_RULES("@media all { #e{a:b} @import 'x' } q{c:d}", "[all]#e{a:b;} q{c:d;}");
    /* (a stray } in a block closes it) */
    EXPECT_RULES("@media all { } p{a:b} } q{c:d}", "p{a:b;}");
    return true;
}

TEST(test_css_rules_top_level) {
    EXPECT_RULES("<!-- p{a:b} --> q{c:d} -->", "p{a:b;} q{c:d;}");
    EXPECT_RULES("<![CDATA[ p{a:b} ]]>", "p{a:b;}");
    /* (only at the top level: in a prelude, CDO is not a selector) */
    EXPECT_RULES("p <!-- {a:b} q{c:d}", "q{c:d;}");
    return true;
}

TEST(test_css_rules_declarations) {
    /* (junk in a declaration list ends at its ; or with its {} block) */
    EXPECT_RULES("#p{a:orange; 12 @page {a:red} a:green}", "#p{a:orange;a:green;}");
    EXPECT_RULES("#p{a:b; 12 a:red}", "#p{a:b;}");
    /* (a bad string ends at the newline, and voids its declaration up to
     * the next ;) */
    EXPECT_RULES("div{a:b; f:'Courier\n a:red; a:c}", "div{a:b;a:c;}");
    EXPECT_RULES("p{a:red url( { t ); b:c}", "p{b:c;}");
    return true;
}

TEST(test_css_rules_import_prefix) {
    bool counts = true;
    const char *a = "@badat-rule foo; @import 'x';";
    size_t e = css_sheet_item_skip(a, strlen(a), 0, &counts);
    ASSERT_TRUE(!counts && e == strlen("@badat-rule foo;"));
    const char *b = "1badselector { p: v } @import 'x';";
    e = css_sheet_item_skip(b, strlen(b), 0, &counts);
    ASSERT_TRUE(!counts && e == strlen("1badselector { p: v }"));
    const char *c = "p { a: b } @import 'x';";
    css_sheet_item_skip(c, strlen(c), 0, &counts);
    ASSERT_TRUE(counts);
    const char *d = "@media print { } @import 'x';";
    css_sheet_item_skip(d, strlen(d), 0, &counts);
    ASSERT_TRUE(counts);
    return true;
}

/* Parsing in ranges (the parallel parse) gives the rules one parse does. */
TEST(test_css_rules_ranges) {
    const char *css = "p{a:b} @media all { q{c:d} } } r{e:f} s{g:h} <!-- t{i:j}";
    size_t n = strlen(css);
    CssStylesheet *whole = css_stylesheet_parse(css, n);
    ASSERT_TRUE(whole != NULL);
    for (size_t stop = 1; stop < n; stop++) {
        size_t e1 = 0, e2 = 0;
        CssStylesheet *x = css_stylesheet_parse_range(css, n, 0, stop, &e1);
        CssStylesheet *y = css_stylesheet_parse_range(css, n, e1, n, &e2);
        int nx = x ? x->rule_count : 0, ny = y ? y->rule_count : 0;
        ASSERT_TRUE(nx + ny == whole->rule_count);
        for (int i = 0; i < whole->rule_count; i++) {
            const CssRule *r = i < nx ? &x->rules[i] : &y->rules[i - nx];
            ASSERT_TRUE(strcmp(r->selector_text, whole->rules[i].selector_text) == 0);
        }
        css_stylesheet_free(x);
        css_stylesheet_free(y);
    }
    css_stylesheet_free(whole);
    return true;
}

void run_css_rules_tests(void) {
    printf("\n--- CSS rule list tests ---\n");
    RUN_TEST(test_css_rules_preludes);
    RUN_TEST(test_css_rules_at_rules);
    RUN_TEST(test_css_rules_in_blocks);
    RUN_TEST(test_css_rules_top_level);
    RUN_TEST(test_css_rules_declarations);
    RUN_TEST(test_css_rules_import_prefix);
    RUN_TEST(test_css_rules_ranges);
}

/*
 * Unit tests for CSS parser
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "test_runner.h"
#include "css_parser.h"
#include "quickjs.h"
#include "js_quickjs.h"
#include "browser_api_impl.h"
#include "browser_api_impl_handle_helpers.h"


TEST(test_parse_empty) {
    CssStylesheet *sheet = css_stylesheet_parse("", 0);
    ASSERT_TRUE(sheet == NULL);
    sheet = css_stylesheet_parse("   \n  /* only comment */ ", strlen("   \n  /* only comment */ "));
    ASSERT_TRUE(sheet == NULL);
    return true;
}

TEST(test_parse_simple_rule) {
    const char *css = "div { color: red; }";
    CssStylesheet *sheet = css_stylesheet_parse(css, strlen(css));
    ASSERT_TRUE(sheet != NULL);
    ASSERT_EQ(1, sheet->rule_count);
    ASSERT_TRUE(strstr(sheet->rules[0].selector_text, "div") != NULL);
    ASSERT_EQ(1, sheet->rules[0].declaration_count);
    ASSERT_STR_EQ("color", sheet->rules[0].declarations[0].property);
    ASSERT_STR_EQ("red", sheet->rules[0].declarations[0].value);
    css_stylesheet_free(sheet);
    return true;
}

TEST(test_parse_multiple_rules) {
    const char *css = "body { margin: 0; padding: 0; }\n.foo { display: none; }\n#bar { width: 100px; }";
    CssStylesheet *sheet = css_stylesheet_parse(css, strlen(css));
    ASSERT_TRUE(sheet != NULL);
    ASSERT_EQ(3, sheet->rule_count);

    ASSERT_EQ(2, sheet->rules[0].declaration_count);
    ASSERT_STR_EQ("body", sheet->rules[0].selector_text);

    ASSERT_EQ(1, sheet->rules[1].declaration_count);
    ASSERT_TRUE(strstr(sheet->rules[1].selector_text, ".foo") != NULL);
    ASSERT_STR_EQ("display", sheet->rules[1].declarations[0].property);
    ASSERT_STR_EQ("none", sheet->rules[1].declarations[0].value);

    ASSERT_EQ(1, sheet->rules[2].declaration_count);
    ASSERT_TRUE(strstr(sheet->rules[2].selector_text, "#bar") != NULL);
    css_stylesheet_free(sheet);
    return true;
}

TEST(test_parse_comments_and_whitespace) {
    const char *css = "/* header */\n.container /* comment */ {\n  /* inside */\n  font-size: 14px;\n  color: blue;\n}";
    CssStylesheet *sheet = css_stylesheet_parse(css, strlen(css));
    ASSERT_TRUE(sheet != NULL);
    ASSERT_EQ(1, sheet->rule_count);
    ASSERT_EQ(2, sheet->rules[0].declaration_count);
    ASSERT_STR_EQ("font-size", sheet->rules[0].declarations[0].property);
    ASSERT_STR_EQ("14px", sheet->rules[0].declarations[0].value);
    ASSERT_STR_EQ("color", sheet->rules[0].declarations[1].property);
    ASSERT_STR_EQ("blue", sheet->rules[0].declarations[1].value);
    css_stylesheet_free(sheet);
    return true;
}

TEST(test_parse_media_rules_entered) {
    /* @media blocks are entered: their rules carry the condition. */
    const char *css = "@media screen { body { color: red; } }\ndiv { display: block; }";
    CssStylesheet *sheet = css_stylesheet_parse(css, strlen(css));
    ASSERT_TRUE(sheet != NULL);
    ASSERT_EQ(2, sheet->rule_count);
    ASSERT_TRUE(strstr(sheet->rules[0].selector_text, "body") != NULL);
    ASSERT_TRUE(sheet->rules[0].media_query != NULL);
    ASSERT_STR_EQ("screen", sheet->rules[0].media_query);
    ASSERT_TRUE(strstr(sheet->rules[1].selector_text, "div") != NULL);
    ASSERT_TRUE(sheet->rules[1].media_query == NULL);
    ASSERT_EQ(1, sheet->rules[1].declaration_count);
    css_stylesheet_free(sheet);
    return true;
}

TEST(test_media_query_level4) {
    /* A 1280x800 dark-scheme screen with a fine, hovering pointer. */
    static const struct { const char *q; bool want; } cases[] = {
        { "(width>=1012px)", true }, { "(width<=1011.98px)", false },
        { "(1012px <= width < 1280px)", false }, { "(1012px <= width <= 1280px)", true },
        { "(80rem > width)", false }, { "(width >= calc(48rem - .02px))", true },
        { "screen and (width>=63.25rem)", true }, { "print and (width>=1px)", false },
        { "not print", true }, { "not screen and (width<100px)", true }, { "only screen", true },
        { "(min-width: 1280px)", true }, { "(max-width: 1279.5px)", false }, { "(width: 1280px)", true },
        { "(prefers-color-scheme:dark)", true }, { "(prefers-color-scheme: light)", false },
        { "(prefers-reduced-motion)", false }, { "(prefers-reduced-motion:no-preference)", true },
        { "(hover:hover) and (pointer:fine)", true }, { "(pointer:coarse)", false },
        { "(forced-colors:active)", false }, { "not (forced-colors:active)", true },
        { "(width < 500px) or (orientation: landscape)", true },
        { "((width > 2000px) or (hover)) and (color)", true },
        { "(resolution>=2x), (-webkit-min-device-pixel-ratio:1)", true },
        { "(resolution>=192dpi)", false }, { "(min-aspect-ratio: 16/10)", true },
        { "(unknown-feature)", false }, { "not (unknown-feature)", false },
        { "(width > 1px) and (height > 1px) or (hover)", false },   /* mixed and/or */
        { "garbage(", false }, { "", true },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        bool got = css_media_query_matches(cases[i].q, 1280, 800);
        if (got != cases[i].want) printf("    \"%s\": got %d\n", cases[i].q, got);
        ASSERT_TRUE(got == cases[i].want);
    }
    /* Nested @media blocks: each query list must match. */
    const char *css = "@media screen, print { @media (width>=2000px) { a { color: red; } } }";
    CssStylesheet *sheet = css_stylesheet_parse(css, strlen(css));
    ASSERT_TRUE(sheet != NULL && sheet->rule_count == 1);
    ASSERT_FALSE(css_rule_media_matches(&sheet->rules[0], 1280, 800));
    ASSERT_TRUE(css_rule_media_matches(&sheet->rules[0], 2400, 800));
    css_stylesheet_free(sheet);
    return true;
}

TEST(test_parse_inline_style) {
    const char *style = "color: red; background-color: blue; font-size: 12px ";
    int count = 0;
    CssDeclaration *decls = css_parse_inline_style(style, &count);
    ASSERT_TRUE(decls != NULL);
    ASSERT_EQ(3, count);
    ASSERT_STR_EQ("color", decls[0].property);
    ASSERT_STR_EQ("red", decls[0].value);
    ASSERT_STR_EQ("background-color", decls[1].property);
    ASSERT_STR_EQ("blue", decls[1].value);
    ASSERT_STR_EQ("font-size", decls[2].property);
    ASSERT_STR_EQ("12px", decls[2].value);
    css_declarations_free(decls, count);
    return true;
}

TEST(test_parse_inline_style_empty) {
    int count = -1;
    CssDeclaration *decls = css_parse_inline_style("", &count);
    ASSERT_TRUE(decls == NULL);
    ASSERT_EQ(0, count);
    return true;
}

JSContextHandle get_shared_test_context(void);

TEST(test_css_index_tables) {
    JSContextHandle ctx = get_shared_test_context();
    if (!SAFE_VALID(ctx)) {
        printf("    (skipped - no shared context)");
        return true;
    }

    printf("    [index] start\n");
    const char *html =
        "<html><head></head><body>"
        "<div id=\"target\" class=\"box\">hello</div>"
        "<span class=\"box\">world</span>"
        "</body></html>";

    printf("    [index] parsing\n");
    HtmlDocument *doc = html_parse(html, strlen(html));
    ASSERT_TRUE(doc != NULL);

    printf("    [index] creating js doc\n");
    GCValue js_doc = html_create_js_document(ctx, doc);
    ASSERT_TRUE(!JS_IsUndefined(js_doc) && !JS_IsNull(js_doc) && !JS_IsException(js_doc));

    printf("    [index] populating\n");
    ASSERT_TRUE(html_populate_js_document(ctx, js_doc, doc));
    printf("    [index] populated\n");

    /* Test the lock-free index tables directly. */
    printf("    [index] get by id\n");
    JSAtom target_id = JS_NewAtom(ctx, "target");
    GCValue by_id = css_get_element_by_id(ctx, target_id);
    JS_FreeAtom(ctx, target_id);
    ASSERT_TRUE(JS_IsObject(by_id));
    printf("    [index] by id ok\n");

    printf("    [index] get by class\n");
    JSAtom box_class = JS_NewAtom(ctx, "box");
    GCValue by_class = css_get_elements_by_class_name(ctx, box_class);
    JS_FreeAtom(ctx, box_class);
    ASSERT_TRUE(JS_IsObject(by_class));   /* (an HTMLCollection: array-like) */
    GCValue len_val = JS_GetPropertyStr(ctx, by_class, "length");
    uint32_t len = 0;
    JS_ToUint32(ctx, &len, len_val);
    ASSERT_EQ(2, (int)len);
    printf("    [index] by class ok\n");

    printf("    [index] freeing doc\n");
    html_document_free(doc);
    printf("    [index] done\n");
    return true;
}

void run_css_parser_tests(void) {
    printf("\n--- CSS Parser Tests ---\n");
    RUN_TEST(test_parse_empty);
    RUN_TEST(test_parse_simple_rule);
    RUN_TEST(test_parse_multiple_rules);
    RUN_TEST(test_parse_comments_and_whitespace);
    RUN_TEST(test_parse_media_rules_entered);
    RUN_TEST(test_media_query_level4);
    RUN_TEST(test_parse_inline_style);
    RUN_TEST(test_parse_inline_style_empty);
    RUN_TEST(test_css_index_tables);
}


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
#include "browser_api_impl_handles.h"

#ifdef __cplusplus
extern "C" {
#endif

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

TEST(test_parse_at_rule_skipped) {
    const char *css = "@media screen { body { color: red; } }\ndiv { display: block; }";
    CssStylesheet *sheet = css_stylesheet_parse(css, strlen(css));
    ASSERT_TRUE(sheet != NULL);
    ASSERT_EQ(1, sheet->rule_count);
    ASSERT_TRUE(strstr(sheet->rules[0].selector_text, "div") != NULL);
    ASSERT_EQ(1, sheet->rules[0].declaration_count);
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

extern "C" JSContextHandle get_shared_test_context(void);

TEST(test_css_apply_parallel) {
    JSContextHandle ctx = get_shared_test_context();
    if (!ctx.valid()) {
        printf("    (skipped - no shared context)");
        return true;
    }

    const char *html =
        "<html><head>"
        "<style>div { color: red; } .box { display: block; } p { margin: 0; }</style>"
        "</head><body>"
        "<div id=\"a\" class=\"box\" style=\"font-size: 12px\">hello</div>"
        "<p>world</p>"
        "</body></html>";

    HtmlDocument *doc = html_parse(html, strlen(html));
    ASSERT_TRUE(doc != NULL);

    GCValue js_doc = html_create_js_document(ctx, doc);
    ASSERT_TRUE(!JS_IsUndefined(js_doc) && !JS_IsNull(js_doc) && !JS_IsException(js_doc));

    ASSERT_TRUE(html_populate_js_document(ctx, js_doc, doc));

    css_apply_document_styles(ctx, js_doc, doc, "https://example.com/");

    HtmlNode *div = html_document_get_element_by_tag(doc, "div");
    ASSERT_TRUE(div != NULL);
    ASSERT_TRUE(div->has_js_object);

    GCValue element = div->js_object;
    GCValue style = JS_GetPropertyStr(ctx, element, "style");

    GCValue color = JS_GetPropertyStr(ctx, style, "color");
    const char *color_str = JS_ToCString(ctx, color);
    ASSERT_TRUE(color_str != NULL);
    ASSERT_STR_EQ("red", color_str);

    GCValue display = JS_GetPropertyStr(ctx, style, "display");
    const char *display_str = JS_ToCString(ctx, display);
    ASSERT_TRUE(display_str != NULL);
    ASSERT_STR_EQ("block", display_str);

    GCValue font_size = JS_GetPropertyStr(ctx, style, "fontSize");
    const char *font_size_str = JS_ToCString(ctx, font_size);
    ASSERT_TRUE(font_size_str != NULL);
    ASSERT_STR_EQ("12px", font_size_str);

    html_document_free(doc);
    return true;
}

TEST(test_css_computed_style_and_indexes) {
    JSContextHandle ctx = get_shared_test_context();
    if (!ctx.valid()) {
        printf("    (skipped - no shared context)");
        return true;
    }

    const char *html =
        "<html><head>"
        "<style>div { color: red; } .box { display: block; } p { margin: 0; }</style>"
        "</head><body>"
        "<div id=\"a\" class=\"box\" style=\"font-size: 12px\">hello</div>"
        "<p>world</p>"
        "</body></html>";

    HtmlDocument *doc = html_parse(html, strlen(html));
    ASSERT_TRUE(doc != NULL);

    GCValue js_doc = html_create_js_document(ctx, doc);
    ASSERT_TRUE(!JS_IsUndefined(js_doc) && !JS_IsNull(js_doc) && !JS_IsException(js_doc));

    ASSERT_TRUE(html_populate_js_document(ctx, js_doc, doc));
    css_apply_document_styles(ctx, js_doc, doc, "https://example.com/");

    /* getComputedStyle returns real values from the per-element table. */
    HtmlNode *div = html_document_get_element_by_tag(doc, "div");
    ASSERT_TRUE(div != NULL && div->has_js_object);

    DOMNodeHandle dom_node = DOMNodeHandle::from_object(div->js_object);
    ASSERT_TRUE(dom_node.valid());

    GCValue global = JS_GetGlobalObject(ctx);
    GCValue get_cs = JS_GetPropertyStr(ctx, global, "getComputedStyle");
    ASSERT_TRUE(!JS_IsUndefined(get_cs) && !JS_IsNull(get_cs) && !JS_IsException(get_cs));
    GCValue args[1] = { div->js_object };
    GCValue cs = JS_Call(ctx, get_cs, global, 1, args);
    ASSERT_TRUE(!JS_IsUndefined(cs) && !JS_IsNull(cs) && !JS_IsException(cs));

    GCValue color = JS_GetPropertyStr(ctx, cs, "color");
    const char *color_str = JS_ToCString(ctx, color);
    ASSERT_TRUE(color_str != NULL);
    ASSERT_STR_EQ("red", color_str);

    GCValue display = JS_GetPropertyStr(ctx, cs, "display");
    const char *display_str = JS_ToCString(ctx, display);
    ASSERT_TRUE(display_str != NULL);
    ASSERT_STR_EQ("block", display_str);

    GCValue font_size = JS_GetPropertyStr(ctx, cs, "fontSize");
    const char *font_size_str = JS_ToCString(ctx, font_size);
    ASSERT_TRUE(font_size_str != NULL);
    ASSERT_STR_EQ("12px", font_size_str);

    html_document_free(doc);
    return true;
}

TEST(test_css_parallel_computed_styles) {
    JSContextHandle ctx = get_shared_test_context();
    if (!ctx.valid()) {
        printf("    (skipped - no shared context)");
        return true;
    }

    /* Build a document with enough elements to span multiple worker chunks. */
    char html[8192];
    snprintf(html, sizeof(html),
        "<html><head><style>"
        "div { color: red; }"
        ".box { display: block; }"
        "#item5 { color: blue; }"
        "span { margin: 0; }"
        "</style></head><body>");
    size_t body_off = strlen(html);
    for (int i = 0; i < 50 && body_off < sizeof(html) - 128; i++) {
        int n = snprintf(html + body_off, sizeof(html) - body_off,
            "<div id=\"item%d\" class=\"%s\" style=\"font-size: %dpx\">%d</div>"
            "<span class=\"box\">s%d</span>",
            i, (i % 3 == 0) ? "box" : "", 10 + i, i, i);
        body_off += n;
    }
    snprintf(html + body_off, sizeof(html) - body_off, "</body></html>");

    HtmlDocument *doc = html_parse(html, strlen(html));
    ASSERT_TRUE(doc != NULL);

    GCValue js_doc = html_create_js_document(ctx, doc);
    ASSERT_TRUE(!JS_IsUndefined(js_doc) && !JS_IsNull(js_doc) && !JS_IsException(js_doc));
    ASSERT_TRUE(html_populate_js_document(ctx, js_doc, doc));

    css_apply_document_styles(ctx, js_doc, doc, "https://example.com/");

    /* Spot-check several elements via getComputedStyle. */
    GCValue global = JS_GetGlobalObject(ctx);
    GCValue get_cs = JS_GetPropertyStr(ctx, global, "getComputedStyle");

    for (int i = 0; i < 50; i++) {
        char id[32];
        snprintf(id, sizeof(id), "item%d", i);

        HtmlNode *node = html_document_get_element_by_id(doc, id);
        if (!node || !node->has_js_object) continue;

        GCValue args[1] = { node->js_object };
        GCValue cs = JS_Call(ctx, get_cs, global, 1, args);
        ASSERT_TRUE(!JS_IsUndefined(cs) && !JS_IsNull(cs) && !JS_IsException(cs));

        GCValue color = JS_GetPropertyStr(ctx, cs, "color");
        const char *color_str = JS_ToCString(ctx, color);
        ASSERT_TRUE(color_str != NULL);
        const char *expected_color = (i == 5) ? "blue" : "red";
        ASSERT_STR_EQ(expected_color, color_str);

        if (i % 3 == 0) {
            GCValue display = JS_GetPropertyStr(ctx, cs, "display");
            const char *display_str = JS_ToCString(ctx, display);
            ASSERT_TRUE(display_str != NULL);
            ASSERT_STR_EQ("block", display_str);
        }

        char expected_font[32];
        snprintf(expected_font, sizeof(expected_font), "%dpx", 10 + i);
        GCValue font_size = JS_GetPropertyStr(ctx, cs, "fontSize");
        const char *font_size_str = JS_ToCString(ctx, font_size);
        ASSERT_TRUE(font_size_str != NULL);
        ASSERT_STR_EQ(expected_font, font_size_str);
    }

    html_document_free(doc);
    return true;
}

TEST(test_css_index_tables) {
    JSContextHandle ctx = get_shared_test_context();
    if (!ctx.valid()) {
        printf("    (skipped - no shared context)");
        return true;
    }

    const char *html =
        "<html><head></head><body>"
        "<div id=\"target\" class=\"box\">hello</div>"
        "<span class=\"box\">world</span>"
        "</body></html>";

    HtmlDocument *doc = html_parse(html, strlen(html));
    ASSERT_TRUE(doc != NULL);

    GCValue js_doc = html_create_js_document(ctx, doc);
    ASSERT_TRUE(!JS_IsUndefined(js_doc) && !JS_IsNull(js_doc) && !JS_IsException(js_doc));
    ASSERT_TRUE(html_populate_js_document(ctx, js_doc, doc));

    /* Test the lock-free index tables directly. */
    JSAtom target_id = JS_NewAtom(ctx, "target");
    GCValue by_id = css_get_element_by_id(ctx, target_id);
    JS_FreeAtom(ctx, target_id);
    ASSERT_TRUE(JS_IsObject(by_id));

    JSAtom box_class = JS_NewAtom(ctx, "box");
    GCValue by_class = css_get_elements_by_class_name(ctx, box_class);
    JS_FreeAtom(ctx, box_class);
    ASSERT_TRUE(JS_IsArray(ctx, by_class));
    GCValue len_val = JS_GetPropertyStr(ctx, by_class, "length");
    uint32_t len = 0;
    JS_ToUint32(ctx, &len, len_val);
    ASSERT_EQ(2, (int)len);

    html_document_free(doc);
    return true;
}

void run_css_parser_tests(void) {
    printf("\n--- CSS Parser Tests ---\n");
    RUN_TEST(test_parse_empty);
    RUN_TEST(test_parse_simple_rule);
    RUN_TEST(test_parse_multiple_rules);
    RUN_TEST(test_parse_comments_and_whitespace);
    RUN_TEST(test_parse_at_rule_skipped);
    RUN_TEST(test_parse_inline_style);
    RUN_TEST(test_parse_inline_style_empty);
    RUN_TEST(test_css_apply_parallel);
    RUN_TEST(test_css_computed_style_and_indexes);
    RUN_TEST(test_css_parallel_computed_styles);
    RUN_TEST(test_css_index_tables);
}

#ifdef __cplusplus
}
#endif

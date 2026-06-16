/*
 * Unit tests for CSS parser
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "test_runner.h"
#include "css_parser.h"

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

void run_css_parser_tests(void) {
    printf("\n--- CSS Parser Tests ---\n");
    RUN_TEST(test_parse_empty);
    RUN_TEST(test_parse_simple_rule);
    RUN_TEST(test_parse_multiple_rules);
    RUN_TEST(test_parse_comments_and_whitespace);
    RUN_TEST(test_parse_at_rule_skipped);
    RUN_TEST(test_parse_inline_style);
    RUN_TEST(test_parse_inline_style_empty);
}

#ifdef __cplusplus
}
#endif

/* Values layout ignores (css_value_invalid, src/css_props.c): outside the
 * grammar of a checked property, so the declaration before them stays. */
#include "test_runner.h"
#include "css_props.h"
#include <stdio.h>
#include <string.h>

void run_css_values_tests(void);

#define EXPECT_INVALID(prop, value, want) do { \
    bool got_ = css_value_invalid(prop, value); \
    if (got_ != (want)) printf("    %s: %s  got %s\n", prop, value, got_ ? "invalid" : "valid"); \
    ASSERT_TRUE(got_ == (want)); \
} while (0)

TEST(test_css_values_colors) {
    EXPECT_INVALID("color", "rgb(\n 0%\n , 50%\n , 0%\n )", false);
    EXPECT_INVALID("color", "currentColor", false);
    EXPECT_INVALID("color", "#abcd", false);
    EXPECT_INVALID("color", "rgb(0 128 0 / 50%)", false);
    EXPECT_INVALID("color", "hsl(120deg 100% 25%)", false);
    EXPECT_INVALID("color", "color-mix(in srgb, red 50%, blue)", false);
    EXPECT_INVALID("color", "var(--x)", false);
    EXPECT_INVALID("color", "inherit", false);
    EXPECT_INVALID("color", "red ! fail", true);
    EXPECT_INVALID("color", "rainbow", true);
    EXPECT_INVALID("color", "red and white", true);
    EXPECT_INVALID("color", "/red", true);
    EXPECT_INVALID("color", "expression('red')", true);
    EXPECT_INVALID("color", ":red", true);
    EXPECT_INVALID("color", "r\\e d", true);
    EXPECT_INVALID("color", "red \\9", true);
    EXPECT_INVALID("background-color", "\"red\"", true);
    return true;
}

TEST(test_css_values_lengths) {
    EXPECT_INVALID("width", "10em", false);
    EXPECT_INVALID("width", "100", false);   /* (quirks pages: kept) */
    EXPECT_INVALID("width", "calc(100% - 10px)", false);
    EXPECT_INVALID("width", "fit-content", false);
    EXPECT_INVALID("max-width", "none", false);
    EXPECT_INVALID("line-height", "normal", false);
    EXPECT_INVALID("height", "+20px", false);
    EXPECT_INVALID("margin", "0 auto", false);
    EXPECT_INVALID("margin", "-10px 5% 0", false);
    EXPECT_INVALID("width", "\"auto\"", true);
    EXPECT_INVALID("width", "none", true);
    EXPECT_INVALID("height", "1 0px", true);
    EXPECT_INVALID("height", "+ 10px", true);
    EXPECT_INVALID("height", "50.1.1%", true);
    EXPECT_INVALID("font-size", "200!invalid", true);
    EXPECT_INVALID("margin", "10 0 -6", true);
    EXPECT_INVALID("padding", "1px 2px 3px 4px 5px", true);
    return true;
}

TEST(test_css_values_others) {
    EXPECT_INVALID("background", "red url(a.png) no-repeat 0 0 / cover", false);
    EXPECT_INVALID("background", "url(\"x\") center/10px auto, red", false);
    EXPECT_INVALID("background", "none", false);
    EXPECT_INVALID("background", "\"red\"", true);
    EXPECT_INVALID("background", "red;", true);
    EXPECT_INVALID("background-image", "url(a.png) repeat", true);
    EXPECT_INVALID("font-family", "\"YouTube Sans\", Roboto, sans-serif", false);
    EXPECT_INVALID("font-family", "test\\f oo, Ahem", false);
    EXPECT_INVALID("font-family", "test!foo, Ahem", true);
    EXPECT_INVALID("font-family", "a,, b", true);
    /* (not checked: left to layout) */
    EXPECT_INVALID("display", "rainbow", false);
    EXPECT_INVALID("--x", "anything !", false);
    return true;
}

void run_css_values_tests(void) {
    printf("\n--- CSS value validity tests ---\n");
    RUN_TEST(test_css_values_colors);
    RUN_TEST(test_css_values_lengths);
    RUN_TEST(test_css_values_others);
}

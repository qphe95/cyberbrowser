/*
 * test_css_props.c - the property table: value checking, canonical
 * serialization, shorthands and @supports.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_runner.h"
#include "../src/css_props.h"

void run_css_props_tests(void);

/* prop: value parses to want (NULL: is invalid). */
static bool parses(const char *prop, const char *value, const char *want) {
    const CssPropDef *p = css_prop_find(prop, strlen(prop));
    if (!p) { printf("    no property %s\n", prop); return false; }
    char *got = css_prop_parse(p, value);
    bool ok = want ? (got && strcmp(got, want) == 0) : !got;
    if (!ok) printf("    %s: \"%s\" -> %s%s%s, want %s\n", prop, value, got ? "\"" : "", got ? got : "invalid",
                    got ? "\"" : "", want ? want : "invalid");
    free(got);
    return ok;
}

TEST(test_css_props_longhands) {
    static const struct { const char *p, *v, *want; } cases[] = {
        { "background-color", "red", "red" },
        { "background-color", "#00FF00", "rgb(0, 255, 0)" },
        { "background-color", "currentColor", "currentcolor" },
        { "background-color", "rgb(100%, 100%, 0%)", "rgb(255, 255, 0)" },
        { "background-color", "rgba(0, 0, 255, 0.5)", "rgba(0, 0, 255, 0.5)" },
        { "background-color", "hsl(120deg 100% 50%)", "rgb(0, 255, 0)" },
        { "background-color", "#0000ff80", "rgba(0, 0, 255, 0.5)" },
        { "background-color", "none", NULL },
        { "background-color", "black white", NULL },
        { "background-color", "black, white", NULL },
        { "background-attachment", "scroll, FIXED, local", "scroll, fixed, local" },
        { "background-attachment", "auto", NULL },
        { "background-attachment", "local, none", NULL },
        { "background-clip", "text border-area", "border-area text" },
        { "background-clip", "margin-box", NULL },
        { "width", "10PX", "10px" },
        { "width", "-10px", NULL },
        { "width", "calc(10px + 5%)", "calc(5% + 10px)" },
        { "width", "calc(1in + 2 * 2px)", "calc(100px)" },
        { "background-position-x", "calc(10px - 0.5em)", "calc(-0.5em + 10px)" },
        { "background-size", "1px", "1px auto" },
        { "background-size", "auto auto", "auto" },
        { "background-repeat", "repeat no-repeat", "repeat-x" },
        { "border-image-repeat", "space space", "space" },
        { "border-image-outset", "0", "0" },
        { "box-shadow", "1px 1px -1px", NULL },
        { "background-image", "linear-gradient(red)", "linear-gradient(red)" },
        { "width", "calc(10px+5%)", NULL },
        { "width", "50.50%", "50.5%" },
        { "width", "0", "0px" },
        { "margin-top", "-1.0em", "-1em" },
        { "margin-top", "auto", "auto" },
        { "z-index", "3", "3" },
        { "z-index", "3.5", NULL },
        { "opacity", "50%", "50%" },
        { "font-family", "Arial, \"Helvetica Neue\", sans-serif", "Arial, \"Helvetica Neue\", sans-serif" },
        { "font-weight", "1001", NULL },
        { "box-shadow", "inset 1px 2px red", "red 1px 2px inset" },
        { "transition-duration", "1s, 200MS", "1s, 200ms" },
        { "transition-duration", "-1s", NULL },
        { "transform", "translateX(10px) rotate(45deg)", "translatex(10px) rotate(45deg)" },
        { "background-image", "linear-gradient(to right, red, blue)", "linear-gradient(to right, red, blue)" },
        { "background-image", "radial-gradient(circle -10px at center, red, blue)", NULL },
        { "background-image", "none, url(a.png)", "none, url(\"a.png\")" },
        { "background-image", "none, auto", NULL },
        { "display", "inline-flex", "inline-flex" },
        { "display", "grid", "grid" },
        { "display", "bogus", NULL },
        { "grid-template-columns", "[a] 1fr repeat(2, 10px) [b c]", "[a] 1fr repeat(2, 10px) [b c]" },
        { "grid-row-start", "0", NULL },
        { "grid-row-start", "span 2", "span 2" },
        { "color", "inherit", "inherit" },
        { "color", "var(--x)", "var(--x)" },
        { "color", "red !important", NULL },
        { "aspect-ratio", "16/9", "16 / 9" },
    };
    bool ok = true;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
        if (!parses(cases[i].p, cases[i].v, cases[i].want)) ok = false;
    ASSERT_TRUE(ok);
    return true;
}

/* A shorthand's longhands, and back. */
static bool expands(const char *prop, const char *value, const char *const *want, int n, const char *back) {
    const CssPropDef *p = css_prop_find(prop, strlen(prop));
    char **vals = NULL;
    int got = p ? css_prop_expand(p, value, &vals) : -1;
    bool ok = got == n;
    for (int i = 0; ok && i < n; i++) ok = strcmp(vals[i], want[i]) == 0;
    if (!ok) {
        printf("    %s: \"%s\" ->", prop, value);
        for (int i = 0; i < got; i++) printf(" [%s]", vals[i]);
        printf(" (%d)\n", got);
    }
    char *s = ok ? css_prop_shorthand_serialize(p, (const char *const *)vals) : NULL;
    if (ok && (back ? !s || strcmp(s, back) : s != NULL)) {
        printf("    %s from longhands: %s, want %s\n", prop, s ? s : "(none)", back ? back : "(none)");
        ok = false;
    }
    free(s);
    for (int i = 0; i < got; i++) free(vals[i]);
    free(vals);
    return ok;
}

TEST(test_css_props_shorthands) {
    bool ok = true;
    { const char *w[] = { "1px", "2px", "1px", "2px" }; ok &= expands("margin", "1px 2px", w, 4, "1px 2px"); }
    { const char *w[] = { "1px", "2px", "3px", "4px" }; ok &= expands("padding", "1px 2px 3px 4px", w, 4, "1px 2px 3px 4px"); }
    { const char *w[] = { "auto", "auto", "auto", "auto" }; ok &= expands("margin", "AUTO", w, 4, "auto"); }
    { const char *w[] = { "hidden", "hidden" }; ok &= expands("overflow", "hidden", w, 2, "hidden"); }
    { const char *w[] = { "thin", "solid", "red" }; ok &= expands("border-top", "red solid thin", w, 3, "thin solid red"); }
    { const char *w[] = { "medium", "dashed", "currentcolor" }; ok &= expands("border-top", "dashed", w, 3, "dashed"); }
    { const char *w[] = { "inherit", "inherit" }; ok &= expands("gap", "inherit", w, 2, "inherit"); }
    { const char *w[] = { "column", "wrap" }; ok &= expands("flex-flow", "wrap column", w, 2, "column wrap"); }
    { const char *w[] = { "1", "1", "0%" }; ok &= expands("flex", "1", w, 3, "1 1 0%"); }
    { const char *w[] = { "0", "0", "auto" }; ok &= expands("flex", "none", w, 3, "0 0 auto"); }
    { const char *w[] = { "10px", "10px 20px", "10px", "10px 20px" }; ok &= expands("border-radius", "10px / 10px 20px", w, 4, "10px / 10px 20px"); }
    { const char *w[] = { "1", "a", "auto", "a" }; ok &= expands("grid-area", "1 / a", w, 4, "1 / a"); }
    { const char *w[] = { "outside", "none", "none" }; ok &= expands("list-style", "none", w, 3, "none"); }
    ASSERT_TRUE(ok);
    ASSERT_TRUE(parses("border", "5px solid black", "5px solid black"));
    ASSERT_TRUE(parses("border", "solid", "solid"));
    ASSERT_TRUE(parses("background", "red", "red"));
    ASSERT_TRUE(parses("background", "url(a.png) no-repeat left top / cover, blue", "url(\"a.png\") left top / cover no-repeat, blue"));
    ASSERT_TRUE(parses("font", "bold 12px/1.5 Arial, sans-serif", "bold 12px / 1.5 Arial, sans-serif"));
    ASSERT_TRUE(parses("font", "12px", NULL));
    ASSERT_TRUE(parses("transition", "opacity 1s ease-in", "opacity 1s ease-in"));
    ASSERT_TRUE(parses("background-position", "right 10px top", "right 10px top"));
    ASSERT_TRUE(parses("margin", "1px 2px 3px 2px", "1px 2px 3px"));
    ASSERT_TRUE(parses("margin", "1px 2px 3px 4px 5px", NULL));
    return true;
}

TEST(test_css_props_supports) {
    ASSERT_TRUE(css_supports_property("display", "grid"));
    ASSERT_FALSE(css_supports_property("display", "bogus"));
    ASSERT_FALSE(css_supports_property("bogus", "1px"));
    ASSERT_TRUE(css_supports_property("--foo", "anything"));
    ASSERT_TRUE(css_supports_condition("(display: grid) and (not (display: bogus))"));
    ASSERT_FALSE(css_supports_condition("(display: grid) and (color: 12px)"));
    ASSERT_TRUE(css_supports_condition("(color: red) or (bogus: 1)"));
    ASSERT_FALSE(css_supports_condition("(color: red) or (bogus: 1) and (x: y)"));
    ASSERT_TRUE(css_prop_find_idl("backgroundColor", 15) == css_prop_find("background-color", 16));
    ASSERT_TRUE(css_prop_find_idl("cssFloat", 8) == css_prop_find("float", 5));
    ASSERT_TRUE(css_prop_find_idl("webkitTransform", 15) == css_prop_find("transform", 9));
    ASSERT_TRUE(css_prop_find_idl("backgroundcolor", 15) == NULL);
    return true;
}

/* color: value's computed value is want (currentcolor is blue). */
static bool computes(const char *value, const char *want) {
    const CssPropDef *p = css_prop_find("color", 5);
    CssComputeEnv env = { 16, 16, 800, 600, { 0, 0, 255, 1 } };
    char *spec = css_prop_parse(p, value);
    char *got = spec ? css_prop_compute(p, spec, &env) : NULL;
    bool ok = got && strcmp(got, want) == 0;
    if (!ok) printf("    color: \"%s\" -> %s, want %s\n", value, got ? got : "invalid", want);
    free(spec);
    free(got);
    return ok;
}

TEST(test_css_props_colors) {
    static const struct { const char *v, *spec, *computed; } cases[] = {
        { "lab(20 0 10/50%)", "lab(20 0 10 / 0.5)", "lab(20 0 10 / 0.5)" },
        { "oklch(0.5 0.1 400deg)", "oklch(0.5 0.1 40)", "oklch(0.5 0.1 40)" },
        { "color(srgb 10% 0.2 none)", "color(srgb 0.1 0.2 none)", "color(srgb 0.1 0.2 none)" },
        { "hwb(120 30% 50%)", "rgb(77, 128, 77)", "rgb(77, 128, 77)" },
        { "hsl(120 none 50%)", "hsl(120 none 50%)", "hsl(120 none 50%)" },
        { "rgb(128 none none)", "color(srgb 0.501961 none none)", "color(srgb 0.501961 none none)" },
        { "rgb(from rebeccapurple r g b)", "rgb(from rebeccapurple r g b)", "color(srgb 0.4 0.2 0.6)" },
        { "hsl(from rebeccapurple h s none / alpha)", NULL, "hsl(270 50% none)" },
        { "lab(from lab(25 20 50) 35 a b / alpha)", NULL, "lab(35 20 50)" },
        { "oklab(from oklab(0.5 .05 0.1 / 0.8) calc(l + 0.01) a b / alpha)", NULL, "oklab(0.51 0.05 0.1 / 0.8)" },
        { "color-mix(in lch, 25% lch(10 20 30deg), lch(50 60 70deg))", NULL, "lch(40 50 60)" },
        { "color-mix(in srgb, red, blue)", "color-mix(in srgb, red, blue)", "color(srgb 0.5 0 0.5)" },
        { "color-mix(in hsl, red 50%, blue)", "color-mix(in hsl, red, blue)", NULL },
        { "color-mix(in srgb, currentcolor, red)", "color-mix(in srgb, currentcolor, red)", "color(srgb 0.5 0 0.5)" },
        { "color-mix(in hwb, hwb(none none none), hwb(30deg 30% 40%))", NULL, "color(srgb 0.6 0.45098 0.301961)" },
        { "light-dark(black, white)", "light-dark(black, white)", "rgb(0, 0, 0)" },
    };
    bool ok = true;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        if (cases[i].spec) ok &= parses("color", cases[i].v, cases[i].spec);
        if (cases[i].computed) ok &= computes(cases[i].v, cases[i].computed);
    }
    double rgba[4];
    ASSERT_TRUE(css_color_parse("oklch(62.8% 0.2577 29.23)", rgba));     /* red */
    ok &= rgba[0] > 254 && rgba[1] < 1 && rgba[2] < 1;
    ASSERT_TRUE(css_color_parse("color-mix(in oklab, white 50%, black)", rgba));
    ok &= rgba[0] > 98 && rgba[0] < 101 && rgba[3] == 1;
    ASSERT_FALSE(css_color_parse("rgb(1, 2 3)", rgba));
    ASSERT_FALSE(css_color_parse("hwb(1, 2%, 3%)", rgba));
    return ok;
}

void run_css_props_tests(void) {
    printf("\n--- CSS Property Table Tests ---\n");
    RUN_TEST(test_css_props_longhands);
    RUN_TEST(test_css_props_shorthands);
    RUN_TEST(test_css_props_supports);
    RUN_TEST(test_css_props_colors);
}

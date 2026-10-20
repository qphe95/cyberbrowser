/*
 * CSS Layout Engine Tests
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "test_runner.h"
#include "quickjs.h"
#include "image_cache.h"
#include "html_dom.h"
#include "css_parser.h"
#include "css_layout.h"
#include "display_list.h"
#include "text_shaper.h"
#include "html_media_extract.h"
#include "js_quickjs.h"
#include "font_registry.h"
#include "font_loader.h"
#include <stdlib.h>

void run_css_layout_tests(void);
JSContextHandle get_shared_test_context(void);
GCValue get_shared_test_global(void);

static bool near_equal(double a, double b) {
    return (a > b ? a - b : b - a) < 0.001;
}

TEST(test_layout_basic_document) {
    const char *html = "<html><body><div>Hello</div></body></html>";
    HtmlDocument *doc = html_parse(html, strlen(html));
    ASSERT_TRUE(doc != NULL);

    LayoutContext ctx = {0};
    ASSERT_TRUE(css_layout_run(&ctx, doc, NULL, 800.0, 600.0));

    ASSERT_TRUE(ctx.tree.count >= 3);
    LayoutBox *root = css_layout_box_for_node(&ctx, doc->root_idx);
    ASSERT_TRUE(root != NULL);
    ASSERT_TRUE(near_equal(root->width, 800.0));
    ASSERT_TRUE(near_equal(root->height, 600.0));

    css_layout_tree_free(&ctx);
    html_document_free(doc);
    return true;
}

TEST(test_layout_stylesheet) {
    printf("    [layout stylesheet] start\n");
    const char *html = "<html><body><div class=\"box\"></div></body></html>";
    printf("    [layout stylesheet] parse html\n");
    HtmlDocument *doc = html_parse(html, strlen(html));
    ASSERT_TRUE(doc != NULL);

    const char *css = "div { width: 200px; height: 100px; margin: 10px; padding: 5px; background-color: #ff0000; }";
    printf("    [layout stylesheet] parse css\n");
    CssStylesheet *sheet = css_stylesheet_parse(css, strlen(css));
    ASSERT_TRUE(sheet != NULL);

    printf("    [layout stylesheet] run layout\n");
    LayoutContext ctx = {0};
    ASSERT_TRUE(css_layout_run(&ctx, doc, sheet, 800.0, 600.0));
    printf("    [layout stylesheet] layout ok\n");

    printf("    [layout stylesheet] find box\n");
    HtmlNode *div = html_document_get_element_by_tag(doc, "div");
    ASSERT_TRUE(div != NULL);
    int div_idx = po_array_index_from_payload(&doc->array, div);
    LayoutBox *box = css_layout_box_for_node(&ctx, div_idx);
    ASSERT_TRUE(box != NULL);
    /* Default box model is content-box: width/height apply to the content box,
     * so the stored border-box totals include padding. */
    ASSERT_TRUE(near_equal(box->width, 210.0));
    ASSERT_TRUE(near_equal(box->height, 110.0));
    ASSERT_TRUE(near_equal(box->margin_top, 10.0));
    ASSERT_TRUE(near_equal(box->padding_left, 5.0));
    ASSERT_TRUE(near_equal(box->background_color_r, 1.0));

    printf("    [layout stylesheet] cleanup\n");
    printf("    [layout stylesheet] free layout tree\n");
    css_layout_tree_free(&ctx);
    printf("    [layout stylesheet] skip stylesheet free (layout owns it)\n");
    /* css_stylesheet_free(sheet); */
    printf("    [layout stylesheet] free doc\n");
    html_document_free(doc);
    printf("    [layout stylesheet] done\n");
    return true;
}

TEST(test_layout_auto_height) {
    const char *html = "<html><body><div style=\"width:100px\"><span></span></div></body></html>";
    HtmlDocument *doc = html_parse(html, strlen(html));
    ASSERT_TRUE(doc != NULL);

    LayoutContext ctx = {0};
    ASSERT_TRUE(css_layout_run(&ctx, doc, NULL, 800.0, 600.0));

    HtmlNode *div = html_document_get_element_by_tag(doc, "div");
    ASSERT_TRUE(div != NULL);
    int div_idx = po_array_index_from_payload(&doc->array, div);
    LayoutBox *box = css_layout_box_for_node(&ctx, div_idx);
    ASSERT_TRUE(box != NULL);
    ASSERT_TRUE(near_equal(box->width, 100.0));
    ASSERT_TRUE(box->height >= 0.0);

    css_layout_tree_free(&ctx);
    html_document_free(doc);
    return true;
}

TEST(test_layout_box_sizing_content_box) {
    const char *html = "<html><body><div class=\"box\"></div></body></html>";
    HtmlDocument *doc = html_parse(html, strlen(html));
    ASSERT_TRUE(doc != NULL);

    /* Explicit content-box with padding should expand the border-box total. */
    const char *css = ".box { box-sizing: content-box; width: 200px; height: 100px; padding: 10px; }";
    CssStylesheet *sheet = css_stylesheet_parse(css, strlen(css));
    ASSERT_TRUE(sheet != NULL);

    LayoutContext ctx = {0};
    ASSERT_TRUE(css_layout_run(&ctx, doc, sheet, 800.0, 600.0));

    HtmlNode *div = html_document_get_element_by_tag(doc, "div");
    ASSERT_TRUE(div != NULL);
    int div_idx = po_array_index_from_payload(&doc->array, div);
    LayoutBox *box = css_layout_box_for_node(&ctx, div_idx);
    ASSERT_TRUE(box != NULL);
    ASSERT_TRUE(near_equal(box->width, 200.0 + 20.0));
    ASSERT_TRUE(near_equal(box->height, 100.0 + 20.0));

    css_layout_tree_free(&ctx);
    html_document_free(doc);
    return true;
}

TEST(test_layout_box_sizing_border_box) {
    const char *html = "<html><body><div class=\"box\"></div></body></html>";
    HtmlDocument *doc = html_parse(html, strlen(html));
    ASSERT_TRUE(doc != NULL);

    /* Border-box: width/height include padding and border, so the total stays
     * at the declared value and the content box shrinks. */
    const char *css = ".box { box-sizing: border-box; width: 200px; height: 100px; padding: 10px; }";
    CssStylesheet *sheet = css_stylesheet_parse(css, strlen(css));
    ASSERT_TRUE(sheet != NULL);

    LayoutContext ctx = {0};
    ASSERT_TRUE(css_layout_run(&ctx, doc, sheet, 800.0, 600.0));

    HtmlNode *div = html_document_get_element_by_tag(doc, "div");
    ASSERT_TRUE(div != NULL);
    int div_idx = po_array_index_from_payload(&doc->array, div);
    LayoutBox *box = css_layout_box_for_node(&ctx, div_idx);
    ASSERT_TRUE(box != NULL);
    ASSERT_TRUE(near_equal(box->width, 200.0));
    ASSERT_TRUE(near_equal(box->height, 100.0));

    css_layout_tree_free(&ctx);
    html_document_free(doc);
    return true;
}

TEST(test_layout_box_sizing_min_max) {
    const char *html = "<html><body><div class=\"box\"></div></body></html>";
    HtmlDocument *doc = html_parse(html, strlen(html));
    ASSERT_TRUE(doc != NULL);

    /* min-width/max-width refer to the same box as width.  For content-box
     * they constrain the content width, so the total includes padding. */
    const char *css = ".box { box-sizing: content-box; width: 50px; min-width: 100px; max-width: 300px; padding: 5px; }";
    CssStylesheet *sheet = css_stylesheet_parse(css, strlen(css));
    ASSERT_TRUE(sheet != NULL);

    LayoutContext ctx = {0};
    ASSERT_TRUE(css_layout_run(&ctx, doc, sheet, 800.0, 600.0));

    HtmlNode *div = html_document_get_element_by_tag(doc, "div");
    ASSERT_TRUE(div != NULL);
    int div_idx = po_array_index_from_payload(&doc->array, div);
    LayoutBox *box = css_layout_box_for_node(&ctx, div_idx);
    ASSERT_TRUE(box != NULL);
    ASSERT_TRUE(near_equal(box->width, 100.0 + 10.0));

    css_layout_tree_free(&ctx);
    html_document_free(doc);
    return true;
}

TEST(test_layout_flex_box_sizing) {
    const char *html = "<html><body><div id=\"flex\"><div id=\"a\"></div><div id=\"b\"></div></div></body></html>";
    HtmlDocument *doc = html_parse(html, strlen(html));
    ASSERT_TRUE(doc != NULL);

    /* In a row flex container, the main-axis size of each item is its
     * border-box width.  Content-box items add padding; border-box items keep
     * the declared total. */
    const char *css =
        "#flex { display: flex; width: 300px; }"
        "#a { box-sizing: content-box; width: 100px; padding: 10px; }"
        "#b { box-sizing: border-box; width: 100px; padding: 10px; }";
    CssStylesheet *sheet = css_stylesheet_parse(css, strlen(css));
    ASSERT_TRUE(sheet != NULL);

    LayoutContext ctx = {0};
    ASSERT_TRUE(css_layout_run(&ctx, doc, sheet, 800.0, 600.0));

    HtmlNode *flex = html_document_get_element_by_id(doc, "flex");
    HtmlNode *a = html_document_get_element_by_id(doc, "a");
    HtmlNode *b = html_document_get_element_by_id(doc, "b");
    ASSERT_TRUE(flex != NULL);
    ASSERT_TRUE(a != NULL);
    ASSERT_TRUE(b != NULL);
    int flex_idx = po_array_index_from_payload(&doc->array, flex);
    LayoutBox *box_flex = css_layout_box_for_node(&ctx, flex_idx);
    int a_idx = po_array_index_from_payload(&doc->array, a);
    int b_idx = po_array_index_from_payload(&doc->array, b);
    LayoutBox *box_a = css_layout_box_for_node(&ctx, a_idx);
    LayoutBox *box_b = css_layout_box_for_node(&ctx, b_idx);
    ASSERT_TRUE(box_a != NULL);
    ASSERT_TRUE(box_b != NULL);
    ASSERT_TRUE(near_equal(box_a->width, 100.0 + 20.0));
    ASSERT_TRUE(near_equal(box_b->width, 100.0));

    css_layout_tree_free(&ctx);
    html_document_free(doc);
    return true;
}

TEST(test_layout_flex_basis_box_sizing) {
    const char *html = "<html><body><div id=\"flex\"><div id=\"item\"></div></div></body></html>";
    HtmlDocument *doc = html_parse(html, strlen(html));
    ASSERT_TRUE(doc != NULL);

    /* flex-basis follows the same box model as width/height for length values. */
    const char *css =
        "#flex { display: flex; width: 300px; }"
        "#item { box-sizing: content-box; flex-basis: 100px; padding: 10px; }";
    CssStylesheet *sheet = css_stylesheet_parse(css, strlen(css));
    ASSERT_TRUE(sheet != NULL);

    LayoutContext ctx = {0};
    ASSERT_TRUE(css_layout_run(&ctx, doc, sheet, 800.0, 600.0));

    HtmlNode *item = html_document_get_element_by_id(doc, "item");
    ASSERT_TRUE(item != NULL);
    int item_idx = po_array_index_from_payload(&doc->array, item);
    LayoutBox *box = css_layout_box_for_node(&ctx, item_idx);
    ASSERT_TRUE(box != NULL);
    ASSERT_TRUE(near_equal(box->width, 100.0 + 20.0));

    css_layout_tree_free(&ctx);
    html_document_free(doc);
    return true;
}

TEST(test_layout_display_list) {
    const char *html = "<html><body><div style=\"width:100px; height:50px; background-color:#00ff00\"></div></body></html>";
    HtmlDocument *doc = html_parse(html, strlen(html));
    ASSERT_TRUE(doc != NULL);

    LayoutContext ctx = {0};
    ASSERT_TRUE(css_layout_run(&ctx, doc, NULL, 800.0, 600.0));

    DisplayList dl;
    ASSERT_TRUE(css_layout_build_display_list(&ctx, &dl));
    ASSERT_TRUE(dl.count > 0);

    bool found_rect = false;
    for (int i = 0; i < dl.count; i++) {
        if (dl.cmds[i].type == DL_RECT) {
            found_rect = true;
            ASSERT_TRUE(near_equal(dl.cmds[i].w, 100.0));
            ASSERT_TRUE(near_equal(dl.cmds[i].h, 50.0));
            ASSERT_TRUE(near_equal(dl.cmds[i].g, 1.0));
        }
    }
    ASSERT_TRUE(found_rect);

    display_list_free(&dl);
    css_layout_tree_free(&ctx);
    html_document_free(doc);
    return true;
}

static bool write_minimal_bmp(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    /* 1x1 24-bit BMP */
    uint8_t file_header[14] = {
        'B', 'M',
        58, 0, 0, 0,       /* file size */
        0, 0, 0, 0,
        54, 0, 0, 0        /* pixel offset */
    };
    uint8_t dib_header[40] = {
        40, 0, 0, 0,
        1, 0, 0, 0,        /* width */
        1, 0, 0, 0,        /* height */
        1, 0,              /* planes */
        24, 0,             /* bpp */
        0, 0, 0, 0,        /* compression */
        4, 0, 0, 0,        /* image size */
        0, 0, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0
    };
    uint8_t pixel[4] = { 0, 0, 255, 0 }; /* BGR + pad */
    fwrite(file_header, 1, sizeof(file_header), f);
    fwrite(dib_header, 1, sizeof(dib_header), f);
    fwrite(pixel, 1, sizeof(pixel), f);
    fclose(f);
    return true;
}

TEST(test_layout_background_image_url) {
    const char *html = "<html><body><div style=\"width:100px; height:50px; background-image:url(/red.png)\"></div></body></html>";
    HtmlDocument *doc = html_parse(html, strlen(html));
    ASSERT_TRUE(doc != NULL);

    LayoutContext ctx = {0};
    ASSERT_TRUE(css_layout_run(&ctx, doc, NULL, 800.0, 600.0));

    HtmlNode *div = html_document_get_element_by_tag(doc, "div");
    ASSERT_TRUE(div != NULL);
    int div_idx = po_array_index_from_payload(&doc->array, div);
    LayoutBox *box = css_layout_box_for_node(&ctx, div_idx);
    ASSERT_TRUE(box != NULL);
    ASSERT_STR_EQ("https://www.youtube.com/red.png", box->background_image_url);

    css_layout_tree_free(&ctx);
    html_document_free(doc);
    return true;
}

TEST(test_layout_img_display_list) {
    const char *bmp_name = "test_image_tmp.bmp";
    ASSERT_TRUE(write_minimal_bmp(bmp_name));

    const char *html = "<html><body><img src=\"test_image_tmp.bmp\" style=\"width:32px; height:32px\"></body></html>";
    HtmlDocument *doc = html_parse(html, strlen(html));
    ASSERT_TRUE(doc != NULL);

    LayoutContext ctx = {0};
    ASSERT_TRUE(css_layout_run(&ctx, doc, NULL, 800.0, 600.0));
    /* Use an empty base URL so the relative image path is passed through to the
     * local image cache as-is. */
    ctx.base_url[0] = '\0';

    ImageCache *cache = image_cache_create();
    ASSERT_TRUE(cache != NULL);
    display_list_set_image_cache(cache);

    DisplayList dl;
    display_list_init(&dl);
    ASSERT_TRUE(css_layout_build_display_list(&ctx, &dl));

    /* <img> src loading is asynchronous; the first display list contains a
     * placeholder.  Wait for pending loads and rebuild to get the real image. */
    image_cache_wait_pending(cache);

    display_list_free(&dl);
    display_list_init(&dl);
    ASSERT_TRUE(css_layout_build_display_list(&ctx, &dl));

    bool found_image = false;
    for (int i = 0; i < dl.count; i++) {
        if (dl.cmds[i].type == DL_IMAGE) {
            found_image = true;
            ASSERT_TRUE(dl.cmds[i].u.image.image_handle >= 0);
        }
    }
    ASSERT_TRUE(found_image);

    display_list_free(&dl);
    display_list_set_image_cache(NULL);
    image_cache_destroy(cache);
    css_layout_tree_free(&ctx);
    html_document_free(doc);
    remove(bmp_name);
    return true;
}

TEST(test_text_shaper_basic) {
    TextShaper *shaper = display_list_get_default_font();   /* system sans-serif */
    ASSERT_TRUE(shaper != NULL);

    float w = 0, h = 0;
    ASSERT_TRUE(text_shaper_measure(shaper, "Hello", &w, &h));
    ASSERT_TRUE(w > 0.0f && h > 0.0f);

    /* Kerning should not break measurement and should differ from unkerned for some pairs. */
    float w_kern = 0;
    ASSERT_TRUE(text_shaper_measure(shaper, "AV", &w_kern, NULL));
    ASSERT_TRUE(w_kern > 0.0f);

    DisplayList dl;
    display_list_init(&dl);
    ASSERT_TRUE(text_shaper_shape_to_display_list(shaper, "Hi \u00E9", 10.0f, 20.0f,
                                                   0.0f, 0.0f, 0.0f, 1.0f, &dl));
    ASSERT_TRUE(dl.count > 0);
    display_list_free(&dl);
    return true;
}

TEST(test_dom_mutation_sync_to_native) {
    printf("    [mutation] start\n");
    JSContextHandle ctx = get_shared_test_context();
    GCValue g_global = get_shared_test_global();

    /* Reset the shared document body to a known state. */
    const char *reset_js =
        "document.body.innerHTML = '';"
        "var __test_div = document.createElement('div');"
        "__test_div.id = 'mutation-test-div';"
        "__test_div.setAttribute('style', 'width:100px; height:50px;');"
        "document.body.appendChild(__test_div);";
    GCValue result = JS_Eval(ctx, reset_js, strlen(reset_js),
                             "<test_mutation>", JS_EVAL_TYPE_GLOBAL);
    (void)result;

    printf("    [mutation] eval done\n");
    GCValue document = JS_GetPropertyStr(ctx, g_global, "document");
    printf("    [mutation] got document\n");
    HtmlDocument *doc = html_document_from_js_dom(ctx, document);
    printf("    [mutation] html doc built\n");
    ASSERT_TRUE(doc != NULL);

    LayoutContext ctx_layout = {0};
    ASSERT_TRUE(css_layout_run(&ctx_layout, doc, NULL, 800.0, 600.0));
    printf("    [mutation] layout done\n");

    HtmlNode *div = html_document_get_element_by_id(doc, "mutation-test-div");
    ASSERT_TRUE(div != NULL);
    int div_idx = po_array_index_from_payload(&doc->array, div);
    LayoutBox *box = css_layout_box_for_node(&ctx_layout, div_idx);
    ASSERT_TRUE(box != NULL);
    ASSERT_TRUE(near_equal(box->width, 100.0));
    ASSERT_TRUE(near_equal(box->height, 50.0));

    css_layout_tree_free(&ctx_layout);
    printf("    [mutation] layout freed\n");
    html_document_free(doc);
    printf("    [mutation] doc freed\n");
    return true;
}

static int g_async_cb_count = 0;
/* Percentages in margins/paddings (also top/bottom) resolve against the
 * containing block width at layout time, also inside calc(); aspect-ratio
 * sizes the box box-sizing names. */
TEST(test_layout_percentages_and_aspect_ratio) {
    const char *html =
        "<html><body style=\"margin:0\"><div style=\"width:400px\">"
        "<div id=\"pt\" style=\"padding-top:56.25%\"></div>"
        "<div id=\"calc\" style=\"padding-top:calc(9 / 16 * 100%)\"></div>"
        "<div id=\"mix\" style=\"padding:calc(10px + 10%) 0 0\"></div>"
        "<div id=\"sh\" style=\"padding:5% 10px\"></div>"
        "<div id=\"ml\" style=\"margin-left:25%;height:10px\"></div>"
        "<div id=\"ar\" style=\"aspect-ratio:16 / 9\"></div>"
        "<div id=\"arbb\" style=\"aspect-ratio:2;box-sizing:border-box;padding:10px\"></div>"
        "<div id=\"arcb\" style=\"aspect-ratio:2;padding:10px\"></div>"
        "</div></body></html>";
    HtmlDocument *doc = html_parse(html, strlen(html));
    ASSERT_TRUE(doc != NULL);
    LayoutContext ctx = {0};
    ASSERT_TRUE(css_layout_run(&ctx, doc, NULL, 800.0, 600.0));
    static const struct { const char *id; double w, h, pt; } want[] = {
        { "pt",   400.0, 225.0, 225.0 },
        { "calc", 400.0, 225.0, 225.0 },
        { "mix",  400.0,  50.0,  50.0 },
        { "sh",   400.0,  40.0,  20.0 },
        { "ml",   300.0,  10.0,   0.0 },
        { "ar",   400.0, 225.0,   0.0 },
        { "arbb", 400.0, 200.0,  10.0 },
        { "arcb", 400.0, 210.0,  10.0 },
    };
    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        HtmlNode *n = html_document_get_element_by_id(doc, want[i].id);
        ASSERT_TRUE(n != NULL);
        LayoutBox *b = css_layout_box_for_node(&ctx, po_array_index_from_payload(&doc->array, n));
        ASSERT_TRUE(b != NULL);
        if (!near_equal(b->width, want[i].w) || !near_equal(b->height, want[i].h) ||
            !near_equal(b->padding_top, want[i].pt)) {
            printf("    #%s: %.2fx%.2f padding-top %.2f, want %.0fx%.0f padding-top %.0f\n",
                   want[i].id, b->width, b->height, b->padding_top, want[i].w, want[i].h, want[i].pt);
            css_layout_tree_free(&ctx);
            html_document_free(doc);
            return false;
        }
    }
    HtmlNode *ml = html_document_get_element_by_id(doc, "ml");
    LayoutBox *mlb = css_layout_box_for_node(&ctx, po_array_index_from_payload(&doc->array, ml));
    ASSERT_TRUE(near_equal(mlb->margin_left, 100.0));
    css_layout_tree_free(&ctx);
    html_document_free(doc);
    return true;
}

/* width/height/min/max/flex-basis percentages, plain and inside calc(),
 * resolve against the containing block, not the viewport. */
TEST(test_layout_percentage_sizes_in_calc) {
    static const struct { const char *css; double w, h; } cases[] = {
        { "width:calc(100% - 20px);height:10px", 380.0, 10.0 },
        { "width:1000px;max-width:50%;height:10px", 200.0, 10.0 },
        { "width:1000px;max-width:calc(50% + 10px);height:10px", 210.0, 10.0 },
        { "width:10px;min-width:calc(25% + 0px);height:10px", 100.0, 10.0 },
        { "height:calc(50% - 10px);width:10px", 10.0, 140.0 },
        { "flex:0 0 calc(50% - 20px);height:10px", 180.0, 10.0 },
        { "flex:1 0%;height:10px", 400.0, 10.0 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char html[512];
        snprintf(html, sizeof(html),
                 "<html><body style=\"margin:0\"><div style=\"width:400px;height:300px;display:flex\">"
                 "<div id=\"t\" style=\"%s\"></div></div></body></html>", cases[i].css);
        HtmlDocument *doc = html_parse(html, strlen(html));
        ASSERT_TRUE(doc != NULL);
        LayoutContext ctx = {0};
        ASSERT_TRUE(css_layout_run(&ctx, doc, NULL, 1024.0, 768.0));
        HtmlNode *n = html_document_get_element_by_id(doc, "t");
        LayoutBox *b = n ? css_layout_box_for_node(&ctx, po_array_index_from_payload(&doc->array, n)) : NULL;
        bool ok = b && near_equal(b->width, cases[i].w) && near_equal(b->height, cases[i].h);
        if (!ok) printf("    %s: %.2fx%.2f, want %.0fx%.0f\n", cases[i].css,
                        b ? b->width : -1.0, b ? b->height : -1.0, cases[i].w, cases[i].h);
        css_layout_tree_free(&ctx);
        html_document_free(doc);
        if (!ok) return false;
    }
    return true;
}

/* CSS-wide keywords: `height: inherit` from an auto-height parent is auto
 * (YouTube's horizontal lockup row), not an explicit 0px height; initial
 * and unset reset to the initial value. */
TEST(test_layout_css_wide_keywords) {
    const char *html =
        "<html><body style=\"margin:0\"><div style=\"width:400px\">"
        "<div id=\"lockup\" style=\"display:block\">"
        "<div id=\"row\" style=\"display:flex;flex-direction:row;height:inherit\">"
        "<div style=\"width:200px;height:113px\"></div><div style=\"flex:1;height:78px\"></div>"
        "</div></div>"
        "<div id=\"init\" style=\"height:20px;height:initial\"><div style=\"height:30px\"></div></div>"
        "<div id=\"unset\" style=\"max-width:50px;max-width:unset\"></div>"
        "<div id=\"dir\" style=\"display:flex;flex-direction:column\"><div id=\"dirc\" style=\"display:flex;flex-direction:inherit\">"
        "<div style=\"height:10px\"></div><div style=\"height:10px\"></div></div></div>"
        "</div></body></html>";
    HtmlDocument *doc = html_parse(html, strlen(html));
    ASSERT_TRUE(doc != NULL);
    LayoutContext ctx = {0};
    ASSERT_TRUE(css_layout_run(&ctx, doc, NULL, 800.0, 600.0));
    static const struct { const char *id; double w, h; } want[] = {
        { "row", 400.0, 113.0 }, { "lockup", 400.0, 113.0 },
        { "init", 400.0, 30.0 }, { "unset", 400.0, 0.0 },
        { "dirc", 400.0, 20.0 },   /* column direction inherited: children stack */
    };
    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        HtmlNode *n = html_document_get_element_by_id(doc, want[i].id);
        LayoutBox *b = n ? css_layout_box_for_node(&ctx, po_array_index_from_payload(&doc->array, n)) : NULL;
        if (!b || !near_equal(b->width, want[i].w) || !near_equal(b->height, want[i].h)) {
            printf("    #%s: %.2fx%.2f, want %.0fx%.0f\n", want[i].id, b ? b->width : -1.0,
                   b ? b->height : -1.0, want[i].w, want[i].h);
            css_layout_tree_free(&ctx);
            html_document_free(doc);
            return false;
        }
    }
    css_layout_tree_free(&ctx);
    html_document_free(doc);
    return true;
}

static void async_image_cb(const char *url, void *user_data) {
    (void)url;
    (void)user_data;
    g_async_cb_count++;
}

TEST(test_async_image_load_callback) {
    const char *bmp_name = "test_async_image_tmp.bmp";
    ASSERT_TRUE(write_minimal_bmp(bmp_name));

    g_async_cb_count = 0;
    ImageCache *cache = image_cache_create();
    ASSERT_TRUE(cache != NULL);

    int handle = image_cache_load_async(cache, bmp_name, async_image_cb, NULL);
    ASSERT_TRUE(handle >= 0);
    ASSERT_TRUE(g_async_cb_count == 0);

    image_cache_wait_pending(cache);
    ASSERT_TRUE(g_async_cb_count == 1);

    int w = 0, h = 0, ch = 0;
    uint8_t *pix = NULL;
    ASSERT_TRUE(image_cache_get(cache, handle, &w, &h, &ch, &pix));
    ASSERT_TRUE(w > 0 && h > 0);

    image_cache_destroy(cache);
    remove(bmp_name);
    return true;
}




/* white-space:pre keeps trailing spaces inside inline-flex items, so
 * "views  " pushes the following box right (YouTube's watch info row). */
TEST(test_layout_pre_trailing_spaces) {
    ASSERT_TRUE(display_list_get_default_font() != NULL);   /* a system sans-serif */
    const char *html =
        "<html><body style=\"margin:0;font-size:20px\"><div>"
        "<span id=\"a\" style=\"display:inline-flex;white-space:pre\"><span>views&#32;&#32;</span></span>"
        "<span id=\"b\" style=\"display:inline-flex;white-space:pre\"><span>views</span></span>"
        "<span id=\"c\" style=\"white-space:pre\">x&#32;&#32;</span><span id=\"d\">y</span>"
        "</div></body></html>";
    HtmlDocument *doc = html_parse(html, strlen(html));
    ASSERT_TRUE(doc != NULL);
    LayoutContext ctx = {0};
    ASSERT_TRUE(css_layout_run(&ctx, doc, NULL, 800.0, 600.0));
    LayoutBox *bx[4];
    const char *ids[4] = { "a", "b", "c", "d" };
    for (int i = 0; i < 4; i++) {
        HtmlNode *n = html_document_get_element_by_id(doc, ids[i]);
        ASSERT_TRUE(n != NULL);
        bx[i] = css_layout_box_for_node(&ctx, po_array_index_from_payload(&doc->array, n));
        ASSERT_TRUE(bx[i] != NULL);
    }
    bool ok = bx[0]->width > bx[1]->width + 8.0 && bx[1]->x >= bx[0]->x + bx[0]->width - 0.5 &&
              bx[3]->x > bx[2]->x + bx[2]->width - 0.5 && bx[2]->width > 17.0;
    css_layout_tree_free(&ctx);
    html_document_free(doc);
    return ok;
}


/* A floated display:table figure shrinks to its image; the caption wraps
 * at that width instead of widening it (Wikipedia thumbnails). */
TEST(test_layout_float_table_figure) {
    const char *html =
        "<html><body style=\"margin:0\"><div style=\"width:800px\">"
        "<figure id=\"f\" style=\"display:table;float:right;margin:0;border:1px solid\">"
        "<a id=\"a\" style=\"display:block\"><img id=\"i\" src=\"missing.png\" width=\"250\" height=\"163\" style=\"max-width:calc(100% - 8px);height:auto;margin:3px;border:1px solid\"></a>"
        "<figcaption id=\"c\" style=\"display:table-caption;caption-side:bottom\">A web browser (Safari) on a MacBook "
        "displaying the Wikipedia web page, with a caption long enough to wrap several times</figcaption>"
        "</figure><p>text</p></div></body></html>";
    HtmlDocument *doc = html_parse(html, strlen(html));
    ASSERT_TRUE(doc != NULL);
    LayoutContext ctx = {0};
    ASSERT_TRUE(css_layout_run(&ctx, doc, NULL, 800.0, 600.0));
    const char *ids[3] = { "f", "i", "c" };
    LayoutBox *bx[3];
    for (int k = 0; k < 3; k++) {
        HtmlNode *n = html_document_get_element_by_id(doc, ids[k]);
        ASSERT_TRUE(n != NULL);
        bx[k] = css_layout_box_for_node(&ctx, po_array_index_from_payload(&doc->array, n));
        ASSERT_TRUE(bx[k] != NULL);
    }
    bool ok = near_equal(bx[0]->width, 260.0) && near_equal(bx[1]->width, 252.0) &&
              near_equal(bx[0]->x, 540.0) && bx[2]->width <= 258.5;
    css_layout_tree_free(&ctx);
    html_document_free(doc);
    return ok;
}


/* Nested custom properties and math functions (min/max/clamp/round)
 * resolve; Wikipedia sizes thumbnails with
 * round(var(--image-size-user) * var(--mw-file-upright,1), 10px). */
TEST(test_layout_calc_nested_vars) {
    const char *css =
        ":root{--std:250px;--user:var(--std)}"
        "#a{width:calc(var(--user) * var(--up,1))}"
        "#b{width:calc(var(--user) * 0.5)}"
        "#c{width:round(var(--user) * var(--up,1),10px)}"
        "#d{width:round(up,243px,10px)}"
        "#e{width:min(500px,50%)}"
        "#f{width:clamp(100px,10px,300px)}"
        "#g{width:calc(max(20px,1em) + 5px)}";
    const char *html =
        "<html><body style=\"margin:0\"><div id=\"a\">x</div><div id=\"b\">y</div>"
        "<div id=\"c\"></div><div id=\"d\"></div><div id=\"e\"></div><div id=\"f\"></div><div id=\"g\"></div></body></html>";
    HtmlDocument *doc = html_parse(html, strlen(html));
    ASSERT_TRUE(doc != NULL);
    CssStylesheet *sheet = css_stylesheet_parse(css, strlen(css));
    ASSERT_TRUE(sheet != NULL);
    LayoutContext ctx = {0};
    ASSERT_TRUE(css_layout_run(&ctx, doc, sheet, 800.0, 600.0));
    HtmlNode *a = html_document_get_element_by_id(doc, "a");
    HtmlNode *b = html_document_get_element_by_id(doc, "b");
    LayoutBox *ba = css_layout_box_for_node(&ctx, po_array_index_from_payload(&doc->array, a));
    LayoutBox *bb = css_layout_box_for_node(&ctx, po_array_index_from_payload(&doc->array, b));
    bool ok = near_equal(ba->width, 250.0) && near_equal(bb->width, 125.0);
    static const struct { const char *id; double w; } more[] = {
        { "c", 250.0 }, { "d", 250.0 }, { "e", 400.0 }, { "f", 100.0 }, { "g", 25.0 },
    };
    for (size_t k = 0; k < sizeof(more) / sizeof(more[0]); k++) {
        HtmlNode *n = html_document_get_element_by_id(doc, more[k].id);
        LayoutBox *bm = n ? css_layout_box_for_node(&ctx, po_array_index_from_payload(&doc->array, n)) : NULL;
        if (!bm || !near_equal(bm->width, more[k].w)) {
            printf("    #%s: width %.2f, want %.0f\n", more[k].id, bm ? bm->width : -1.0, more[k].w);
            ok = false;
        }
    }
    css_layout_tree_free(&ctx);
    html_document_free(doc);
    return ok;
}


/* Preserved newlines force line breaks under pre, pre-wrap and pre-line;
 * white-space:normal folds them into spaces. */
TEST(test_layout_preserved_newlines_break_lines) {
    ASSERT_TRUE(display_list_get_default_font() != NULL);   /* a system sans-serif */
    const char *html =
        "<html><body style=\"margin:0;font-size:16px;line-height:20px\">"
        "<div id=\"n\" style=\"width:400px\">a\nb\nc</div>"
        "<div id=\"p\" style=\"width:400px;white-space:pre\">a\nb\nc</div>"
        "<div id=\"w\" style=\"width:400px;white-space:pre-wrap\">a\nb\nc</div>"
        "<div id=\"l\" style=\"width:400px;white-space:pre-line\">a  \n  b\nc</div>"
        "<div id=\"y\" style=\"width:400px\"><span style=\"white-space:pre-wrap\"><span>one\n\ntwo </span>"
        "<span><a id=\"ya\">link</a></span></span></div>"
        "</body></html>";
    HtmlDocument *doc = html_parse(html, strlen(html));
    ASSERT_TRUE(doc != NULL);
    LayoutContext ctx = {0};
    ASSERT_TRUE(css_layout_run(&ctx, doc, NULL, 800.0, 600.0));
    static const struct { const char *id; double h; } want[] = {
        { "n", 20.0 }, { "p", 60.0 }, { "w", 60.0 }, { "l", 60.0 }, { "y", 60.0 },
    };
    bool ok = true;
    for (size_t k = 0; k < sizeof(want) / sizeof(want[0]); k++) {
        HtmlNode *n = html_document_get_element_by_id(doc, want[k].id);
        LayoutBox *b = n ? css_layout_box_for_node(&ctx, po_array_index_from_payload(&doc->array, n)) : NULL;
        if (!b || !near_equal(b->height, want[k].h)) {
            printf("    #%s: height %.2f, want %.0f\n", want[k].id, b ? b->height : -1.0, want[k].h);
            ok = false;
        }
    }
    /* The link follows "two " on the third line. */
    HtmlNode *ya = html_document_get_element_by_id(doc, "ya");
    LayoutBox *yb = ya ? css_layout_box_for_node(&ctx, po_array_index_from_payload(&doc->array, ya)) : NULL;
    HtmlNode *yd = html_document_get_element_by_id(doc, "y");
    LayoutBox *ydb = yd ? css_layout_box_for_node(&ctx, po_array_index_from_payload(&doc->array, yd)) : NULL;
    if (!yb || !ydb || !near_equal(yb->y - ydb->y, 40.0) || yb->x < 20.0) {
        printf("    #ya at %.1f,%.1f, want y 40 after \"two \"\n", yb ? yb->x : -1.0, yb ? yb->y : -1.0);
        ok = false;
    }
    css_layout_tree_free(&ctx);
    html_document_free(doc);
    return ok;
}

/* white-space:pre / nowrap: inline-blocks stay on one line (YouTube's
 * collapsed info row), a pre flex item keeps its one-line width, and a
 * nowrap span inside normal text still moves to the next line whole. */
TEST(test_layout_nowrap_atomic_inlines) {
    const char *html =
        "<html><body style=\"margin:0;font-size:16px;line-height:20px\">"
        "<div id=\"p\" style=\"width:200px;white-space:pre\">"
        "<a id=\"a1\" style=\"display:inline-block;width:100px\">x</a>"
        "<a id=\"a2\" style=\"display:inline-block;width:100px\">y</a>"
        "<a id=\"a3\" style=\"display:inline-block;width:100px\">z</a></div>"
        "<div style=\"display:flex;width:150px\"><div id=\"f\" style=\"white-space:pre\">"
        "<a style=\"display:inline-block;width:100px\">x</a><a style=\"display:inline-block;width:100px\">y</a>"
        "</div></div>"
        "<div id=\"n\" style=\"width:100px\">aaaa aaaa <span id=\"ns\" style=\"white-space:nowrap\">bb bb</span></div>"
        "</body></html>";
    HtmlDocument *doc = html_parse(html, strlen(html));
    ASSERT_TRUE(doc != NULL);
    LayoutContext ctx = {0};
    ASSERT_TRUE(css_layout_run(&ctx, doc, NULL, 800.0, 600.0));
    const char *ids[6] = { "p", "a1", "a2", "a3", "f", "ns" };
    LayoutBox *bx[6];
    for (int k = 0; k < 6; k++) {
        HtmlNode *n = html_document_get_element_by_id(doc, ids[k]);
        ASSERT_TRUE(n != NULL);
        bx[k] = css_layout_box_for_node(&ctx, po_array_index_from_payload(&doc->array, n));
        ASSERT_TRUE(bx[k] != NULL);
    }
    bool ok = near_equal(bx[1]->y, bx[3]->y) && near_equal(bx[3]->x, bx[1]->x + 200.0) &&
              near_equal(bx[0]->height, 20.0) && bx[4]->width >= 199.5 && bx[5]->y >= 59.0;
    if (!ok)
        printf("    a1 %.1f,%.1f a3 %.1f,%.1f p.h %.1f f.w %.1f ns.y %.1f\n", bx[1]->x, bx[1]->y, bx[3]->x, bx[3]->y,
               bx[0]->height, bx[4]->width, bx[5]->y);
    css_layout_tree_free(&ctx);
    html_document_free(doc);
    return ok;
}

/* mask-image: linear-gradient() wraps the box's subtree in MASK_BEGIN /
 * MASK_END with the angle and alpha stops parsed; borders paint only the
 * sides that have them (no extra outline). */
TEST(test_layout_mask_gradient_display_list) {
    const char *html =
        "<html><body style=\"margin:0\">"
        "<div id=\"m\" style=\"width:200px;height:40px;-webkit-mask-image:linear-gradient(90deg,rgb(0,0,0) 90%,rgba(0,0,0,0))\">"
        "<div style=\"width:100px;height:20px;background:#f00\"></div></div>"
        "<div style=\"width:100px;height:10px;border-bottom:1px solid #000\"></div>"
        "<div style=\"width:100px;height:10px;background:#0f0;border:1px solid #00f\"></div>"
        "</body></html>";
    HtmlDocument *doc = html_parse(html, strlen(html));
    ASSERT_TRUE(doc != NULL);
    LayoutContext ctx = {0};
    ASSERT_TRUE(css_layout_run(&ctx, doc, NULL, 800.0, 600.0));
    DisplayList dl;
    display_list_init(&dl);
    ASSERT_TRUE(css_layout_build_display_list(&ctx, &dl));
    int begin = -1, end = -1, red = -1, black_rects = 0, outlines = 0, green = -1, blue = -1;
    for (int i = 0; i < dl.count; i++) {
        const DisplayListCmd *c = &dl.cmds[i];
        if (c->type == DL_MASK_BEGIN) begin = i;
        else if (c->type == DL_MASK_END) end = i;
        else if (c->type == DL_RECT && c->r > 0.9f && c->g < 0.1f) red = i;
        else if (c->type == DL_RECT && c->r < 0.1f && c->g < 0.1f && c->b < 0.1f) black_rects++;
        else if (c->type == DL_BORDER) outlines++;
        if (c->type == DL_RECT && c->g > 0.9f && c->r < 0.1f) green = i;
        if (c->type == DL_RECT && c->b > 0.9f && c->r < 0.1f && c->g < 0.1f && blue < 0) blue = i;
    }
    bool ok = begin >= 0 && red > begin && end > red && dl.mask_count == 1 && black_rects == 1 && outlines == 0 && green >= 0 && blue > green;
    if (ok) {
        const DisplayMask *m = &dl.masks[0];
        ok = m->kind == 2 && near_equal(m->angle_deg, 90.0) && m->nstops == 2 &&
             near_equal(m->stop_pos[0], 0.9) && near_equal(m->stop_alpha[0], 1.0) &&
             near_equal(m->stop_pos[1], 1.0) && near_equal(m->stop_alpha[1], 0.0);
    }
    if (!ok) printf("    begin %d red %d end %d masks %d black %d outlines %d\n", begin, red, end, dl.mask_count,
                    black_rects, outlines);
    display_list_free(&dl);
    css_layout_tree_free(&ctx);
    html_document_free(doc);
    return ok;
}

/* Line-clamp / text-overflow truncation: the cut text wraps to exactly the
 * allowed lines (or fits the width) and ends with an ellipsis; text that
 * already fits is left alone. */
TEST(test_text_truncate_ellipsis) {
    TextShaper *f = display_list_get_default_font();
    ASSERT_TRUE(f != NULL);
    const char *text = "one two three four five six seven eight nine ten eleven twelve";
    char *t = text_shaper_truncate_lines(f, text, 0, 0, 80, 80, 1.0f, 2);
    ASSERT_TRUE(t != NULL);
    size_t n = strlen(t);
    ASSERT_TRUE(n >= 3 && strcmp(t + n - 3, "\xE2\x80\xA6") == 0);
    TsWrapResult wr;
    ASSERT_TRUE(text_shaper_wrap_measure(f, t, 0, 0, 80, 80, 1.0f, 20.0f, 0, 0, 0, &wr));
    ASSERT_TRUE(wr.lines == 2);
    free(t);
    ASSERT_TRUE(text_shaper_truncate_lines(f, "short", 0, 0, 80, 80, 1.0f, 2) == NULL);
    char *w = text_shaper_truncate_width(f, "Hello wonderful world", 1.0f, 60.0f);
    ASSERT_TRUE(w != NULL);
    float ww = 0;
    text_shaper_measure(f, w, &ww, NULL);
    ASSERT_TRUE(ww <= 60.01f && strcmp(w + strlen(w) - 3, "\xE2\x80\xA6") == 0);
    free(w);
    ASSERT_TRUE(text_shaper_truncate_width(f, "Hi", 1.0f, 60.0f) == NULL);
    return true;
}

static uint32_t be32_at(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

/* Wrap an sfnt as WOFF 1.0 with uncompressed tables (valid WOFF). */
static uint8_t *make_woff1(const uint8_t *sfnt, size_t len, size_t *out_len)
{
    if (len < 12) return NULL;
    int n = (sfnt[4] << 8) | sfnt[5];
    size_t total = 44 + (size_t)n * 20;
    for (int i = 0; i < n; i++) total += (be32_at(sfnt + 12 + i * 16 + 12) + 3) & ~3u;
    uint8_t *w = (uint8_t *)calloc(1, total);
    if (!w) return NULL;
    memcpy(w, "wOFF", 4);
    memcpy(w + 4, sfnt, 4);
    put32(w + 8, (uint32_t)total);
    w[12] = (uint8_t)(n >> 8); w[13] = (uint8_t)n;
    put32(w + 16, (uint32_t)len);
    w[20] = 1;
    size_t off = 44 + (size_t)n * 20;
    for (int i = 0; i < n; i++) {
        const uint8_t *rec = sfnt + 12 + i * 16;
        uint32_t toff = be32_at(rec + 8), tlen = be32_at(rec + 12);
        uint8_t *d = w + 44 + i * 20;
        memcpy(d, rec, 4);
        put32(d + 4, (uint32_t)off);
        put32(d + 8, tlen);
        put32(d + 12, tlen);
        memcpy(d + 16, rec + 4, 4);
        if (toff + (size_t)tlen <= len) memcpy(w + off, sfnt + toff, tlen);
        off += (tlen + 3) & ~3u;
    }
    *out_len = total;
    return w;
}

static char *base64_encode(const uint8_t *p, size_t n)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *o = (char *)malloc((n + 2) / 3 * 4 + 1);
    if (!o) return NULL;
    size_t k = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < n ? (uint32_t)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        o[k++] = t[(v >> 18) & 63];
        o[k++] = t[(v >> 12) & 63];
        o[k++] = i + 1 < n ? t[(v >> 6) & 63] : '=';
        o[k++] = i + 2 < n ? t[v & 63] : '=';
    }
    o[k] = '\0';
    return o;
}

/* System fonts resolve by generic and real family names, weight and style;
 * an @font-face web font (a system serif wrapped as WOFF in a data: URL) is
 * queued on first use, loaded, then chosen over the fallback family. */
TEST(test_font_registry_system_and_web_fonts) {
    int sans = font_registry_resolve("sans-serif", 400, false);
    int bold = font_registry_resolve("sans-serif", 700, false);
    ASSERT_TRUE(sans >= 0 && bold >= 0);
    ASSERT_TRUE(font_registry_resolve("\"No Such Family\", sans-serif", 400, false) == sans);
#ifdef _WIN32
    ASSERT_TRUE(bold != sans);
    ASSERT_TRUE(font_registry_resolve("serif", 400, false) != sans);
    ASSERT_TRUE(font_registry_resolve("Arial", 400, false) == sans);
#endif

    char path[1024];
    int index = 0;
    ASSERT_TRUE(font_registry_family_file("serif", path, sizeof(path), &index));
    FILE *f = fopen(path, "rb");
    ASSERT_TRUE(f != NULL);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *ttf = (uint8_t *)malloc((size_t)sz);
    ASSERT_TRUE(ttf && fread(ttf, 1, (size_t)sz, f) == (size_t)sz);
    fclose(f);
    if (index != 0 || be32_at(ttf) == 0x74746366u) { free(ttf); printf("    (skipped - serif is a collection)"); return true; }

    size_t wlen = 0;
    uint8_t *woff = make_woff1(ttf, (size_t)sz, &wlen);
    ASSERT_TRUE(woff != NULL);
    uint8_t *back = NULL;
    size_t blen = 0;
    ASSERT_TRUE(font_decode_web(woff, wlen, &back, &blen));
    free(back);

    char *b64 = base64_encode(woff, wlen);
    ASSERT_TRUE(b64 != NULL);
    size_t clen = strlen(b64) + 256;
    char *css = (char *)malloc(clen);
    snprintf(css, clen, "@font-face{font-family:'Test Web Face';font-weight:400;"
                        "src:url(data:font/woff;base64,%s) format('woff');unicode-range:U+0000-00FF}", b64);
    font_registry_scan_css(css, strlen(css), "https://example.test/");
    int before = font_registry_resolve("'Test Web Face', sans-serif", 400, false);
    ASSERT_TRUE(before == sans);                              /* queued; sans stands in */
    ASSERT_TRUE(font_registry_load_pending() == 1);
    int web = font_registry_resolve("'Test Web Face', sans-serif", 400, false);
    ASSERT_TRUE(web >= 0 && web != sans);
    float w_web = 0, w_sys = 0;
    TextShaper *sys = text_shaper_create_from_memory(ttf, (size_t)sz, 0, 16.0f);   /* takes ttf */
    ASSERT_TRUE(sys != NULL);
    ASSERT_TRUE(text_shaper_measure(font_registry_slot(web), "Hamburgefonstiv", &w_web, NULL));
    ASSERT_TRUE(text_shaper_measure(sys, "Hamburgefonstiv", &w_sys, NULL));
    text_shaper_destroy(sys);
    free(css);
    free(b64);
    free(woff);
    ASSERT_TRUE(w_web > 0.0f && w_web == w_sys);
    return true;
}

static float measure_sfnt(const uint8_t *sfnt, size_t len, const char *text)
{
    uint8_t *copy = (uint8_t *)malloc(len);
    if (!copy) return -1.0f;
    memcpy(copy, sfnt, len);
    TextShaper *s = text_shaper_create_from_memory(copy, len, 0, 16.0f);
    float w = -1.0f;
    if (s) text_shaper_measure(s, text, &w, NULL);
    text_shaper_destroy(s);
    return w;
}

/* A variable font instanced at its default weight measures exactly like the
 * font itself; heavier instances are wider (gvar phantom-point advances) and
 * stay loadable. */
TEST(test_font_variable_instances) {
    char path[1024];
    int index = 0;
    if (!font_registry_family_file("bahnschrift", path, sizeof(path), &index)) {
        printf("    (skipped - no Bahnschrift variable font)");
        return true;
    }
    FILE *f = fopen(path, "rb");
    ASSERT_TRUE(f != NULL);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *ttf = (uint8_t *)malloc((size_t)sz);
    ASSERT_TRUE(ttf && fread(ttf, 1, (size_t)sz, f) == (size_t)sz);
    fclose(f);
    ASSERT_TRUE(font_is_variable(ttf, (size_t)sz));
    float wmin = 0, wdef = 0, wmax = 0;
    ASSERT_TRUE(font_axis_range(ttf, (size_t)sz, FONT_AXIS_TAG('w', 'g', 'h', 't'), &wmin, &wdef, &wmax));
    ASSERT_TRUE(wmin < wdef && wdef < wmax);

    const char *text = "Hamburgefonstiv 0123456789";
    float base = measure_sfnt(ttf, (size_t)sz, text);
    float widths[3];
    float weights[3] = { wmin, wdef, wmax };
    for (int i = 0; i < 3; i++) {
        FontAxisValue v = { FONT_AXIS_TAG('w', 'g', 'h', 't'), weights[i] };
        uint8_t *inst = NULL;
        size_t ilen = 0;
        ASSERT_TRUE(font_instance(ttf, (size_t)sz, &v, 1, &inst, &ilen));
        ASSERT_TRUE(!font_is_variable(inst, ilen));
        widths[i] = measure_sfnt(inst, ilen, text);
        free(inst);
    }
    float dmin = 0, ddef = 0, dmax = 0, narrow = -1.0f;
    bool has_wdth = font_axis_range(ttf, (size_t)sz, FONT_AXIS_TAG('w', 'd', 't', 'h'), &dmin, &ddef, &dmax);
    if (has_wdth && dmin < ddef) {
        FontAxisValue v = { FONT_AXIS_TAG('w', 'd', 't', 'h'), dmin };
        uint8_t *inst = NULL;
        size_t ilen = 0;
        ASSERT_TRUE(font_instance(ttf, (size_t)sz, &v, 1, &inst, &ilen));
        narrow = measure_sfnt(inst, ilen, text);
        free(inst);
    }
    /* Through the registry: a variable @font-face with a weight range gets one
     * instance per weight used, reused on the next lookup. */
    char *b64 = base64_encode(ttf, (size_t)sz);
    ASSERT_TRUE(b64 != NULL);
    size_t clen = strlen(b64) + 256;
    char *css = (char *)malloc(clen);
    snprintf(css, clen, "@font-face{font-family:'Var Test';font-weight:300 700;"
                        "src:url(data:font/ttf;base64,%s) format('truetype')}", b64);
    font_registry_scan_css(css, strlen(css), "https://example.test/");
    font_registry_resolve("'Var Test'", 400, false);
    ASSERT_TRUE(font_registry_load_pending() == 1);
    int light = font_registry_resolve("'Var Test'", 300, false);
    int heavy = font_registry_resolve("'Var Test'", 700, false);
    ASSERT_TRUE(light >= 0 && heavy >= 0 && light != heavy);
    ASSERT_TRUE(font_registry_resolve("'Var Test', serif", 700, false) == heavy);
    free(css);
    free(b64);
    free(ttf);
    printf("    wght %.0f/%.0f/%.0f: %.2f %.2f %.2f, wdth %.0f: %.2f (font %.2f)\n", weights[0], weights[1],
           weights[2], widths[0], widths[1], widths[2], dmin, narrow, base);
    ASSERT_TRUE(base > 0.0f && widths[1] == base);
    ASSERT_TRUE(widths[0] <= widths[1] && widths[1] <= widths[2]);
    if (has_wdth && dmin < ddef) ASSERT_TRUE(narrow > 0.0f && narrow < base);
    return true;
}

void run_css_layout_tests(void) {
    printf("\n--- CSS Layout Engine Tests ---\n");
    RUN_TEST(test_layout_basic_document);
    RUN_TEST(test_layout_stylesheet);
    RUN_TEST(test_layout_auto_height);
    RUN_TEST(test_layout_box_sizing_content_box);
    RUN_TEST(test_layout_box_sizing_border_box);
    RUN_TEST(test_layout_box_sizing_min_max);
    RUN_TEST(test_layout_flex_box_sizing);
    RUN_TEST(test_layout_flex_basis_box_sizing);
    RUN_TEST(test_layout_display_list);
    RUN_TEST(test_layout_background_image_url);
    RUN_TEST(test_layout_img_display_list);
    RUN_TEST(test_text_shaper_basic);
    RUN_TEST(test_dom_mutation_sync_to_native);
    RUN_TEST(test_async_image_load_callback);
    RUN_TEST(test_layout_percentages_and_aspect_ratio);
    RUN_TEST(test_layout_percentage_sizes_in_calc);
    RUN_TEST(test_layout_css_wide_keywords);
    RUN_TEST(test_layout_pre_trailing_spaces);
    RUN_TEST(test_layout_float_table_figure);
    RUN_TEST(test_layout_calc_nested_vars);
    RUN_TEST(test_layout_preserved_newlines_break_lines);
    RUN_TEST(test_layout_nowrap_atomic_inlines);
    RUN_TEST(test_layout_mask_gradient_display_list);
    RUN_TEST(test_text_truncate_ellipsis);
    RUN_TEST(test_font_registry_system_and_web_fonts);
    RUN_TEST(test_font_variable_instances);
}

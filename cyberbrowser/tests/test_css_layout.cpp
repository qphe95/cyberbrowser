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

extern "C" void run_css_layout_tests(void);
extern "C" JSContextHandle get_shared_test_context(void);
extern "C" GCValue get_shared_test_global(void);

static bool near_equal(double a, double b) {
    return (a > b ? a - b : b - a) < 0.001;
}

TEST(test_layout_basic_document) {
    const char *html = "<html><body><div>Hello</div></body></html>";
    HtmlDocument *doc = html_parse(html, strlen(html));
    ASSERT_TRUE(doc != NULL);

    LayoutContext ctx;
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
    LayoutContext ctx;
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

    LayoutContext ctx;
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

    LayoutContext ctx;
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

    LayoutContext ctx;
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

    LayoutContext ctx;
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

    LayoutContext ctx;
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

    LayoutContext ctx;
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

    LayoutContext ctx;
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

    LayoutContext ctx;
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

    LayoutContext ctx;
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
    const char *paths[] = {
        "cyberbrowser/third_party/fonts/Roboto-Regular.ttf",
        "../../third_party/fonts/Roboto-Regular.ttf",
        "../third_party/fonts/Roboto-Regular.ttf",
        "third_party/fonts/Roboto-Regular.ttf",
        NULL
    };
    TextShaper *shaper = NULL;
    for (int i = 0; paths[i]; i++) {
        shaper = text_shaper_create(paths[i], 16.0f);
        if (shaper) break;
    }
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

    text_shaper_destroy(shaper);
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

    LayoutContext ctx_layout;
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

TEST(test_select_best_media_url) {
    JsExecResult result;
    memset(&result, 0, sizeof(result));
    result.captured_url_count = 5;

    snprintf(result.captured_urls[0], sizeof(result.captured_urls[0]),
             "https://r1---sn-foo.googlevideo.com/videoplayback?mime=video/mp4&itag=137&sabr=1&sig=abc");
    snprintf(result.captured_urls[1], sizeof(result.captured_urls[1]),
             "https://r2---sn-bar.googlevideo.com/videoplayback?mime=audio/mp4&itag=140&sig=def");
    snprintf(result.captured_urls[2], sizeof(result.captured_urls[2]),
             "https://r3---sn-baz.googlevideo.com/videoplayback?mime=video/mp4&itag=18");
    snprintf(result.captured_urls[3], sizeof(result.captured_urls[3]),
             "https://example.com/not-a-googlevideo-url.mp4");
    snprintf(result.captured_urls[4], sizeof(result.captured_urls[4]),
             "https://r4---sn-qux.googlevideo.com/videoplayback?mime=video/mp4&itag=22");

    char url[2048] = {0};
    char mime[64] = {0};

    /* Audio preference should pick the audio itag=140 URL. */
    ASSERT_TRUE(html_select_best_media_url(&result, false, url, sizeof(url), mime, sizeof(mime)));
    ASSERT_TRUE(strstr(url, "itag=140") != NULL);
    ASSERT_TRUE(strcmp(mime, "audio/mp4") == 0);

    /* Video preference should pick a preferred video itag (18 or 22),
     * not the SABR URL and not the audio URL. */
    memset(url, 0, sizeof(url));
    memset(mime, 0, sizeof(mime));
    ASSERT_TRUE(html_select_best_media_url(&result, true, url, sizeof(url), mime, sizeof(mime)));
    ASSERT_TRUE(strstr(url, "sabr=1") == NULL);
    ASSERT_TRUE(strstr(url, "itag=140") == NULL);
    ASSERT_TRUE(strstr(url, "itag=18") != NULL || strstr(url, "itag=22") != NULL);
    ASSERT_TRUE(strcmp(mime, "video/mp4") == 0);

    return true;
}

TEST(test_extract_yt_player_response_media) {
    const char *html =
        "<!DOCTYPE html><html><head>"
        "<script>var ytInitialPlayerResponse = {"
        "  \"videoDetails\": {"
        "    \"title\": \"Test Video\","
        "    \"thumbnails\": ["
        "      {\"url\": \"https://thumb1.example.com/low.jpg\"},"
        "      {\"url\": \"https://thumb2.example.com/high.jpg\"}"
        "    ]"
        "  },"
        "  \"streamingData\": {"
        "    \"formats\": ["
        "      {\"itag\": 18, \"mimeType\": \"video/mp4; codecs=...\", \"url\": \"https://r1---sn-foo.googlevideo.com/videoplayback?itag=18&mime=video/mp4\"}"
        "    ],"
        "    \"adaptiveFormats\": ["
        "      {\"itag\": 140, \"mimeType\": \"audio/mp4; codecs=...\", \"url\": \"https://r2---sn-bar.googlevideo.com/videoplayback?itag=140&mime=audio/mp4\"}"
        "    ]"
        "  }"
        "};</script></head><body></body></html>";

    char url[2048] = {0};
    char mime[64] = {0};
    char title[256] = {0};
    char thumbnail[2048] = {0};

    ASSERT_TRUE(html_extract_yt_player_response_media(html, false,
                                                       url, sizeof(url),
                                                       mime, sizeof(mime),
                                                       title, sizeof(title),
                                                       thumbnail, sizeof(thumbnail)));
    ASSERT_TRUE(strstr(url, "itag=140") != NULL);
    ASSERT_TRUE(strcmp(mime, "audio/mp4") == 0);
    ASSERT_TRUE(strcmp(title, "Test Video") == 0);
    ASSERT_TRUE(strcmp(thumbnail, "https://thumb2.example.com/high.jpg") == 0);

    memset(url, 0, sizeof(url));
    memset(mime, 0, sizeof(mime));
    ASSERT_TRUE(html_extract_yt_player_response_media(html, true,
                                                       url, sizeof(url),
                                                       mime, sizeof(mime),
                                                       NULL, 0, NULL, 0));
    ASSERT_TRUE(strstr(url, "itag=18") != NULL);
    ASSERT_TRUE(strcmp(mime, "video/mp4") == 0);

    return true;
}

extern "C" void run_css_layout_tests(void) {
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
    RUN_TEST(test_select_best_media_url);
    RUN_TEST(test_extract_yt_player_response_media);
}

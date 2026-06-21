/*
 * CSS Layout Engine Tests
 */

#include <stdio.h>
#include <string.h>
#include "test_runner.h"
#include "html_dom.h"
#include "css_parser.h"
#include "css_layout.h"
#include "display_list.h"

extern "C" void run_css_layout_tests(void);

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
    ASSERT_TRUE(near_equal(box->width, 200.0));
    ASSERT_TRUE(near_equal(box->height, 100.0));
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

extern "C" void run_css_layout_tests(void) {
    printf("\n--- CSS Layout Engine Tests ---\n");
    RUN_TEST(test_layout_basic_document);
    RUN_TEST(test_layout_stylesheet);
    RUN_TEST(test_layout_auto_height);
    RUN_TEST(test_layout_display_list);
}

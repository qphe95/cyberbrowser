/* Image cache: early size detection, worker decoding, scaled decodes. */
#include "test_runner.h"
#include "image_cache.h"
#include "svg_render.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

void run_image_cache_tests(void);

/* Fixtures (generated with Pillow): a 640x480 JPEG, left half red / right
 * half blue; a 300x200 green lossy WebP; a 20x10 PNG. */
static const char k_jpeg_640x480[] = "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAYEBQYFBAYGBQYHBwYIChAKCgkJChQODwwQFxQYGBcUFhYaHSUfGhsjHBYWICwgIyYnKSopGR8tMC0oMCUoKSj/2wBDAQcHBwoIChMKChMoGhYaKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCj/wAARCAHgAoADASIAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwDz+iiivmD9yCiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigDjqKKK/pk/nsKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKAOxooor+Zj+hAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooA46iiiv6ZP57CiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigDsaKKK/mY/oQKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKAOOooor+mT+ewooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooA7Giiiv5mP6ECiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigDjqKKK/pk/nsKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKAOxooor+Zj+hAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooA46iiiv6ZP57CiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigDsaKKK/mY/oQKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKAOOooor+mT+ewooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooA7Giiiv5mP6ECiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigDjqKKK/pk/nsKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKAOxooor+Zj+hAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooA46iiiv6ZP57CiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigDsaKKK/mY/oQKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKAOOooor+mT+ewooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooA7Giiiv5mP6ECiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigDjqKKK/pk/nsKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKAOxooor+Zj+hAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooA46iiiv6ZP57CiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigDsaKKK/mY/oQKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKAOOooor+mT+ewooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooA7Giiiv5mP6ECiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigDjqKKK/pk/nsKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKAOxooor+Zj+hAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooA46iiiv6ZP57CiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigDsaKKK/mY/oQKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKAOOooor+mT+ewooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooA7Giiiv5mP6ECiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigDjqKKK/pk/nsKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKAOxooor+Zj+hAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooA46iiiv6ZP57CiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigDsaKKK/mY/oQKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKAOOooor+mT+ewooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooA7Giiiv5mP6ECiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigDjqKKK/pk/nsKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKAOxooor+Zj+hAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooA46iiiv6ZP57CiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigDsaKKK/mY/oQKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKAOOooor+mT+ewooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooA7Giiiv5mP6ECiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigDjqKKK/pk/nsKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKAOxooor+Zj+hAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooA46iiiv6ZP57CiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigDsaKKK/mY/oQKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKAOOooor+mT+ewooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooA7Giiiv5mP6ECiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigDjqKKK/pk/nsKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKAOxooor+Zj+hAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooA46iiiv6ZP57CiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigDsaKKK/mY/oQKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKAOOooor+mT+ewooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooA7Giiiv5mP6ECiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigDjqKKK/pk/nsKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKAOxooor+Zj+hAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooA46iiiv6ZP57CiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigDsaKKK/mY/oQKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKAOOooor+mT+ewooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooA//2Q==";
static const char k_webp_300x200[] = "UklGRsAAAABXRUJQVlA4ILQAAADwEACdASosAcgAPm02mUmkIyKhICgAgA2JaW7hdrEe3AfgAAAYYRcnIe+2TkPfbJyHvtk5D32ych77ZOQ99snIe+2TkPfbJyHvtk5D32ych77ZOQ99snIe+2TkPfbJyHvtk5D32ych77ZOQ99snIe+2TkPfbJyHvtk5D32ych77ZOQ99snIe+2TkPfbJyHvSAA/uxv//87Baqzfel//+vR/PR/PR/qdR4IiAKAAAAAAAAAAAA=";
static const char k_png_20x10[] = "iVBORw0KGgoAAAANSUhEUgAAABQAAAAKCAYAAAC0VX7mAAAAG0lEQVR4nGPkEpH7z0BFwERNw0YNHDVwsBgIAAMwAU9XEUiwAAAAAElFTkSuQmCC";

static char *data_url(const char *mime, const char *b64)
{
    size_t n = strlen(mime) + strlen(b64) + 32;
    char *u = malloc(n);
    snprintf(u, n, "data:%s;base64,%s", mime, b64);
    return u;
}

TEST(test_image_cache_natural_size_before_pixels) {
    ImageCache *c = image_cache_create();
    char *u = data_url("image/jpeg", k_jpeg_640x480);
    int w = 0, h = 0;
    image_cache_natural_size(c, u, &w, &h);       /* starts the fetch */
    image_cache_wait_pending(c);
    ASSERT_TRUE(image_cache_natural_size(c, u, &w, &h));
    ASSERT_EQ(640, w);
    ASSERT_EQ(480, h);
    ASSERT_EQ(1, image_cache_status(c, u));
    free(u);
    image_cache_destroy(c);
    return true;
}

TEST(test_image_cache_jpeg_scaled_decode) {
    ImageCache *c = image_cache_create();
    char *u = data_url("image/jpeg", k_jpeg_640x480);
    /* Painted at 160x120: libjpeg-turbo decodes at 1/4 scale. */
    int handle = image_cache_request(c, u, 160, 120);
    ASSERT_TRUE(handle >= 0);
    image_cache_wait_pending(c);
    int w = 0, h = 0, ch = 0;
    uint8_t *px = NULL;
    ASSERT_TRUE(image_cache_get(c, handle, &w, &h, &ch, &px));
    ASSERT_EQ(160, w);
    ASSERT_EQ(120, h);
    ASSERT_EQ(4, ch);
    /* Left half red, right half blue (allow JPEG error). */
    const uint8_t *l = px + (60 * w + 20) * 4, *r = px + (60 * w + 140) * 4;
    ASSERT_TRUE(l[0] > 180 && l[2] < 70);
    ASSERT_TRUE(r[2] > 180 && r[0] < 70);
    int nw = 0, nh = 0;
    ASSERT_TRUE(image_cache_get_natural(c, handle, &nw, &nh));
    ASSERT_EQ(640, nw);
    /* A larger paint size re-decodes from the retained bytes. */
    image_cache_request(c, u, 640, 480);
    image_cache_wait_pending(c);
    ASSERT_TRUE(image_cache_get(c, handle, &w, &h, &ch, &px));
    ASSERT_EQ(640, w);
    ASSERT_EQ(480, h);
    free(u);
    image_cache_destroy(c);
    return true;
}

TEST(test_image_cache_webp_scaled_decode) {
    ImageCache *c = image_cache_create();
    char *u = data_url("image/webp", k_webp_300x200);
    int handle = image_cache_request(c, u, 150, 100);
    image_cache_wait_pending(c);
    int w = 0, h = 0, ch = 0;
    uint8_t *px = NULL;
    ASSERT_TRUE(image_cache_get(c, handle, &w, &h, &ch, &px));
    ASSERT_EQ(150, w);
    ASSERT_EQ(100, h);
    const uint8_t *p = px + (50 * w + 75) * 4;
    ASSERT_TRUE(p[1] > 150 && p[0] < 80 && p[3] == 255);
    free(u);
    image_cache_destroy(c);
    return true;
}

TEST(test_image_cache_png_and_svg) {
    ImageCache *c = image_cache_create();
    char *u = data_url("image/png", k_png_20x10);
    int handle = image_cache_request(c, u, 0, 0);
    image_cache_wait_pending(c);
    int w = 0, h = 0, ch = 0;
    uint8_t *px = NULL;
    ASSERT_TRUE(image_cache_get(c, handle, &w, &h, &ch, &px));
    ASSERT_EQ(20, w);
    ASSERT_EQ(10, h);
    ASSERT_EQ(10, px[0]);
    ASSERT_EQ(30, px[2]);
    free(u);
    /* SVG: natural size from the root element, rasterized at paint size. */
    const char *svg = "data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='40' height='20'%3E"
                      "%3Crect width='40' height='20' fill='%23ff0000'/%3E%3C/svg%3E";
    int nw = 0, nh = 0;
    image_cache_natural_size(c, svg, &nw, &nh);
    image_cache_wait_pending(c);
    ASSERT_TRUE(image_cache_natural_size(c, svg, &nw, &nh));
    ASSERT_EQ(40, nw);
    ASSERT_EQ(20, nh);
    int sh = image_cache_request(c, svg, 80, 40);
    image_cache_wait_pending(c);
    ASSERT_TRUE(image_cache_get(c, sh, &w, &h, &ch, &px));
    ASSERT_EQ(80, w);
    ASSERT_EQ(40, h);
    ASSERT_TRUE(px[(20 * w + 40) * 4] > 200);
    image_cache_destroy(c);
    return true;
}

/* A document PlutoSVG cannot parse is destroyed by PlutoSVG, which frees
 * the source copy; freeing it again corrupted the heap (YouTube). */
TEST(test_svg_parse_error_frees_once) {
    const char *bad = "not an svg <svg/>";
    float w = 1, h = 1;
    uint8_t *px = NULL;
    int pw = 0, ph = 0;
    for (int i = 0; i < 200; i++) {
        ASSERT_TRUE(!svg_intrinsic_size(bad, strlen(bad), &w, &h));
        ASSERT_TRUE(!svg_render_rgba(bad, strlen(bad), 10, 10, 0xff, &px, &pw, &ph));
        char *p = (char *)malloc(64);   /* heap still usable */
        ASSERT_TRUE(p != NULL);
        free(p);
    }
    return true;
}

static int g_notes;
static void count_note(const char *url, void *ud) { (void)url; (void)ud; g_notes++; }

TEST(test_image_cache_errors_and_notify) {
    ImageCache *c = image_cache_create();
    g_notes = 0;
    image_cache_set_notify(c, count_note, NULL);
    const char *bad = "data:image/png;base64,AAAAAAAAAAAAAAAAAAAA";
    int handle = image_cache_request(c, bad, 10, 10);
    ASSERT_TRUE(handle >= 0);
    image_cache_wait_pending(c);              /* also runs the callbacks */
    ASSERT_EQ(-1, image_cache_status(c, bad));
    ASSERT_FALSE(image_cache_get(c, handle, NULL, NULL, NULL, NULL));
    ASSERT_TRUE(g_notes >= 1);
    ASSERT_FALSE(image_cache_has_pending(c));
    image_cache_destroy(c);
    return true;
}

TEST(test_image_cache_many_parallel) {
    ImageCache *c = image_cache_create();
    char *urls[24];
    for (int i = 0; i < 24; i++) {
        /* Distinct URLs (a MIME parameter) for the same bytes. */
        size_t n = strlen(k_jpeg_640x480) + 64;
        urls[i] = malloc(n);
        snprintf(urls[i], n, "data:image/jpeg;n=%d;base64,%s", i, k_jpeg_640x480);
        int w = 0, h = 0;
        image_cache_natural_size(c, urls[i], &w, &h);
    }
    image_cache_wait_pending(c);
    ImageCacheStats st;
    image_cache_stats(c, &st);
    ASSERT_EQ(24, st.entries);
    ASSERT_EQ(24, st.ready);
    for (int i = 0; i < 24; i++) free(urls[i]);
    image_cache_destroy(c);
    return true;
}

void run_image_cache_tests(void) {
    printf("\n--- Image cache tests ---\n");
    RUN_TEST(test_image_cache_natural_size_before_pixels);
    RUN_TEST(test_image_cache_jpeg_scaled_decode);
    RUN_TEST(test_image_cache_webp_scaled_decode);
    RUN_TEST(test_image_cache_png_and_svg);
    RUN_TEST(test_svg_parse_error_frees_once);
    RUN_TEST(test_image_cache_errors_and_notify);
    RUN_TEST(test_image_cache_many_parallel);
}

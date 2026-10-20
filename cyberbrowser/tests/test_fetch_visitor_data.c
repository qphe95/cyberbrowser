/*
 * Test: Fetch YouTube watch page and extract visitorData.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#endif

#include "platform.h"
#include "http_download.h"

static bool extract_visitor_data(const char *html, size_t html_len, char *out, size_t out_len) {
    /* Pattern 1: "VISITOR_DATA":"..." */
    const char *p = html;
    const char *end = html + html_len;
    while (p < end) {
        const char *key = "\"VISITOR_DATA\":\"";
        size_t key_len = strlen(key);
        if (p + key_len < end && memcmp(p, key, key_len) == 0) {
            p += key_len;
            size_t i = 0;
            while (p + i < end && p[i] != '"' && i < out_len - 1) {
                out[i] = p[i];
                i++;
            }
            out[i] = '\0';
            return i > 0;
        }
        p++;
    }

    /* Pattern 2: visitorData":"..." (lowercase) */
    p = html;
    while (p < end) {
        const char *key = "visitorData\":\"";
        size_t key_len = strlen(key);
        if (p + key_len < end && memcmp(p, key, key_len) == 0) {
            p += key_len;
            size_t i = 0;
            while (p + i < end && p[i] != '"' && i < out_len - 1) {
                out[i] = p[i];
                i++;
            }
            out[i] = '\0';
            return i > 0;
        }
        p++;
    }

    /* Pattern 3: VISITOR_INFO1_LIVE cookie */
    p = html;
    while (p < end) {
        const char *key = "VISITOR_INFO1_LIVE=";
        size_t key_len = strlen(key);
        if (p + key_len < end && memcmp(p, key, key_len) == 0) {
            p += key_len;
            size_t i = 0;
            while (p + i < end && p[i] != ';' && p[i] != '"' && i < out_len - 1) {
                out[i] = p[i];
                i++;
            }
            out[i] = '\0';
            return i > 0;
        }
        p++;
    }

    return false;
}

int main(int argc, char *argv[]) {
    setbuf(stdout, NULL);
    const char *video_id = "hQkUiegDu1s";
    if (argc > 1) video_id = argv[1];

    printf("=== Fetch VisitorData Test ===\n");
    printf("Video ID: %s\n\n", video_id);

    printf("platform_init...\n");
    if (!platform_init()) { return 1; }
    printf("platform_http_init...\n");
    if (!platform_http_init()) { return 1; }

    char url[512];
    snprintf(url, sizeof(url), "https://www.youtube.com/watch?v=%s", video_id);

    printf("Fetching: %s\n", url);
    HttpBuffer buffer = {0};
    char err[512] = {0};
    printf("Calling http_get_to_memory...\n");
    if (!http_get_to_memory(url, &buffer, err, sizeof(err))) {
        printf("Fetch failed: %s\n", err);
        platform_http_cleanup();
        platform_cleanup();
        return 1;
    }

    printf("Response size: %zu bytes\n", buffer.size);

    /* Save for inspection */
    FILE *f = fopen("watch_page.html", "wb");
    if (f) {
        fwrite(buffer.data, 1, buffer.size > 50000 ? 50000 : buffer.size, f);
        fclose(f);
        printf("Saved first 50KB to watch_page.html\n");
    }

    char visitor_data[1024] = {0};
    if (extract_visitor_data(buffer.data, buffer.size, visitor_data, sizeof(visitor_data))) {
        printf("Extracted visitorData: %.100s...\n", visitor_data);

        /* Now test youtubei with this visitorData */
        printf("\nTesting youtubei API with extracted visitorData...\n");
        const char *api_url = "https://www.youtube.com/youtubei/v1/player?key=AIzaSyAO_FJ2SlqU8Q4STEHLGCilw_Y9_11qcW8&prettyPrint=false";
        const char *headers[] = {
            "Content-Type: application/json",
            "X-Goog-Api-Format-Version: 2"
        };
        char post_body[2048];
        snprintf(post_body, sizeof(post_body),
            "{"
            "\"context\":{"
            "\"client\":{"
            "\"clientName\":\"ANDROID_VR\","
            "\"clientVersion\":\"1.56.21\","
            "\"deviceMake\":\"Oculus\","
            "\"deviceModel\":\"Quest\","
            "\"osName\":\"Android\","
            "\"osVersion\":\"12\","
            "\"hl\":\"en\","
            "\"gl\":\"US\","
            "\"visitorData\":\"%s\""
            "}"
            "},"
            "\"videoId\":\"%s\""
            "}", visitor_data, video_id);

        HttpBuffer response = {0};
        int status = 0;
        char err2[512] = {0};
        if (http_post_to_memory(api_url, post_body, strlen(post_body),
                                headers, 2, &response, &status, err2, sizeof(err2))) {
            printf("API status: %d, size: %zu\n", status, response.size);
            if (response.data && response.size > 0) {
                const char *ps = strstr(response.data, "\"playabilityStatus\"");
                if (ps) {
                    const char *st = strstr(ps, "\"status\":\"");
                    if (st) {
                        st += 10;
                        printf("Playability: ");
                        while (*st && *st != '"') putchar(*st++);
                        printf("\n");
                    }
                }
                int found = 0;
                const char *itags[] = {"140", "251", "18"};
                for (int j = 0; j < 3; j++) {
                    char needle[32];
                    snprintf(needle, sizeof(needle), "\"itag\":%s", itags[j]);
                    if (strstr(response.data, needle)) found++;
                }
                printf("Streams found: %d\n", found);
            }
            http_free_buffer(&response);
        } else {
            printf("API call failed: %s\n", err2);
        }
    } else {
        printf("Could not extract visitorData from watch page HTML\n");
    }

    http_free_buffer(&buffer);
    platform_http_cleanup();
    platform_cleanup();
    printf("\n=== Done ===\n");
    return 0;
}

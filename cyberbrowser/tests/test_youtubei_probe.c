/*
 * Probe the youtubei/v1/player API for a specific video.
 * No QuickJS, no HTML parsing — just the raw API call.
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

int main(int argc, char *argv[]) {
    const char *video_id = "hQkUiegDu1s";
    if (argc > 1) video_id = argv[1];

    printf("=== YouTubei API Probe ===\n");
    printf("Video ID: %s\n\n", video_id);

    if (!platform_init()) {
        fprintf(stderr, "platform_init failed\n");
        return 1;
    }
    if (!platform_http_init()) {
        fprintf(stderr, "platform_http_init failed\n");
        return 1;
    }

    const char *api_url = "https://www.youtube.com/youtubei/v1/player?key=AIzaSyAO_FJ2SlqU8Q4STEHLGCilw_Y9_11qcW8&prettyPrint=false";
    const char *headers[] = {
        "Content-Type: application/json",
        "X-Goog-Api-Format-Version: 2"
    };

    char post_body[1024];
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
        "\"gl\":\"US\""
        "}"
        "},"
        "\"videoId\":\"%s\""
        "}", video_id);

    printf("POST %s\n", api_url);
    printf("Body: %s\n\n", post_body);

    HttpBuffer response = {0};
    int status = 0;
    char err[512] = {0};

    if (!http_post_to_memory(api_url, post_body, strlen(post_body),
                             headers, 2, &response, &status, err, sizeof(err))) {
        fprintf(stderr, "API call failed: %s\n", err);
        platform_http_cleanup();
        platform_cleanup();
        return 1;
    }

    printf("HTTP status: %d\n", status);
    printf("Response size: %zu bytes\n", response.size);

    if (response.data && response.size > 0) {
        /* Save response to file for inspection */
        FILE *f = fopen("youtubei_response.json", "wb");
        if (f) {
            fwrite(response.data, 1, response.size, f);
            fclose(f);
            printf("Saved raw response to youtubei_response.json\n");
        }

        /* Check for playabilityStatus */
        const char *ps = strstr(response.data, "\"playabilityStatus\"");
        if (ps) {
            const char *status = strstr(ps, "\"status\":\"");
            if (status) {
                status += 10;
                printf("\nPlayability status: ");
                while (*status && *status != '"') putchar(*status++);
                printf("\n");
            }
            const char *reason = strstr(ps, "\"reason\":\"");
            if (reason) {
                reason += 10;
                printf("Reason: ");
                while (*reason && *reason != '"') putchar(*reason++);
                printf("\n");
            }
        }

        /* Check for LOGIN_REQUIRED and retry with visitorData */
        if (strstr(response.data, "LOGIN_REQUIRED")) {
            printf("WARNING: LOGIN_REQUIRED detected\n");

            const char *vd_key = "\"visitorData\":\"";
            const char *vd = strstr(response.data, vd_key);
            if (vd) {
                vd += strlen(vd_key);
                char visitor_data[1024] = {0};
                size_t i = 0;
                while (vd[i] && vd[i] != '"' && i < sizeof(visitor_data) - 1) {
                    visitor_data[i] = vd[i];
                    i++;
                }
                visitor_data[i] = '\0';
                printf("Retrying with visitorData...\n");

                char vd_header[1100];
                snprintf(vd_header, sizeof(vd_header), "X-Goog-Visitor-Id: %s", visitor_data);
                const char *headers_with_vd[] = {
                    "Content-Type: application/json",
                    "X-Goog-Api-Format-Version: 2",
                    vd_header
                };

                HttpBuffer response2 = {0};
                int status2 = 0;
                char err2[512] = {0};
                if (http_post_to_memory(api_url, post_body, strlen(post_body),
                                        headers_with_vd, 3, &response2, &status2, err2, sizeof(err2))) {
                    printf("Retry status: %d, size: %zu bytes\n", status2, response2.size);
                    if (response2.data && response2.size > 0) {
                        FILE *f2 = fopen("youtubei_response_retry.json", "wb");
                        if (f2) {
                            fwrite(response2.data, 1, response2.size, f2);
                            fclose(f2);
                            printf("Saved retry response to youtubei_response_retry.json\n");
                        }

                        /* Look for itags in retry response */
                        const char *itags[] = {"140", "139", "251", "18"};
                        const char *labels[] = {"AAC 128kbps", "AAC 48kbps", "Opus 160kbps", "360p MP4"};
                        printf("\nStream availability (retry):\n");
                        for (int j = 0; j < 4; j++) {
                            char needle[32];
                            snprintf(needle, sizeof(needle), "\"itag\":%s", itags[j]);
                            if (strstr(response2.data, needle)) {
                                printf("  itag=%s (%s) - FOUND\n", itags[j], labels[j]);
                            } else {
                                printf("  itag=%s (%s) - NOT FOUND\n", itags[j], labels[j]);
                            }
                        }

                        const char *ps2 = strstr(response2.data, "\"playabilityStatus\"");
                        if (ps2) {
                            const char *status2_str = strstr(ps2, "\"status\":\"");
                            if (status2_str) {
                                status2_str += 10;
                                printf("Retry playability: ");
                                while (*status2_str && *status2_str != '"') putchar(*status2_str++);
                                printf("\n");
                            }
                        }
                    }
                    http_free_buffer(&response2);
                } else {
                    printf("Retry failed: %s\n", err2);
                }
            } else {
                printf("No visitorData found in response, cannot retry\n");
            }
        } else {
            /* Look for itag=140, 139, 251, 18 in the response */
            const char *itags[] = {"140", "139", "251", "18"};
            const char *labels[] = {"AAC 128kbps", "AAC 48kbps", "Opus 160kbps", "360p MP4"};
            printf("\nStream availability:\n");
            for (int i = 0; i < 4; i++) {
                char needle[32];
                snprintf(needle, sizeof(needle), "\"itag\":%s", itags[i]);
                if (strstr(response.data, needle)) {
                    printf("  itag=%s (%s) - FOUND\n", itags[i], labels[i]);
                } else {
                    printf("  itag=%s (%s) - NOT FOUND\n", itags[i], labels[i]);
                }
            }
        }
    }

    http_free_buffer(&response);
    platform_http_cleanup();
    platform_cleanup();
    printf("\n=== Done ===\n");
    return 0;
}

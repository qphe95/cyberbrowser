/*
 * Test: Find the correct browser emulation for youtubei API.
 * Tries multiple client/ header combinations.
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

static bool try_youtubei(const char *video_id, const char *client_name, const char *client_version,
                         const char *device_make, const char *device_model,
                         const char *os_name, const char *os_version,
                         const char *visitor_data,
                         const char **extra_headers, size_t extra_header_count) {
    char api_url[512];
    snprintf(api_url, sizeof(api_url),
        "https://www.youtube.com/youtubei/v1/player?key=AIzaSyAO_FJ2SlqU8Q4STEHLGCilw_Y9_11qcW8&prettyPrint=false");

    char post_body[4096];
    if (visitor_data && visitor_data[0]) {
        snprintf(post_body, sizeof(post_body),
            "{"
            "\"context\":{"
            "\"client\":{"
            "\"clientName\":\"%s\","
            "\"clientVersion\":\"%s\","
            "\"deviceMake\":\"%s\","
            "\"deviceModel\":\"%s\","
            "\"osName\":\"%s\","
            "\"osVersion\":\"%s\","
            "\"hl\":\"en\","
            "\"gl\":\"US\","
            "\"visitorData\":\"%s\""
            "}"
            "},"
            "\"videoId\":\"%s\""
            "}",
            client_name, client_version, device_make, device_model, os_name, os_version,
            visitor_data, video_id);
    } else {
        snprintf(post_body, sizeof(post_body),
            "{"
            "\"context\":{"
            "\"client\":{"
            "\"clientName\":\"%s\","
            "\"clientVersion\":\"%s\","
            "\"deviceMake\":\"%s\","
            "\"deviceModel\":\"%s\","
            "\"osName\":\"%s\","
            "\"osVersion\":\"%s\","
            "\"hl\":\"en\","
            "\"gl\":\"US\""
            "}"
            "},"
            "\"videoId\":\"%s\""
            "}",
            client_name, client_version, device_make, device_model, os_name, os_version,
            video_id);
    }

    size_t header_count = 2 + extra_header_count;
    const char **headers = (const char **)malloc(sizeof(char *) * header_count);
    headers[0] = "Content-Type: application/json";
    headers[1] = "X-Goog-Api-Format-Version: 2";
    for (size_t i = 0; i < extra_header_count; i++) {
        headers[2 + i] = extra_headers[i];
    }

    HttpBuffer response = {0};
    int status = 0;
    char err[512] = {0};

    bool ok = http_post_to_memory(api_url, post_body, strlen(post_body),
                                  headers, header_count, &response, &status, err, sizeof(err));
    free(headers);

    if (!ok || status != 200 || !response.data || response.size == 0) {
        printf("  FAIL: %s (status=%d, size=%zu)\n", err, status, response.size);
        http_free_buffer(&response);
        return false;
    }

    /* Check playability */
    const char *ps = strstr(response.data, "\"playabilityStatus\"");
    if (ps) {
        const char *status_str = strstr(ps, "\"status\":\"");
        if (status_str) {
            status_str += 10;
            char status_buf[64] = {0};
            size_t i = 0;
            while (status_str[i] && status_str[i] != '"' && i < sizeof(status_buf)-1) {
                status_buf[i] = status_str[i];
                i++;
            }
            if (strcmp(status_buf, "OK") == 0) {
                /* Count itags */
                int found = 0;
                const char *itags[] = {"140", "251", "18"};
                for (int j = 0; j < 3; j++) {
                    char needle[32];
                    snprintf(needle, sizeof(needle), "\"itag\":%s", itags[j]);
                    if (strstr(response.data, needle)) found++;
                }
                printf("  SUCCESS: %d streams found (response=%zu bytes)\n", found, response.size);
                http_free_buffer(&response);
                return true;
            } else {
                const char *reason = strstr(ps, "\"reason\":\"");
                if (reason) {
                    reason += 10;
                    printf("  FAIL: playability=%s reason=", status_buf);
                    while (*reason && *reason != '"') putchar(*reason++);
                    printf(" (response=%zu bytes)\n", response.size);
                } else {
                    printf("  FAIL: playability=%s (response=%zu bytes)\n", status_buf, response.size);
                }
            }
        } else {
            printf("  FAIL: no playability status found\n");
        }
    } else {
        printf("  FAIL: no playabilityStatus in response\n");
    }

    http_free_buffer(&response);
    return false;
}

static bool extract_visitor_data_from_file(const char *filename, char *out, size_t out_len) {
    FILE *f = fopen(filename, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *data = (char *)malloc(size + 1);
    if (!data) { fclose(f); return false; }
    fread(data, 1, size, f);
    data[size] = '\0';
    fclose(f);

    const char *key = "\"visitorData\":\"";
    const char *p = strstr(data, key);
    if (p) {
        p += strlen(key);
        size_t i = 0;
        while (p[i] && p[i] != '"' && i < out_len - 1) {
            out[i] = p[i];
            i++;
        }
        out[i] = '\0';
        free(data);
        return i > 0;
    }

    /* Also try VISITOR_INFO1_LIVE cookie format */
    p = strstr(data, "VISITOR_INFO1_LIVE=");
    if (p) {
        p += strlen("VISITOR_INFO1_LIVE=");
        size_t i = 0;
        while (p[i] && p[i] != ';' && p[i] != '"' && i < out_len - 1) {
            out[i] = p[i];
            i++;
        }
        out[i] = '\0';
        free(data);
        return i > 0;
    }

    free(data);
    return false;
}

int main(int argc, char *argv[]) {
    const char *video_id = "hQkUiegDu1s";
    if (argc > 1) video_id = argv[1];

    printf("=== Browser Emulation Test ===\n");
    printf("Video ID: %s\n\n", video_id);

    if (!platform_init()) { return 1; }
    if (!platform_http_init()) { return 1; }

    /* First, extract visitorData from the previous retry response if available */
    char visitor_data[1024] = {0};
    bool has_visitor_data = extract_visitor_data_from_file("youtubei_response.json", visitor_data, sizeof(visitor_data));
    if (!has_visitor_data) {
        has_visitor_data = extract_visitor_data_from_file("youtubei_response_retry.json", visitor_data, sizeof(visitor_data));
    }
    if (has_visitor_data) {
        printf("Extracted visitorData: %.80s...\n\n", visitor_data);
    } else {
        printf("No visitorData found in previous responses\n\n");
    }

    /* Test 1: ANDROID_VR without visitorData (current behavior) */
    printf("Test 1: ANDROID_VR (no visitorData)\n");
    try_youtubei(video_id, "ANDROID_VR", "1.56.21", "Oculus", "Quest", "Android", "12",
                 NULL, NULL, 0);

    /* Test 2: ANDROID_VR with visitorData */
    if (has_visitor_data) {
        printf("Test 2: ANDROID_VR (with visitorData)\n");
        try_youtubei(video_id, "ANDROID_VR", "1.56.21", "Oculus", "Quest", "Android", "12",
                     visitor_data, NULL, 0);
    }

    /* Test 3: WEB without visitorData */
    printf("Test 3: WEB (no visitorData)\n");
    try_youtubei(video_id, "WEB", "2.20260317.05.00", "", "", "Windows", "10.0",
                 NULL, NULL, 0);

    /* Test 4: WEB with visitorData */
    if (has_visitor_data) {
        printf("Test 4: WEB (with visitorData)\n");
        try_youtubei(video_id, "WEB", "2.20260317.05.00", "", "", "Windows", "10.0",
                     visitor_data, NULL, 0);
    }

    /* Test 5: WEB with visitorData + browser headers */
    if (has_visitor_data) {
        printf("Test 5: WEB (with visitorData + Origin/Referer)\n");
        const char *extra[] = {
            "Origin: https://www.youtube.com",
            "Referer: https://www.youtube.com/watch?v=hQkUiegDu1s"
        };
        try_youtubei(video_id, "WEB", "2.20260317.05.00", "", "", "Windows", "10.0",
                     visitor_data, extra, 2);
    }

    /* Test 6: TVHTML5 without visitorData */
    printf("Test 6: TVHTML5 (no visitorData)\n");
    try_youtubei(video_id, "TVHTML5", "7.20250312.10.00", "", "", "Linux", "",
                 NULL, NULL, 0);

    /* Test 7: TVHTML5 with visitorData */
    if (has_visitor_data) {
        printf("Test 7: TVHTML5 (with visitorData)\n");
        try_youtubei(video_id, "TVHTML5", "7.20250312.10.00", "", "", "Linux", "",
                     visitor_data, NULL, 0);
    }

    /* Test 8: WEB_EMBEDDED_PLAYER without visitorData */
    printf("Test 8: WEB_EMBEDDED_PLAYER (no visitorData)\n");
    try_youtubei(video_id, "WEB_EMBEDDED_PLAYER", "2.20260317.05.00", "", "", "Windows", "10.0",
                 NULL, NULL, 0);

    /* Test 9: IOS without visitorData */
    printf("Test 9: IOS (no visitorData)\n");
    try_youtubei(video_id, "IOS", "19.29.1", "Apple", "iPhone16,2", "iOS", "17.5.1",
                 NULL, NULL, 0);

    platform_http_cleanup();
    platform_cleanup();
    printf("\n=== Done ===\n");
    return 0;
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform.h"
#include "http_download.h"

int main() {
    setbuf(stdout, NULL);
    printf("platform_init...\n");
    if (!platform_init()) { return 1; }
    printf("platform_http_init...\n");
    if (!platform_http_init()) { return 1; }

    const char *video_id = "hQkUiegDu1s";
    char url[512];
    snprintf(url, sizeof(url), "https://www.youtube.com/watch?v=%s", video_id);

    printf("Fetching watch page...\n");
    HttpBuffer html = {0};
    char err[512] = {0};
    if (!http_get_to_memory(url, &html, err, sizeof(err))) {
        printf("Watch page failed: %s\n", err);
        platform_http_cleanup();
        platform_cleanup();
        return 1;
    }
    printf("Watch page: %zu bytes\n", html.size);

    const char *api_url = "https://www.youtube.com/youtubei/v1/player?key=AIzaSyAO_FJ2SlqU8Q4STEHLGCilw_Y9_11qcW8&prettyPrint=false";
    const char *headers[] = {
        "Content-Type: application/json",
        "X-Goog-Api-Format-Version: 2"
    };
    const char *post_body = "{\"context\":{\"client\":{\"clientName\":\"ANDROID_VR\",\"clientVersion\":\"1.56.21\",\"deviceMake\":\"Oculus\",\"deviceModel\":\"Quest\",\"osName\":\"Android\",\"osVersion\":\"12\",\"hl\":\"en\",\"gl\":\"US\",\"visitorData\":\"test\"}},\"videoId\":\"hQkUiegDu1s\"}";

    printf("POST to youtubei API...\n");
    HttpBuffer response = {0};
    int status = 0;
    if (http_post_to_memory(api_url, post_body, strlen(post_body),
                            headers, 2, &response, &status, err, sizeof(err))) {
        printf("API Success! status=%d, size=%zu\n", status, response.size);
        http_free_buffer(&response);
    } else {
        printf("API Failed: %s\n", err);
    }

    http_free_buffer(&html);
    platform_http_cleanup();
    platform_cleanup();
    printf("Done.\n");
    return 0;
}

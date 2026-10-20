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

    const char *url = "https://www.youtube.com/youtubei/v1/player?key=AIzaSyAO_FJ2SlqU8Q4STEHLGCilw_Y9_11qcW8&prettyPrint=false";
    printf("GET %s\n", url);
    HttpBuffer buffer = {0};
    char err[512] = {0};
    if (http_get_to_memory(url, &buffer, err, sizeof(err))) {
        printf("Success! size=%zu\n", buffer.size);
    } else {
        printf("Failed: %s\n", err);
    }

    platform_http_cleanup();
    platform_cleanup();
    printf("Done.\n");
    return 0;
}

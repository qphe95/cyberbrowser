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

    const char *url = "https://www.youtube.com/watch?v=hQkUiegDu1s";
    char err[512] = {0};

    printf("GET 1...\n");
    HttpBuffer html1 = {0};
    if (!http_get_to_memory(url, &html1, err, sizeof(err))) {
        printf("GET 1 failed: %s\n", err);
        platform_http_cleanup();
        platform_cleanup();
        return 1;
    }
    printf("GET 1: %zu bytes\n", html1.size);
    http_free_buffer(&html1);

    printf("GET 2...\n");
    HttpBuffer html2 = {0};
    if (!http_get_to_memory(url, &html2, err, sizeof(err))) {
        printf("GET 2 failed: %s\n", err);
        platform_http_cleanup();
        platform_cleanup();
        return 1;
    }
    printf("GET 2: %zu bytes\n", html2.size);
    http_free_buffer(&html2);

    platform_http_cleanup();
    platform_cleanup();
    printf("Done.\n");
    return 0;
}

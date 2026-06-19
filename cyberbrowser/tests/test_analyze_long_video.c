/*
 * Test: Analyze the 7-hour video URL.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#endif

#include "platform.h"
#include "url_analyzer.h"
#include "http_download.h"

int main(int argc, char *argv[]) {
    const char *youtube_url = "https://www.youtube.com/watch?v=hQkUiegDu1s";
    if (argc > 1) youtube_url = argv[1];

    printf("=== Long Video Analysis Test ===\n");
    printf("URL: %s\n\n", youtube_url);

    if (!platform_init()) { return 1; }
    if (!platform_http_init()) { return 1; }

    char err[512] = {0};
    MediaUrl media = {0};

    printf("Analyzing URL...\n");
    if (!url_analyze(youtube_url, &media, err, sizeof(err))) {
        fprintf(stderr, "URL analysis failed: %s\n", err);
        platform_http_cleanup();
        platform_cleanup();
        return 1;
    }

    printf("Analysis successful!\n");
    printf("  Media URL: %.200s...\n", media.url);
    printf("  MIME type: %s\n", media.mime);
    printf("  Title: %s\n", media.title[0] ? media.title : "(none)");

    platform_http_cleanup();
    platform_cleanup();
    printf("\n=== Done ===\n");
    return 0;
}

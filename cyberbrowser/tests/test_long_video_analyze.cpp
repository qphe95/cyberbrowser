/*
 * Test: Analyze a very long YouTube video URL and report stream details.
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
#include "js_quickjs.h"
#include "browser_api_impl.h"
#include "quickjs.h"
#include "quickjs_gc_unified.h"

static bool init_engine(void) {
    if (!platform_init()) {
        fprintf(stderr, "Failed to initialize platform\n");
        return false;
    }
    if (!platform_http_init()) {
        fprintf(stderr, "Failed to initialize HTTP\n");
        return false;
    }
    if (!gc_init()) {
        fprintf(stderr, "Failed to initialize GC\n");
        return false;
    }
    if (!js_quickjs_init()) {
        fprintf(stderr, "Failed to initialize QuickJS\n");
        return false;
    }
    if (!js_quickjs_create_runtime()) {
        fprintf(stderr, "Failed to create QuickJS runtime\n");
        return false;
    }
    return true;
}

static void cleanup_engine(void) {
    js_quickjs_cleanup();
    platform_http_cleanup();
    platform_cleanup();
}

/* Try to get Content-Length without downloading the whole body */
static void probe_stream_size(const char *url) {
    char err[512] = {0};
    HttpBuffer buffer = {0};

    printf("Probing stream size (HEAD request not supported, doing partial GET)...\n");

    /* For googlevideo URLs, the Range header is already added by http_download.c.
       We'll do a full GET but only read a small amount to see headers. */
    if (!http_get_to_memory(url, &buffer, err, sizeof(err))) {
        printf("Probe failed: %s\n", err);
        return;
    }
    printf("Probe succeeded: %zu bytes total (possibly limited by Range request)\n", buffer.size);
    http_free_buffer(&buffer);
}

int main(int argc, char *argv[]) {
    const char *youtube_url = "https://www.youtube.com/watch?v=hQkUiegDu1s";
    if (argc > 1) {
        youtube_url = argv[1];
    }

    printf("=== Long Video Analysis Test ===\n");
    printf("URL: %s\n\n", youtube_url);

    if (!init_engine()) {
        return 1;
    }

    char err[512] = {0};
    MediaUrl media = {0};

    printf("Analyzing URL...\n");
    if (!url_analyze(youtube_url, &media, err, sizeof(err))) {
        fprintf(stderr, "URL analysis failed: %s\n", err);
        cleanup_engine();
        return 1;
    }

    printf("Analysis successful!\n");
    printf("  Media URL: %.200s...\n", media.url);
    printf("  MIME type: %s\n", media.mime);
    printf("  Title: %s\n", media.title[0] ? media.title : "(none)");

    probe_stream_size(media.url);

    cleanup_engine();
    printf("\n=== Done ===\n");
    return 0;
}

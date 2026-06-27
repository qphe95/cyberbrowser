/*
 * BGMDWNLD CLI - command-line background music downloader
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "platform.h"
#include "win32_compat.h"
#include "url_analyzer.h"
#include "http_download.h"
#include "js_quickjs.h"
#include "browser_api_impl.h"
#include "quickjs.h"
#include "quickjs_gc_unified.h"

#ifdef _WIN32
/* If double-clicked from Explorer, pause so the window stays open */
static void pause_if_double_clicked(void) {
    DWORD processes[2];
    DWORD count = GetConsoleProcessList(processes, 2);
    if (count <= 1) {
        printf("\nPress Enter to exit...");
        getchar();
    }
}
#else
static void pause_if_double_clicked(void) {}
#endif

static bool init_engine(void) {
    printf("Initializing platform...\n");
    if (!platform_init()) {
        fprintf(stderr, "Failed to initialize platform\n");
        return false;
    }
    printf("Initializing HTTP...\n");
    if (!platform_http_init()) {
        fprintf(stderr, "Failed to initialize HTTP\n");
        return false;
    }
    printf("Initializing GC...\n");
    if (!gc_init()) {
        fprintf(stderr, "Failed to initialize GC\n");
        return false;
    }
    printf("Initializing QuickJS...\n");
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

static bool g_download_done = false;
static bool g_download_success = false;

typedef struct {
    const char *url;
    const char *out_path;
    DownloadState *state;
    char *err;
    size_t errLen;
} DownloadThreadArgs;

static void* download_thread(void *arg) {
    DownloadThreadArgs *args = (DownloadThreadArgs *)arg;
    args->state->bytes_downloaded = 0;
    args->state->bytes_total = 0;
    args->state->state = 0;
    g_download_success = http_download_to_file(args->url, args->out_path, args->state, args->err, args->errLen);
    g_download_done = true;
    return NULL;
}

static void print_progress(DownloadState *state) {
    size_t downloaded = __atomic_load_n(&state->bytes_downloaded, __ATOMIC_RELAXED);
    size_t total = __atomic_load_n(&state->bytes_total, __ATOMIC_RELAXED);
    int st = __atomic_load_n(&state->state, __ATOMIC_RELAXED);

    if (total > 0) {
        double pct = (double)downloaded * 100.0 / (double)total;
        printf("\rProgress: %.1f%% (%zu / %zu bytes)", pct, downloaded, total);
    } else if (st == 1) {
        printf("\rGetting file size...");
    } else {
        printf("\rDownloading...");
    }
}

static void derive_mime_from_extension(const char *url, char *mime, size_t mime_len) {
    const char *dot = strrchr(url, '.');
    if (!dot) {
        strncpy(mime, "application/octet-stream", mime_len - 1);
        mime[mime_len - 1] = '\0';
        return;
    }
    if (strcasecmp(dot, ".mp4") == 0 || strcasecmp(dot, ".m4a") == 0) {
        strncpy(mime, "audio/mp4", mime_len - 1);
    } else if (strcasecmp(dot, ".webm") == 0) {
        strncpy(mime, "audio/webm", mime_len - 1);
    } else if (strcasecmp(dot, ".mp3") == 0) {
        strncpy(mime, "audio/mpeg", mime_len - 1);
    } else {
        strncpy(mime, "application/octet-stream", mime_len - 1);
    }
    mime[mime_len - 1] = '\0';
}

static bool has_media_extension(const char *url) {
    const char *dot = strrchr(url, '.');
    if (!dot) return false;
    return strcasecmp(dot, ".mp4") == 0 ||
           strcasecmp(dot, ".m4a") == 0 ||
           strcasecmp(dot, ".webm") == 0 ||
           strcasecmp(dot, ".mp3") == 0;
}

static void extract_filename(const char *url, char *out, size_t out_len) {
    MediaUrl media = {0};
    char err[512] = {0};
    if (url_analyze(url, &media, err, sizeof(err)) || has_media_extension(url)) {
        if (media.title[0]) {
            size_t title_len = strlen(media.title);
            if (title_len > 0) {
                if (title_len >= out_len) title_len = out_len - 1;
                memcpy(out, media.title, title_len);
                out[title_len] = '\0';
                for (size_t i = 0; i < title_len; i++) {
                    if (out[i] == '/' || out[i] == '\\' || out[i] == ':' ||
                        out[i] == '*' || out[i] == '?' || out[i] == '"' ||
                        out[i] == '<' || out[i] == '>' || out[i] == '|') {
                        out[i] = '_';
                    }
                }
                if (strstr(media.mime, "audio/mp4") || strstr(media.mime, "video/mp4")) {
                    strncat(out, ".m4a", out_len - strlen(out) - 1);
                } else if (strstr(media.mime, "audio/webm")) {
                    strncat(out, ".webm", out_len - strlen(out) - 1);
                } else {
                    strncat(out, ".audio", out_len - strlen(out) - 1);
                }
                return;
            }
        }
    }
    const char *last_slash = strrchr(url, '/');
    if (last_slash) {
        snprintf(out, out_len, "%s.m4a", last_slash + 1);
    } else {
        snprintf(out, out_len, "download.m4a");
    }
}

int main(int argc, char *argv[]) {
    setbuf(stdout, NULL);
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <youtube_url>\n", argv[0]);
        fprintf(stderr, "Example: %s https://www.youtube.com/watch?v=dQw4w9WgXcQ\n", argv[0]);
        pause_if_double_clicked();
        return 1;
    }

    const char *youtube_url = argv[1];

    if (!init_engine()) {
        pause_if_double_clicked();
        return 1;
    }

    char err[512] = {0};
    if (!platform_media_save_init("BGMDWLDR", err, sizeof(err))) {
        fprintf(stderr, "Failed to init media save: %s\n", err);
        cleanup_engine();
        pause_if_double_clicked();
        return 1;
    }
    printf("Save directory: %s\n", platform_media_save_get_path());

    printf("Analyzing URL: %s\n", youtube_url);
    MediaUrl media = {0};
    printf("Calling url_analyze...\n");
    if (!url_analyze(youtube_url, &media, err, sizeof(err))) {
        if (has_media_extension(youtube_url)) {
            strncpy(media.url, youtube_url, sizeof(media.url) - 1);
            media.url[sizeof(media.url) - 1] = '\0';
            derive_mime_from_extension(youtube_url, media.mime, sizeof(media.mime));
            printf("URL analysis skipped; using direct media URL: %s\n", media.url);
        } else {
            fprintf(stderr, "URL analysis failed: %s\n", err);
            cleanup_engine();
            pause_if_double_clicked();
            return 1;
        }
    }
    printf("Media URL: %s\n", media.url);

    char filename[256] = {0};
    if (media.title[0]) {
        size_t title_len = strlen(media.title);
        if (title_len >= sizeof(filename)) title_len = sizeof(filename) - 1;
        memcpy(filename, media.title, title_len);
        filename[title_len] = '\0';
        for (size_t i = 0; i < title_len; i++) {
            if (filename[i] == '/' || filename[i] == '\\' || filename[i] == ':' ||
                filename[i] == '*' || filename[i] == '?' || filename[i] == '"' ||
                filename[i] == '<' || filename[i] == '>' || filename[i] == '|') {
                filename[i] = '_';
            }
        }
        if (strstr(media.mime, "audio/mp4") || strstr(media.mime, "video/mp4")) {
            strncat(filename, ".m4a", sizeof(filename) - strlen(filename) - 1);
        } else if (strstr(media.mime, "audio/webm")) {
            strncat(filename, ".webm", sizeof(filename) - strlen(filename) - 1);
        } else {
            strncat(filename, ".audio", sizeof(filename) - strlen(filename) - 1);
        }
    } else {
        const char *last_slash = strrchr(media.url, '/');
        if (last_slash) {
            snprintf(filename, sizeof(filename), "%s.m4a", last_slash + 1);
        } else {
            snprintf(filename, sizeof(filename), "download.m4a");
        }
    }

    char out_path[512];
    snprintf(out_path, sizeof(out_path), "%s/%s", platform_media_save_get_path(), filename);
    printf("Downloading to: %s\n", out_path);

    DownloadState ds = {0};
    download_state_init(&ds);

    DownloadThreadArgs dt_args = {
        .url = media.url,
        .out_path = out_path,
        .state = &ds,
        .err = err,
        .errLen = sizeof(err)
    };

    pthread_t dl_thread;
    pthread_create(&dl_thread, (pthread_attr_t*)NULL, download_thread, &dt_args);

    while (!g_download_done) {
        print_progress(&ds);
#ifdef _WIN32
        Sleep(300);
#else
        usleep(300000);
#endif
    }
    print_progress(&ds);
    printf("\n");

    pthread_join(dl_thread, NULL);

    if (!g_download_success) {
        fprintf(stderr, "Download failed: %s\n", err);
        cleanup_engine();
        pause_if_double_clicked();
        return 1;
    }
    printf("Download complete: %s\n", out_path);

    cleanup_engine();
    pause_if_double_clicked();
    return 0;
}

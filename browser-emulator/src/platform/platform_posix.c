/*
 * Platform Abstraction Layer - POSIX Implementation (macOS/Linux)
 */

#include "platform.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include <pthread.h>
#include <errno.h>

/* ============================================================================
 * Logging
 * ============================================================================ */

extern LogLevel platform_get_log_level(void);

void platform_vlog(LogLevel level, const char *tag, const char *fmt, va_list args) {
    if (level < platform_get_log_level()) {
        return;
    }
    
    const char *level_str = "?";
    switch (level) {
        case LOG_LEVEL_DEBUG: level_str = "D"; break;
        case LOG_LEVEL_INFO:  level_str = "I"; break;
        case LOG_LEVEL_WARN:  level_str = "W"; break;
        case LOG_LEVEL_ERROR: level_str = "E"; break;
    }
    
    fprintf(stderr, "[%s/%s] ", level_str, tag);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
}

void platform_log(LogLevel level, const char *tag, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    platform_vlog(level, tag, fmt, args);
    va_end(args);
}

/* ============================================================================
 * Threading/Mutex
 * ============================================================================ */

struct PlatformMutex {
    pthread_mutex_t mutex;
};

PlatformMutex* platform_mutex_create(void) {
    PlatformMutex *mutex = malloc(sizeof(PlatformMutex));
    if (!mutex) return NULL;
    
    if (pthread_mutex_init(&mutex->mutex, NULL) != 0) {
        free(mutex);
        return NULL;
    }
    
    return mutex;
}

void platform_mutex_destroy(PlatformMutex *mutex) {
    if (mutex) {
        pthread_mutex_destroy(&mutex->mutex);
        free(mutex);
    }
}

void platform_mutex_lock(PlatformMutex *mutex) {
    if (mutex) {
        pthread_mutex_lock(&mutex->mutex);
    }
}

void platform_mutex_unlock(PlatformMutex *mutex) {
    if (mutex) {
        pthread_mutex_unlock(&mutex->mutex);
    }
}

/* ============================================================================
 * Time
 * ============================================================================ */

unsigned long long platform_get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (unsigned long long)tv.tv_sec * 1000ULL + (unsigned long long)tv.tv_usec / 1000ULL;
}

void platform_sleep_ms(unsigned int ms) {
    usleep(ms * 1000);
}

/* ============================================================================
 * File System
 * ============================================================================ */

struct PlatformFile {
    FILE *fp;
};

PlatformFile* platform_file_open_write(const char *path) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return NULL;
    
    PlatformFile *file = malloc(sizeof(PlatformFile));
    if (!file) {
        fclose(fp);
        return NULL;
    }
    
    file->fp = fp;
    return file;
}

PlatformFile* platform_file_open_append(const char *path) {
    FILE *fp = fopen(path, "ab");
    if (!fp) return NULL;
    
    PlatformFile *file = malloc(sizeof(PlatformFile));
    if (!file) {
        fclose(fp);
        return NULL;
    }
    
    file->fp = fp;
    return file;
}

size_t platform_file_write(PlatformFile *file, const void *data, size_t size) {
    if (!file || !file->fp) return 0;
    return fwrite(data, 1, size, file->fp);
}

void platform_file_flush(PlatformFile *file) {
    if (file && file->fp) {
        fflush(file->fp);
    }
}

void platform_file_close(PlatformFile *file) {
    if (file) {
        if (file->fp) {
            fclose(file->fp);
        }
        free(file);
    }
}

char* platform_file_read_all(const char *path, size_t *outSize) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    if (size < 0) {
        fclose(fp);
        return NULL;
    }
    
    char *buffer = malloc(size + 1);
    if (!buffer) {
        fclose(fp);
        return NULL;
    }
    
    size_t read = fread(buffer, 1, size, fp);
    fclose(fp);
    
    buffer[read] = '\0';
    
    if (outSize) {
        *outSize = read;
    }
    
    return buffer;
}

bool platform_file_exists(const char *path) {
    return access(path, F_OK) == 0;
}

const char* platform_get_temp_dir(void) {
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir) {
        tmpdir = getenv("TMP");
    }
    if (!tmpdir) {
        tmpdir = getenv("TEMP");
    }
    if (!tmpdir) {
        tmpdir = "/tmp";
    }
    return tmpdir;
}

/* ============================================================================
 * Asset Loading
 * ============================================================================ */

char* platform_asset_read(const char *assetName, size_t *outSize) {
    /* On macOS/Linux, assets are loaded from a data directory relative to executable */
    char path[512];
    const char *asset_dir = getenv("BROWSER_EMULATOR_ASSETS");
    if (!asset_dir) {
        asset_dir = "./assets";
    }
    
    snprintf(path, sizeof(path), "%s/%s", asset_dir, assetName);
    return platform_file_read_all(path, outSize);
}

/* ============================================================================
 * Media Saving
 * ============================================================================ */

#include <sys/stat.h>

static char g_media_save_path[512] = {0};

bool platform_media_save_init(const char *app_name, char *err, size_t err_len) {
    if (!app_name || !app_name[0]) {
        if (err && err_len > 0) snprintf(err, err_len, "app_name is null");
        return false;
    }
    
    const char *home = getenv("HOME");
    if (!home) {
        home = getenv("USERPROFILE"); /* Windows fallback */
    }
    if (!home) {
        if (err && err_len > 0) snprintf(err, err_len, "HOME not set");
        return false;
    }
    
    /* Build path: ~/Music/<app_name>/ */
    int n = snprintf(g_media_save_path, sizeof(g_media_save_path),
                     "%s/Music/%s", home, app_name);
    if (n < 0 || (size_t)n >= sizeof(g_media_save_path)) {
        if (err && err_len > 0) snprintf(err, err_len, "path too long");
        return false;
    }
    
    /* Create directory tree */
    char path_build[512];
    strncpy(path_build, g_media_save_path, sizeof(path_build) - 1);
    path_build[sizeof(path_build) - 1] = '\0';
    
    for (char *p = path_build + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(path_build, 0755);
            *p = '/';
        }
    }
    mkdir(path_build, 0755);
    
    return true;
}

bool platform_media_save_init_video(const char *app_name, char *err, size_t err_len) {
    if (!app_name || !app_name[0]) {
        if (err && err_len > 0) snprintf(err, err_len, "app_name is null");
        return false;
    }

    const char *home = getenv("HOME");
    if (!home) {
        home = getenv("USERPROFILE"); /* Windows fallback */
    }
    if (!home) {
        if (err && err_len > 0) snprintf(err, err_len, "HOME not set");
        return false;
    }

    /* Build path: ~/Movies/<app_name>/ */
    int n = snprintf(g_media_save_path, sizeof(g_media_save_path),
                     "%s/Movies/%s", home, app_name);
    if (n < 0 || (size_t)n >= sizeof(g_media_save_path)) {
        if (err && err_len > 0) snprintf(err, err_len, "path too long");
        return false;
    }

    /* Create directory tree */
    char path_build[512];
    strncpy(path_build, g_media_save_path, sizeof(path_build) - 1);
    path_build[sizeof(path_build) - 1] = '\0';

    for (char *p = path_build + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(path_build, 0755);
            *p = '/';
        }
    }
    mkdir(path_build, 0755);

    return true;
}

bool platform_media_save_audio(const char *data, size_t size,
                               const char *filename, const char *mime_type,
                               char *err, size_t err_len) {
    (void)mime_type;
    
    if (!data || size == 0 || !filename || !filename[0]) {
        if (err && err_len > 0) snprintf(err, err_len, "invalid args");
        return false;
    }
    
    if (g_media_save_path[0] == '\0') {
        if (err && err_len > 0) snprintf(err, err_len, "media save not initialized");
        return false;
    }
    
    char full_path[768];
    int n = snprintf(full_path, sizeof(full_path), "%s/%s", g_media_save_path, filename);
    if (n < 0 || (size_t)n >= sizeof(full_path)) {
        if (err && err_len > 0) snprintf(err, err_len, "path too long");
        return false;
    }
    
    PlatformFile *file = platform_file_open_write(full_path);
    if (!file) {
        if (err && err_len > 0) snprintf(err, err_len, "failed to open %s", full_path);
        return false;
    }
    
    size_t written = platform_file_write(file, data, size);
    platform_file_flush(file);
    platform_file_close(file);
    
    if (written != size) {
        if (err && err_len > 0) snprintf(err, err_len, "write incomplete: %zu/%zu", written, size);
        return false;
    }
    
    return true;
}

const char* platform_media_save_get_path(void) {
    return g_media_save_path[0] ? g_media_save_path : "";
}

void platform_media_save_cleanup(void) {
    g_media_save_path[0] = '\0';
}

/* ============================================================================
 * Initialization
 * ============================================================================ */

bool platform_init(void) {
    return true;
}

void platform_cleanup(void) {
    /* Nothing to cleanup for POSIX */
}

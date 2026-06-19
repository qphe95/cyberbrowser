/*
 * Platform Abstraction Layer - Android Implementation
 * 
 * This file provides Android-specific implementations of the platform abstraction.
 * It bridges the browser-emulator module to Android's native APIs.
 */

#include "platform.h"

#include <android/log.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <jni.h>

/* ============================================================================
 * Logging
 * ============================================================================ */

extern LogLevel platform_get_log_level(void);

static int log_level_to_android(LogLevel level) {
    switch (level) {
        case LOG_LEVEL_DEBUG: return ANDROID_LOG_DEBUG;
        case LOG_LEVEL_INFO:  return ANDROID_LOG_INFO;
        case LOG_LEVEL_WARN:  return ANDROID_LOG_WARN;
        case LOG_LEVEL_ERROR: return ANDROID_LOG_ERROR;
        default: return ANDROID_LOG_INFO;
    }
}

void platform_vlog(LogLevel level, const char *tag, const char *fmt, va_list args) {
    if (level < platform_get_log_level()) {
        return;
    }
    
    int android_level = log_level_to_android(level);
    __android_log_vprint(android_level, tag, fmt, args);
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

#include <pthread.h>

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

#include <sys/time.h>

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
    int fd;
};

PlatformFile* platform_file_open_write(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return NULL;
    
    PlatformFile *file = malloc(sizeof(PlatformFile));
    if (!file) {
        close(fd);
        return NULL;
    }
    
    file->fd = fd;
    return file;
}

PlatformFile* platform_file_open_append(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return NULL;
    
    PlatformFile *file = malloc(sizeof(PlatformFile));
    if (!file) {
        close(fd);
        return NULL;
    }
    
    file->fd = fd;
    return file;
}

size_t platform_file_write(PlatformFile *file, const void *data, size_t size) {
    if (!file || file->fd < 0) return 0;
    return write(file->fd, data, size);
}

void platform_file_flush(PlatformFile *file) {
    if (file && file->fd >= 0) {
        fsync(file->fd);
    }
}

void platform_file_close(PlatformFile *file) {
    if (file) {
        if (file->fd >= 0) {
            close(file->fd);
        }
        free(file);
    }
}

char* platform_file_read_all(const char *path, size_t *outSize) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    
    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return NULL;
    }
    
    char *buffer = malloc(st.st_size + 1);
    if (!buffer) {
        close(fd);
        return NULL;
    }
    
    ssize_t read_bytes = read(fd, buffer, st.st_size);
    close(fd);
    
    if (read_bytes < 0) {
        free(buffer);
        return NULL;
    }
    
    buffer[read_bytes] = '\0';
    
    if (outSize) {
        *outSize = read_bytes;
    }
    
    return buffer;
}

bool platform_file_exists(const char *path) {
    return access(path, F_OK) == 0;
}

const char* platform_get_temp_dir(void) {
    return "/data/local/tmp";
}

/* ============================================================================
 * Asset Loading
 * ============================================================================ */

#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>

static AAssetManager *g_asset_manager = NULL;

void platform_android_set_asset_manager(AAssetManager *mgr) {
    g_asset_manager = mgr;
}

char* platform_asset_read(const char *assetName, size_t *outSize) {
    if (!g_asset_manager) {
        return NULL;
    }
    
    AAsset *asset = AAssetManager_open(g_asset_manager, assetName, AASSET_MODE_BUFFER);
    if (!asset) {
        return NULL;
    }
    
    off_t size = AAsset_getLength(asset);
    char *buffer = malloc(size + 1);
    if (!buffer) {
        AAsset_close(asset);
        return NULL;
    }
    
    int read_bytes = AAsset_read(asset, buffer, size);
    AAsset_close(asset);
    
    if (read_bytes < 0) {
        free(buffer);
        return NULL;
    }
    
    buffer[read_bytes] = '\0';
    
    if (outSize) {
        *outSize = read_bytes;
    }
    
    return buffer;
}

/* ============================================================================
 * Media Saving
 * ============================================================================ */

static char g_media_save_path[512] = {0};

bool platform_media_save_init(const char *app_name, char *err, size_t err_len) {
    (void)app_name;
    /* On Android, use media_store.c directly for MediaStore integration.
     * This stub allows the platform API to compile but returns false.
     * A full implementation would wrap MediaStore via JNI here. */
    if (err && err_len > 0) {
        snprintf(err, err_len, "Android: use media_store.c directly");
    }
    snprintf(g_media_save_path, sizeof(g_media_save_path),
             "Music/%s", app_name ? app_name : "BGMDWLDR");
    return true;
}

bool platform_media_save_init_video(const char *app_name, char *err, size_t err_len) {
    (void)app_name;
    /* On Android, use media_store.c directly for MediaStore integration. */
    if (err && err_len > 0) {
        snprintf(err, err_len, "Android: use media_store.c directly");
    }
    snprintf(g_media_save_path, sizeof(g_media_save_path),
             "Movies/%s", app_name ? app_name : "BGMDWLDR");
    return true;
}

bool platform_media_save_audio(const char *data, size_t size,
                               const char *filename, const char *mime_type,
                               char *err, size_t err_len) {
    (void)data;
    (void)size;
    (void)filename;
    (void)mime_type;
    if (err && err_len > 0) {
        snprintf(err, err_len, "Android: use media_store.c directly");
    }
    return false;
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
    /* Nothing to cleanup */
}

#ifndef UTF8_FILENAME_H
#define UTF8_FILENAME_H

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Cross-platform helpers for opening/removing files using UTF-8 paths.
 * On Windows these convert UTF-8 -> UTF-16 and call the wide APIs.
 * On macOS/Linux/Android they are simple passthroughs to fopen/remove/access. */

static inline FILE *fopen_utf8(const char *path, const char *mode) {
#ifdef _WIN32
    wchar_t wpath[1024];
    wchar_t wmode[32];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 1024) == 0)
        return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode, 32) == 0)
        return NULL;
    return _wfopen(wpath, wmode);
#else
    return fopen(path, mode);
#endif
}

static inline int remove_utf8(const char *path) {
#ifdef _WIN32
    wchar_t wpath[1024];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 1024) == 0)
        return -1;
    return _wremove(wpath);
#else
    return remove(path);
#endif
}

static inline int access_utf8(const char *path, int mode) {
#ifdef _WIN32
    wchar_t wpath[1024];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 1024) == 0)
        return -1;
    return _waccess(wpath, mode);
#else
    return access(path, mode);
#endif
}

static inline bool file_exists_utf8(const char *path) {
#ifdef _WIN32
    wchar_t wpath[1024];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 1024) == 0)
        return false;
    DWORD attr = GetFileAttributesW(wpath);
    return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
#else
    return access(path, F_OK) == 0;
#endif
}

#ifdef __cplusplus
}
#endif

#endif /* UTF8_FILENAME_H */

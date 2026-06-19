#ifndef WIN32_DIRENT_H
#define WIN32_DIRENT_H

/* Minimal dirent compatibility for Windows test builds */

#include <winsock2.h>
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct dirent {
    char d_name[MAX_PATH];
};

typedef struct {
    HANDLE handle;
    WIN32_FIND_DATAA data;
    struct dirent entry;
    int first;
} DIR;

static inline DIR *opendir(const char *name) {
    DIR *dir = (DIR *)malloc(sizeof(DIR));
    if (!dir) return NULL;
    char path[MAX_PATH + 4];
    snprintf(path, sizeof(path), "%s\\*", name);
    dir->handle = FindFirstFileA(path, &dir->data);
    if (dir->handle == INVALID_HANDLE_VALUE) {
        free(dir);
        return NULL;
    }
    dir->first = 1;
    return dir;
}

static inline struct dirent *readdir(DIR *dir) {
    if (!dir) return NULL;
    if (dir->first) {
        dir->first = 0;
    } else {
        if (!FindNextFileA(dir->handle, &dir->data)) {
            return NULL;
        }
    }
    strncpy(dir->entry.d_name, dir->data.cFileName, MAX_PATH);
    dir->entry.d_name[MAX_PATH - 1] = '\0';
    return &dir->entry;
}

static inline int closedir(DIR *dir) {
    if (!dir) return -1;
    FindClose(dir->handle);
    free(dir);
    return 0;
}

#endif /* WIN32_DIRENT_H */

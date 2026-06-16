#ifndef URL_ANALYZER_H
#define URL_ANALYZER_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEDIA_URL_MAX 2048
#define MEDIA_MIME_MAX 64

typedef struct MediaUrl {
    char url[MEDIA_URL_MAX];
    char mime[MEDIA_MIME_MAX];
    char title[256];
    char thumbnailUrl[2048];
} MediaUrl;

bool url_analyze(const char *inputUrl, MediaUrl *outMedia, char *err, size_t errLen);
bool url_analyze_with_options(const char *inputUrl, MediaUrl *outMedia, char *err, size_t errLen, bool preferVideo);

#ifdef __cplusplus
}
#endif

#endif

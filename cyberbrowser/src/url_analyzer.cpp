#include "url_analyzer.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static void set_err(char *err, size_t errLen, const char *msg) {
    if (err && errLen > 0) {
        snprintf(err, errLen, "%s", msg);
    }
}

static bool has_media_extension(const char *url) {
    const char *ext = strrchr(url, '.');
    if (!ext) {
        return false;
    }
    return strcmp(ext, ".mp4") == 0 || strcmp(ext, ".webm") == 0 ||
           strcmp(ext, ".m3u8") == 0 || strcmp(ext, ".m4a") == 0 ||
           strcmp(ext, ".mp3") == 0;
}

static void derive_mime_from_extension(const char *url, char *mime, size_t mime_len) {
    const char *ext = strrchr(url, '.');
    if (!ext) {
        strncpy(mime, "application/octet-stream", mime_len - 1);
    } else if (strcmp(ext, ".mp4") == 0) {
        strncpy(mime, "video/mp4", mime_len - 1);
    } else if (strcmp(ext, ".webm") == 0) {
        strncpy(mime, "video/webm", mime_len - 1);
    } else if (strcmp(ext, ".m3u8") == 0) {
        strncpy(mime, "application/vnd.apple.mpegurl", mime_len - 1);
    } else if (strcmp(ext, ".m4a") == 0) {
        strncpy(mime, "audio/mp4", mime_len - 1);
    } else if (strcmp(ext, ".mp3") == 0) {
        strncpy(mime, "audio/mpeg", mime_len - 1);
    } else {
        strncpy(mime, "application/octet-stream", mime_len - 1);
    }
    mime[mime_len - 1] = '\0';
}

bool url_analyze_with_options(const char *inputUrl, MediaUrl *outMedia,
                              char *err, size_t errLen, bool prefer_video) {
    (void)prefer_video;
    if (!inputUrl || !outMedia) {
        set_err(err, errLen, "Invalid arguments");
        return false;
    }

    memset(outMedia, 0, sizeof(MediaUrl));

    if (has_media_extension(inputUrl)) {
        strncpy(outMedia->url, inputUrl, sizeof(outMedia->url) - 1);
        outMedia->url[sizeof(outMedia->url) - 1] = '\0';
        derive_mime_from_extension(inputUrl, outMedia->mime, sizeof(outMedia->mime));
        return true;
    }

    set_err(err, errLen, "No supported media URL found");
    return false;
}

bool url_analyze(const char *inputUrl, MediaUrl *outMedia, char *err, size_t errLen) {
    return url_analyze_with_options(inputUrl, outMedia, err, errLen, false);
}

#include "url_analyzer.h"
#include "html_media_extract.h"
#include "http_download.h"
#include "session_state.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <ctype.h>
#include "platform.h"

#define LOG_TAG "url_analyzer"
#define LOGI(...) platform_log(LOG_LEVEL_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) platform_log(LOG_LEVEL_ERROR, LOG_TAG, __VA_ARGS__)

/* Logging wrapper */
static void file_log(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    platform_vlog(LOG_LEVEL_DEBUG, LOG_TAG, fmt, args);
    va_end(args);
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

static void set_err(char *err, size_t errLen, const char *msg) {
    if (err && errLen > 0) {
        snprintf(err, errLen, "%s", msg);
    }
}

static void resolve_url(const char *baseUrl, const char *candidate,
                        char *out, size_t outLen) {
    if (strstr(candidate, "://")) {
        snprintf(out, outLen, "%s", candidate);
        return;
    }
    const char *schemeEnd = strstr(baseUrl, "://");
    if (!schemeEnd) {
        snprintf(out, outLen, "%s", candidate);
        return;
    }
    size_t schemeLen = (size_t)(schemeEnd - baseUrl);
    const char *hostStart = schemeEnd + 3;
    const char *pathStart = strchr(hostStart, '/');
    size_t hostLen = pathStart ? (size_t)(pathStart - hostStart) : strlen(hostStart);
    if (candidate[0] == '/' && candidate[1] == '/') {
        snprintf(out, outLen, "%.*s:%s", (int)schemeLen, baseUrl, candidate);
        return;
    }
    if (candidate[0] == '/') {
        snprintf(out, outLen, "%.*s://%.*s%s",
                 (int)schemeLen, baseUrl,
                 (int)hostLen, hostStart, candidate);
        return;
    }
    snprintf(out, outLen, "%.*s://%.*s/%s",
             (int)schemeLen, baseUrl,
             (int)hostLen, hostStart, candidate);
}

/* Extract video ID from YouTube URL */
static bool extract_video_id(const char *url, char *out_id, size_t out_len) {
    const char *patterns[] = {"?v=", "&v=", "/v/", "/embed/", ".be/", NULL};
    for (int i = 0; patterns[i]; i++) {
        const char *p = strstr(url, patterns[i]);
        if (p) {
            p += strlen(patterns[i]);
            size_t len = 0;
            while (p[len] && (isalnum((unsigned char)p[len]) || p[len] == '_' || p[len] == '-')) {
                len++;
            }
            if (len >= 11 && len < out_len) {
                memcpy(out_id, p, len);
                out_id[len] = '\0';
                return true;
            }
        }
    }
    return false;
}

/* Simple JSON scanner: find a "url":"..." value that appears after a given itag.
 * We look for "itag":<target> then scan forward to the next "url":"..." inside
 * the same object (before the next closing brace).
 */
static bool find_url_for_itag(const char *json, int target_itag, char *out_url, size_t out_len) {
    char itag_pattern[32];
    snprintf(itag_pattern, sizeof(itag_pattern), "\"itag\":%d", target_itag);

    const char *scan = json;
    while ((scan = strstr(scan, itag_pattern)) != NULL) {
        scan += strlen(itag_pattern);
        /* Look for "url":" within the next object boundary */
        const char *brace_end = strchr(scan, '}');
        const char *url_key = strstr(scan, "\"url\":\"");
        if (url_key && (!brace_end || url_key < brace_end)) {
            url_key += 7; /* skip "url":" */
            const char *url_end = strchr(url_key, '"');
            if (url_end) {
                size_t len = (size_t)(url_end - url_key);
                if (len > 0 && len < out_len) {
                    memcpy(out_url, url_key, len);
                    out_url[len] = '\0';
                    return true;
                }
            }
        }
    }
    return false;
}

/* Fallback: scan any "url":"https://...googlevideo.com..." in JSON */
static bool find_any_googlevideo_url(const char *json, char *out_url, size_t out_len) {
    const char *scan = json;
    while ((scan = strstr(scan, "\"url\":\"")) != NULL) {
        scan += 7;
        const char *end = strchr(scan, '"');
        if (end) {
            size_t len = (size_t)(end - scan);
            if (len > 0 && len < out_len && strstr(scan, "googlevideo.com")) {
                /* Prefer URLs with itag=18 (combined audio+video) or itag=140 (audio) */
                if (strstr(scan, "itag=18") || strstr(scan, "itag=140")) {
                    memcpy(out_url, scan, len);
                    out_url[len] = '\0';
                    return true;
                }
            }
            scan = end + 1;
        } else {
            break;
        }
    }
    /* Second pass: accept any googlevideo URL */
    scan = json;
    while ((scan = strstr(scan, "\"url\":\"")) != NULL) {
        scan += 7;
        const char *end = strchr(scan, '"');
        if (end) {
            size_t len = (size_t)(end - scan);
            if (len > 0 && len < out_len && strstr(scan, "googlevideo.com")) {
                memcpy(out_url, scan, len);
                out_url[len] = '\0';
                return true;
            }
            scan = end + 1;
        } else {
            break;
        }
    }
    return false;
}

/* Extract visitorData from youtubei JSON response */
static bool extract_visitor_data(const char *json, char *out_vd, size_t out_len) {
    const char *key = "\"visitorData\":\"";
    const char *p = strstr(json, key);
    if (!p) return false;
    p += strlen(key);
    const char *end = strchr(p, '"');
    if (!end) return false;
    size_t len = (size_t)(end - p);
    if (len == 0 || len >= out_len) return false;
    memcpy(out_vd, p, len);
    out_vd[len] = '\0';
    return true;
}

/* Fallback: extract visitorData from YouTube HTML page via string scanning.
 * Used only when the JS execution path fails. */
static bool extract_visitor_data_from_html_fallback(const char *html, size_t html_len, char *out_vd, size_t out_len) {
    const char *p = html;
    const char *end = html + html_len;
    while (p < end) {
        const char *key = "\"VISITOR_DATA\":\"";
        size_t key_len = strlen(key);
        if (p + key_len < end && memcmp(p, key, key_len) == 0) {
            p += key_len;
            size_t i = 0;
            while (p + i < end && p[i] != '"' && i < out_len - 1) {
                out_vd[i] = p[i];
                i++;
            }
            out_vd[i] = '\0';
            return i > 0;
        }
        p++;
    }
    /* Also try lowercase pattern */
    p = html;
    while (p < end) {
        const char *key = "visitorData\":\"";
        size_t key_len = strlen(key);
        if (p + key_len < end && memcmp(p, key, key_len) == 0) {
            p += key_len;
            size_t i = 0;
            while (p + i < end && p[i] != '"' && i < out_len - 1) {
                out_vd[i] = p[i];
                i++;
            }
            out_vd[i] = '\0';
            return i > 0;
        }
        p++;
    }
    /* Try cookie format */
    p = html;
    while (p < end) {
        const char *key = "VISITOR_INFO1_LIVE=";
        size_t key_len = strlen(key);
        if (p + key_len < end && memcmp(p, key, key_len) == 0) {
            p += key_len;
            size_t i = 0;
            while (p + i < end && p[i] != ';' && p[i] != '"' && i < out_len - 1) {
                out_vd[i] = p[i];
                i++;
            }
            out_vd[i] = '\0';
            return i > 0;
        }
        p++;
    }
    return false;
}

/* Fetch YouTube watch page and extract visitorData via true browser emulation:
 * 1. Load the watch page HTML
 * 2. Extract inline <script> tags and execute them in QuickJS
 * 3. Read ytcfg.get('VISITOR_DATA') from the live JS objects
 * 
 * The actual JS execution is done in html_media_extract.cpp (C++) since
 * QuickJS headers require C++ compilation. This function orchestrates the
 * page fetch and delegates script execution.
 */
static bool fetch_visitor_data_from_watch_page(const char *video_id, char *out_vd, size_t out_len,
                                                char *err, size_t errLen) {
    char url[256];
    snprintf(url, sizeof(url), "https://www.youtube.com/watch?v=%s", video_id);
    LOGI("Fetching watch page to establish session: %s", url);
    file_log("Fetching watch page to establish session: %s", url);

    HttpBuffer html = {0};
    if (!http_get_to_memory(url, &html, err, errLen)) {
        LOGE("Failed to fetch watch page: %s", err);
        file_log("Failed to fetch watch page: %s", err);
        return false;
    }

    LOGI("Watch page response: %zu bytes", html.size);
    file_log("Watch page response: %zu bytes", html.size);

    bool found = false;

    /* Primary path: true browser emulation via JS execution */
    found = html_extract_visitor_data(html.data, out_vd, out_len);
    if (found) {
        LOGI("Extracted visitorData via JS execution: %.80s...", out_vd);
        file_log("Extracted visitorData via JS execution");
    } else {
        file_log("JS execution did not yield visitorData, falling back to string scan");
    }

    /* Fallback: string scanning if JS path failed or unavailable */
    if (!found) {
        found = extract_visitor_data_from_html_fallback(html.data, html.size, out_vd, out_len);
        if (found) {
            LOGI("Extracted visitorData via string scan (fallback): %.80s...", out_vd);
            file_log("Extracted visitorData via string scan (fallback)");
        }
    }

    http_free_buffer(&html);

    if (!found) {
        LOGE("visitorData not found in watch page");
        file_log("visitorData not found in watch page");
        snprintf(err, errLen, "visitorData not found in watch page");
    }
    return found;
}

/* Check if playabilityStatus indicates LOGIN_REQUIRED */
static bool is_login_required(const char *json) {
    const char *p = strstr(json, "\"playabilityStatus\"");
    if (!p) return false;
    const char *status = strstr(p, "\"status\":\"");
    if (!status) return false;
    status += 10; /* skip "status":" */
    size_t len = 0;
    while (status[len] && status[len] != '"') len++;
    return (len == 14 && strncasecmp(status, "LOGIN_REQUIRED", 14) == 0);
}

/* Try to get a direct media URL via the YouTube youtubei/v1/player API
 * using the ANDROID_VR client with visitorData from a prior page load.
 */
static bool try_youtubei_api(const char *video_id, const char *visitor_data,
                              MediaUrl *outMedia, char *err, size_t errLen,
                              bool prefer_video) {
    const char *api_url = "https://www.youtube.com/youtubei/v1/player?key=AIzaSyAO_FJ2SlqU8Q4STEHLGCilw_Y9_11qcW8&prettyPrint=false";

    const char *effective_visitor = (visitor_data && visitor_data[0]) ? visitor_data : g_visitor_data;

    char client_version_header[128] = {0};
    char visitor_id_header[256] = {0};
    const char *headers[5] = {
        "Content-Type: application/json",
        "X-Goog-Api-Format-Version: 2",
        "X-YouTube-Client-Name: 1",
        NULL,
        NULL
    };
    size_t header_count = 3;
    snprintf(client_version_header, sizeof(client_version_header),
             "X-YouTube-Client-Version: 1.56.21");
    headers[header_count++] = client_version_header;
    if (effective_visitor && effective_visitor[0]) {
        snprintf(visitor_id_header, sizeof(visitor_id_header),
                 "X-Goog-Visitor-Id: %s", effective_visitor);
        headers[header_count++] = visitor_id_header;
    }

    char post_body[2048];
    if (effective_visitor && effective_visitor[0]) {
        snprintf(post_body, sizeof(post_body),
            "{"
            "\"context\":{"
            "\"client\":{"
            "\"clientName\":\"ANDROID_VR\","
            "\"clientVersion\":\"1.56.21\","
            "\"deviceMake\":\"Oculus\","
            "\"deviceModel\":\"Quest\","
            "\"osName\":\"Android\","
            "\"osVersion\":\"12\","
            "\"hl\":\"en\","
            "\"gl\":\"US\","
            "\"visitorData\":\"%s\""
            "}"
            "},"
            "\"videoId\":\"%s\""
            "}", effective_visitor, video_id);
    } else {
        snprintf(post_body, sizeof(post_body),
            "{"
            "\"context\":{"
            "\"client\":{"
            "\"clientName\":\"ANDROID_VR\","
            "\"clientVersion\":\"1.56.21\","
            "\"deviceMake\":\"Oculus\","
            "\"deviceModel\":\"Quest\","
            "\"osName\":\"Android\","
            "\"osVersion\":\"12\","
            "\"hl\":\"en\","
            "\"gl\":\"US\""
            "}"
            "},"
            "\"videoId\":\"%s\""
            "}", video_id);
    }

    LOGI("Trying youtubei API for videoId=%s", video_id);
    file_log("Trying youtubei API for videoId=%s", video_id);

    HttpBuffer response = {0};
    int status = 0;
    if (!http_post_to_memory(api_url, post_body, strlen(post_body),
                             headers, header_count, &response, &status, err, errLen)) {
        LOGE("youtubei API request failed: %s", err);
        file_log("youtubei API request failed: %s", err);
        return false;
    }

    if (status != 200) {
        snprintf(err, errLen, "youtubei API returned HTTP %d", status);
        LOGE("%s", err);
        file_log("%s", err);
        http_free_buffer(&response);
        return false;
    }

    LOGI("youtubei API returned %zu bytes", response.size);
    file_log("youtubei API returned %zu bytes", response.size);

    /* Ensure null-terminated for string scanning */
    char *json = (char*)malloc(response.size + 1);
    if (!json) {
        snprintf(err, errLen, "Out of memory");
        http_free_buffer(&response);
        return false;
    }
    memcpy(json, response.data, response.size);
    json[response.size] = '\0';
    http_free_buffer(&response);

    char found_url[2048] = {0};
    bool is_audio = false;
    if (prefer_video) {
        /* Prefer highest quality combined audio+video stream */
        if (find_url_for_itag(json, 22, found_url, sizeof(found_url))) {
            is_audio = false; /* 720p MP4 */
        } else if (find_url_for_itag(json, 78, found_url, sizeof(found_url))) {
            is_audio = false; /* 480p MP4 */
        } else if (find_url_for_itag(json, 59, found_url, sizeof(found_url))) {
            is_audio = false; /* 480p MP4 alternate */
        } else if (find_url_for_itag(json, 18, found_url, sizeof(found_url))) {
            is_audio = false; /* 360p MP4 fallback */
        } else if (find_url_for_itag(json, 140, found_url, sizeof(found_url))) {
            is_audio = true;
        } else if (find_url_for_itag(json, 139, found_url, sizeof(found_url))) {
            is_audio = true;
        } else if (find_url_for_itag(json, 251, found_url, sizeof(found_url))) {
            is_audio = true;
        } else {
            /* Last resort: any googlevideo URL */
            find_any_googlevideo_url(json, found_url, sizeof(found_url));
        }
    } else {
        /* Prefer itag=140 (audio-only MP4) for background music */
        if (find_url_for_itag(json, 140, found_url, sizeof(found_url))) {
            is_audio = true;
        } else if (find_url_for_itag(json, 139, found_url, sizeof(found_url))) {
            is_audio = true;
        } else if (find_url_for_itag(json, 251, found_url, sizeof(found_url))) {
            is_audio = true;
        } else if (!find_url_for_itag(json, 18, found_url, sizeof(found_url))) {
            /* Last resort: any googlevideo URL */
            find_any_googlevideo_url(json, found_url, sizeof(found_url));
        }
    }

    if (found_url[0]) {
        LOGI("Found direct URL via youtubei API: %.100s...", found_url);
        file_log("Found direct URL via youtubei API: %.100s...", found_url);
        strncpy(outMedia->url, found_url, sizeof(outMedia->url) - 1);
        outMedia->url[sizeof(outMedia->url) - 1] = '\0';
        if (is_audio) {
            snprintf(outMedia->mime, sizeof(outMedia->mime), "audio/mp4");
        } else {
            snprintf(outMedia->mime, sizeof(outMedia->mime), "video/mp4");
        }
        /* Extract title from videoDetails (try multiple paths) */
        const char *vd = strstr(json, "\"videoDetails\"");
        if (vd) {
            /* Path 1: videoDetails.title as plain string */
            const char *t = strstr(vd, "\"title\":\"");
            if (t) {
                t += 9;
                const char *tend = strchr(t, '"');
                if (tend) {
                    size_t tlen = (size_t)(tend - t);
                    if (tlen > 0 && tlen < sizeof(outMedia->title)) {
                        memcpy(outMedia->title, t, tlen);
                        outMedia->title[tlen] = '\0';
                        LOGI("Title from youtubei: %.100s", outMedia->title);
                    }
                }
            }
            /* Path 2: videoDetails.title.runs[0].text */
            if (!outMedia->title[0]) {
                const char *runs = strstr(vd, "\"runs\"");
                if (runs) {
                    const char *txt = strstr(runs, "\"text\":\"");
                    if (txt) {
                        txt += 8;
                        const char *tend = strchr(txt, '"');
                        if (tend) {
                            size_t tlen = (size_t)(tend - txt);
                            if (tlen > 0 && tlen < sizeof(outMedia->title)) {
                                memcpy(outMedia->title, txt, tlen);
                                outMedia->title[tlen] = '\0';
                                LOGI("Title from youtubei (runs): %.100s", outMedia->title);
                            }
                        }
                    }
                }
            }
            /* Path 3: microformat.playerMicroformatRenderer.title.simpleText */
            if (!outMedia->title[0]) {
                const char *mf = strstr(json, "\"playerMicroformatRenderer\"");
                if (mf) {
                    const char *st = strstr(mf, "\"simpleText\":\"");
                    if (st) {
                        st += 14;
                        const char *tend = strchr(st, '"');
                        if (tend) {
                            size_t tlen = (size_t)(tend - st);
                            if (tlen > 0 && tlen < sizeof(outMedia->title)) {
                                memcpy(outMedia->title, st, tlen);
                                outMedia->title[tlen] = '\0';
                                LOGI("Title from youtubei (microformat): %.100s", outMedia->title);
                            }
                        }
                    }
                }
            }
            /* Extract thumbnail */
            const char *th = strstr(vd, "\"thumbnails\"");
            if (th) {
                const char *last_url = NULL;
                const char *scan = th;
                while ((scan = strstr(scan, "\"url\":\"")) != NULL) {
                    last_url = scan + 7;
                    scan++;
                }
                if (last_url) {
                    const char *uend = strchr(last_url, '"');
                    if (uend) {
                        size_t ulen = (size_t)(uend - last_url);
                        if (ulen > 0 && ulen < sizeof(outMedia->thumbnailUrl)) {
                            memcpy(outMedia->thumbnailUrl, last_url, ulen);
                            outMedia->thumbnailUrl[ulen] = '\0';
                            LOGI("Thumbnail from youtubei: %.100s", outMedia->thumbnailUrl);
                        }
                    }
                }
            }
        }
        free(json);
        return true;
    }

    free(json);
    snprintf(err, errLen, "No direct URL found in youtubei API response");
    return false;
}

bool url_analyze_with_options(const char *inputUrl, MediaUrl *outMedia, char *err, size_t errLen, bool prefer_video) {
    LOGI("Starting URL analysis for: %.100s...", inputUrl);
    file_log("Starting URL analysis for: %.100s...", inputUrl);

    if (!inputUrl || !outMedia) {
        set_err(err, errLen, "Invalid URL input");
        LOGE("Invalid input: url=%p, out=%p", (void*)inputUrl, (void*)outMedia);
        return false;
    }
    outMedia->url[0] = '\0';
    outMedia->mime[0] = '\0';

    if (has_media_extension(inputUrl)) {
        LOGI("URL has media extension, using directly");
        file_log("URL has media extension, using directly");
        snprintf(outMedia->url, sizeof(outMedia->url), "%s", inputUrl);
        return true;
    }

    /* Proper browser emulation: first establish a session by loading the watch page,
     * then extract visitorData, then call the youtubei API with the session token.
     * This mirrors exactly what a real browser does. */
    char video_id[32] = {0};
    if (extract_video_id(inputUrl, video_id, sizeof(video_id))) {
        char visitor_data[1024] = {0};
        bool has_vd = fetch_visitor_data_from_watch_page(video_id, visitor_data, sizeof(visitor_data), err, errLen);
        if (!has_vd) {
            /* If watch page fetch failed, clear error and continue to fallback */
            if (err) err[0] = '\0';
        }
        if (try_youtubei_api(video_id, has_vd ? visitor_data : NULL, outMedia, err, errLen, prefer_video)) {
            LOGI("URL analysis complete via youtubei API");
            file_log("URL analysis complete via youtubei API");
            return true;
        }
        /* Clear error for fallback */
        if (err) err[0] = '\0';
    }

    /* Fallback: HTML scraping + JS execution */
    LOGI("youtubei API failed, falling back to HTML scraping...");
    file_log("youtubei API failed, falling back to HTML scraping...");

    LOGI("Fetching HTML from URL...");
    file_log("Fetching HTML from URL...");
    HttpBuffer html = {0};
    if (!http_get_to_memory(inputUrl, &html, err, errLen)) {
        LOGE("HTTP fetch failed: %s", err);
        file_log("HTTP fetch failed: %s", err);
        return false;
    }
    LOGI("Received %zu bytes of HTML", html.size);
    file_log("Received %zu bytes of HTML", html.size);

    LOGI("Extracting media URL from HTML...");
    file_log("Extracting media URL from HTML...");
    HtmlMediaCandidate candidate;
    if (!html_extract_media_url(html.data, &candidate, err, errLen)) {
        LOGE("Media extraction failed: %s", err);
        file_log("Media extraction failed: %s", err);
        http_free_buffer(&html);
        return false;
    }
    LOGI("Found media URL: %.100s...", candidate.url);
    file_log("Found media URL: %.100s...", candidate.url);

    http_free_buffer(&html);
    resolve_url(inputUrl, candidate.url, outMedia->url, sizeof(outMedia->url));
    if (candidate.mime[0]) {
        snprintf(outMedia->mime, sizeof(outMedia->mime), "%s", candidate.mime);
    }
    if (candidate.title[0]) {
        snprintf(outMedia->title, sizeof(outMedia->title), "%s", candidate.title);
    }
    if (candidate.thumbnailUrl[0]) {
        snprintf(outMedia->thumbnailUrl, sizeof(outMedia->thumbnailUrl), "%s", candidate.thumbnailUrl);
    }
    LOGI("URL analysis complete");
    return true;
}

bool url_analyze(const char *inputUrl, MediaUrl *outMedia, char *err, size_t errLen) {
    return url_analyze_with_options(inputUrl, outMedia, err, errLen, false);
}

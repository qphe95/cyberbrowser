/*
 * Image cache: asynchronous fetch, early size detection, and decoding on
 * worker threads.
 *
 * Pipeline per image (one entry per URL / path / data: URL):
 *   1. queued    — a request from layout (natural size) or paint (pixels)
 *   2. fetching  — a worker downloads the bytes (or reads the file)
 *   3. sized     — the header was parsed: the natural size is published at
 *                  once, before any pixel work, so layout can size boxes
 *   4. decoding  — decoded straight to the size it is painted at: JPEG uses
 *                  libjpeg-turbo's DCT-domain scaling (1/2, 1/4, 1/8 ...),
 *                  WebP uses libwebp's scaler, SVG rasterizes at that size
 *   5. ready     — pixels published (one atomic pointer: width, height and
 *                  data always agree)
 * A later paint request at a larger size than the decoded one re-decodes
 * from the retained compressed bytes.
 *
 * Threads: a small pool of workers (fetches block on sockets; decoding runs
 * on the same worker right after its fetch).  The main thread learns about
 * progress in image_cache_process_pending(), which invokes the callbacks.
 * Pixel buffers replaced by a re-decode are freed there too, so a buffer is
 * never freed while the (main-thread) rasterizer may be reading it.
 */
#include "image_cache.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>
#include <stdatomic.h>
#include <time.h>
#include <pthread.h>
#include "url_utils.h"

#ifdef _WIN32
#include <windows.h>
#endif

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "svg_render.h"

#include "turbojpeg.h"
#include "webp/decode.h"

#include "http_download.h"
#include "platform.h"
#include "cyber_env.h"
#include "cyber_profile.h"

#define LOG_TAG "image_cache"
#define LOG_ERROR(...) platform_log(LOG_LEVEL_ERROR, LOG_TAG, __VA_ARGS__)
#define LOG_INFO(...) platform_log(LOG_LEVEL_INFO, LOG_TAG, __VA_ARGS__)

enum {
    ST_QUEUED = 0,
    ST_FETCHING,
    ST_SIZED,       /* bytes + natural size known, no pixels yet */
    ST_DECODING,
    ST_READY,
    ST_ERROR
};

/* Pixels published as one unit so readers never see mismatched sizes. */
typedef struct ImagePixels {
    int width, height;
    uint8_t *data;      /* RGBA, top-left origin */
} ImagePixels;

typedef enum { FMT_UNKNOWN = 0, FMT_JPEG, FMT_PNG, FMT_GIF, FMT_WEBP, FMT_BMP, FMT_SVG } ImageFormat;

typedef struct ImageEntry {
    char *source;
    uint64_t hash;
    _Atomic int state;
    _Atomic int nat_w, nat_h;           /* natural size, 0 = unknown */
    _Atomic int want_w, want_h;         /* largest requested paint size */
    _Atomic(ImagePixels *) pixels;
    int decoded_full;                   /* decoded at natural size */

    /* Worker-owned until the entry is sized. */
    uint8_t *bytes;
    size_t nbytes;
    ImageFormat fmt;

    /* Main-thread notification state (guarded by the cache mutex). */
    ImageLoadCallback callback;
    void *user_data;
    bool notify_pending;
    bool queued;                        /* in the work queue */
    struct ImageEntry *qnext;
} ImageEntry;

struct ImageCache {
    pthread_mutex_t mu;
    pthread_cond_t work;                /* workers: queue non-empty / stop */
    pthread_cond_t progress;            /* waiters: an entry changed state */

    ImageEntry **entries;               /* handle = index; pointers are stable */
    int count, capacity;
    int *slots;                         /* open addressing: handle + 1, 0 = empty */
    int slot_mask;

    ImageEntry *qhead, *qtail;
    int busy;                           /* entries queued or being worked on */
    pthread_t threads[32];
    int nthreads;
    bool stop;

    ImagePixels **retired;              /* replaced pixels, freed on the main thread */
    int nretired, capretired;

    ImageLoadCallback notify;           /* cache-wide notification */
    void *notify_ud;
};

/* ------------------------------------------------------------------------- */
/* Small helpers                                                              */
/* ------------------------------------------------------------------------- */

static uint64_t hash_str(const char *s)
{
    uint64_t h = 1469598103934665603ull;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ull; }
    return h ? h : 1;
}

static void pixels_free(ImagePixels *p)
{
    if (!p) return;
    free(p->data);
    free(p);
}

static void cond_wait_ms(pthread_cond_t *c, pthread_mutex_t *m, int ms)
{
#ifdef _WIN32
    SleepConditionVariableSRW(c, m, (DWORD)ms, 0);
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_cond_timedwait(c, m, &ts);
#endif
}

/* Decode a base64-encoded string into newly allocated binary data. */
static uint8_t *base64_decode(const char *in, size_t in_len, size_t *out_len)
{
    static signed char tab[256];
    static int init = 0;
    if (!init) {
        memset(tab, -1, sizeof(tab));
        const char *b = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; b[i]; i++) tab[(unsigned char)b[i]] = (signed char)i;
        tab['-'] = 62; tab['_'] = 63;   /* base64url */
        init = 1;
    }
    if (!in || in_len == 0) return NULL;
    uint8_t *out = (uint8_t *)malloc((in_len / 4) * 3 + 3);
    if (!out) return NULL;
    int val = 0, valb = -8;
    size_t j = 0;
    for (size_t i = 0; i < in_len && in[i] != '='; i++) {
        int d = tab[(unsigned char)in[i]];
        if (d < 0) continue;
        val = (val << 6) + d;
        valb += 6;
        if (valb >= 0) {
            out[j++] = (uint8_t)((val >> valb) & 0xFF);
            valb -= 8;
        }
    }
    /* NUL-terminated like the percent-decoded payload: fetch() hands it to
     * JS_ParseJSON, which reads buf[len].  (The allocation has room: at most
     * 3 bytes per 4 characters, plus 2.) */
    out[j] = 0;
    *out_len = j;
    return out;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* data: URL payload (base64 or percent-encoded).  NULL when malformed.
 * Declared in url_utils.h; fetch() uses it for data: URLs too. */
uint8_t *data_url_decode(const char *url, size_t *out_len)
{
    if (!url_is_data_url(url)) return NULL;
    const char *comma = strchr(url, ',');
    if (!comma) return NULL;
    const char *header = url + 5;
    size_t header_len = (size_t)(comma - header);
    bool base64 = header_len >= 7 && strncasecmp(header + header_len - 7, ";base64", 7) == 0;
    const char *payload = comma + 1;
    if (base64) return base64_decode(payload, strlen(payload), out_len);
    size_t n = strlen(payload);
    uint8_t *out = (uint8_t *)malloc(n + 1);
    if (!out) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        if (payload[i] == '%' && i + 2 < n && hexval(payload[i + 1]) >= 0 && hexval(payload[i + 2]) >= 0) {
            out[j++] = (uint8_t)(hexval(payload[i + 1]) * 16 + hexval(payload[i + 2]));
            i += 2;
        } else {
            out[j++] = (uint8_t)payload[i];
        }
    }
    out[j] = 0;
    *out_len = j;
    return out;
}

/* ------------------------------------------------------------------------- */
/* Format sniffing and natural size from the header                          */
/* ------------------------------------------------------------------------- */

static bool is_svg_document(const uint8_t *d, size_t n)
{
    if (!d || n < 5) return false;
    size_t i = 0;
    if (n >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF) i = 3;
    while (i < n && isspace(d[i])) i++;
    if (i >= n || d[i] != '<') return false;
    size_t end = n - i < 4096 ? n : i + 4096;
    for (size_t j = i; j + 4 <= end; j++)
        if (memcmp(d + j, "<svg", 4) == 0) return true;
    return false;
}

static unsigned rd16be(const uint8_t *p) { return ((unsigned)p[0] << 8) | p[1]; }
static unsigned rd32be(const uint8_t *p) { return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3]; }
static unsigned rd16le(const uint8_t *p) { return p[0] | ((unsigned)p[1] << 8); }
static unsigned rd24le(const uint8_t *p) { return p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16); }
static unsigned rd32le(const uint8_t *p) { return p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24); }

static float svg_length(const char *v)
{
    if (!v) return 0.0f;
    float f = strtof(v, NULL);
    return f > 0.0f ? f : 0.0f;
}

/* Identify the format and read the natural size without decoding pixels. */
static ImageFormat sniff(const uint8_t *d, size_t n, int *w, int *h)
{
    *w = *h = 0;
    if (!d || n < 12) return FMT_UNKNOWN;
    if (n >= 24 && memcmp(d, "\x89PNG\r\n\x1a\n", 8) == 0) {
        *w = (int)rd32be(d + 16);
        *h = (int)rd32be(d + 20);
        return FMT_PNG;
    }
    if (memcmp(d, "GIF8", 4) == 0) {
        *w = (int)rd16le(d + 6);
        *h = (int)rd16le(d + 8);
        return FMT_GIF;
    }
    if (d[0] == 'B' && d[1] == 'M' && n >= 26) {
        *w = (int)rd32le(d + 18);
        int hh = (int)rd32le(d + 22);
        *h = hh < 0 ? -hh : hh;
        return FMT_BMP;
    }
    if (memcmp(d, "RIFF", 4) == 0 && memcmp(d + 8, "WEBP", 4) == 0) {
        int fw = 0, fh = 0;
        if (WebPGetInfo(d, n, &fw, &fh)) { *w = fw; *h = fh; }
        else if (n >= 30 && memcmp(d + 12, "VP8X", 4) == 0) {
            *w = (int)rd24le(d + 24) + 1;
            *h = (int)rd24le(d + 27) + 1;
        }
        return FMT_WEBP;
    }
    if (d[0] == 0xFF && d[1] == 0xD8) {
        /* Walk the markers to the first SOFn. */
        size_t i = 2;
        while (i + 9 < n) {
            if (d[i] != 0xFF) { i++; continue; }
            unsigned m = d[i + 1];
            if (m == 0xFF) { i++; continue; }
            if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }
            unsigned len = rd16be(d + i + 2);
            if ((m >= 0xC0 && m <= 0xCF) && m != 0xC4 && m != 0xC8 && m != 0xCC) {
                *h = (int)rd16be(d + i + 5);
                *w = (int)rd16be(d + i + 7);
                break;
            }
            i += 2 + len;
        }
        return FMT_JPEG;
    }
    if (is_svg_document(d, n)) {
        /* Natural size from the root element's width/height or viewBox. */
        char head[2048];
        size_t k = n < sizeof(head) - 1 ? n : sizeof(head) - 1;
        memcpy(head, d, k);
        head[k] = '\0';
        char *svg = strstr(head, "<svg");
        if (svg) {
            char *end = strchr(svg, '>');
            if (end) *end = '\0';
            char *pw = strstr(svg, " width=\"");
            if (!pw) pw = strstr(svg, " width='");
            char *ph = strstr(svg, " height=\"");
            if (!ph) ph = strstr(svg, " height='");
            float fw = pw ? svg_length(pw + 8) : 0.0f, fh = ph ? svg_length(ph + 9) : 0.0f;
            char *vb = strstr(svg, "viewBox=\"");
            if (!vb) vb = strstr(svg, "viewBox='");
            if (vb && (fw <= 0.0f || fh <= 0.0f)) {
                float a, b, c, e;
                if (sscanf(vb + 9, "%f%*[ ,]%f%*[ ,]%f%*[ ,]%f", &a, &b, &c, &e) == 4 && c > 0 && e > 0) {
                    if (fw > 0.0f) fh = fw * e / c;
                    else if (fh > 0.0f) fw = fh * c / e;
                    else { fw = c; fh = e; }
                }
            }
            if (fw <= 0.0f || fh <= 0.0f) { fw = 300.0f; fh = 150.0f; }
            *w = (int)(fw + 0.5f);
            *h = (int)(fh + 0.5f);
        }
        return FMT_SVG;
    }
    return FMT_UNKNOWN;
}

/* ------------------------------------------------------------------------- */
/* Decoders                                                                   */
/* ------------------------------------------------------------------------- */

/* Size to decode at: the smallest of the natural size and a size covering
 * the requested paint size (aspect preserved).  0 want = natural. */
static void target_size(int nw, int nh, int ww, int wh, int *tw, int *th)
{
    *tw = nw; *th = nh;
    if (nw <= 0 || nh <= 0 || ww <= 0 || wh <= 0) return;
    double s = fmax((double)ww / nw, (double)wh / nh);
    if (s >= 1.0) return;
    *tw = (int)ceil(nw * s);
    *th = (int)ceil(nh * s);
    if (*tw < 1) *tw = 1;
    if (*th < 1) *th = 1;
}

static ImagePixels *pixels_new(int w, int h)
{
    ImagePixels *p = (ImagePixels *)malloc(sizeof(ImagePixels));
    if (!p) return NULL;
    p->width = w;
    p->height = h;
    p->data = (uint8_t *)malloc((size_t)w * (size_t)h * 4);
    if (!p->data) { free(p); return NULL; }
    return p;
}

static ImagePixels *decode_jpeg(const uint8_t *d, size_t n, int ww, int wh)
{
    tjhandle tj = tj3Init(TJINIT_DECOMPRESS);
    if (!tj) return NULL;
    ImagePixels *px = NULL;
    if (tj3DecompressHeader(tj, d, n) == 0) {
        int w = tj3Get(tj, TJPARAM_JPEGWIDTH), h = tj3Get(tj, TJPARAM_JPEGHEIGHT);
        int tw, th;
        target_size(w, h, ww, wh, &tw, &th);
        /* DCT-domain scaling: the smallest factor that still covers the
         * target size (factors are num/8 for num = 1..16). */
        int nf = 0;
        tjscalingfactor *sf = tj3GetScalingFactors(&nf);
        tjscalingfactor best = { 1, 1 };
        int best_w = w;
        for (int i = 0; sf && i < nf; i++) {
            if (sf[i].num > sf[i].denom) continue;     /* no upscaling */
            int sw = TJSCALED(w, sf[i]), sh = TJSCALED(h, sf[i]);
            if (sw >= tw && sh >= th && sw < best_w) { best = sf[i]; best_w = sw; }
        }
        tj3SetScalingFactor(tj, best);
        int ow = TJSCALED(w, best), oh = TJSCALED(h, best);
        px = pixels_new(ow, oh);
        if (px && tj3Decompress8(tj, d, n, px->data, ow * 4, TJPF_RGBA) != 0 &&
            tj3GetErrorCode(tj) == TJERR_FATAL) {
            /* Warnings (truncated data, ...) still leave usable pixels; a
             * fatal error (e.g. CMYK) falls back to stb_image. */
            pixels_free(px);
            px = NULL;
        }
    }
    tj3Destroy(tj);
    return px;
}

static ImagePixels *decode_webp(const uint8_t *d, size_t n, int ww, int wh)
{
    WebPDecoderConfig cfg;
    if (!WebPInitDecoderConfig(&cfg)) return NULL;
    if (WebPGetFeatures(d, n, &cfg.input) != VP8_STATUS_OK) return NULL;
    int w = cfg.input.width, h = cfg.input.height;
    int tw, th;
    target_size(w, h, ww, wh, &tw, &th);
    if (tw != w || th != h) {
        cfg.options.use_scaling = 1;
        cfg.options.scaled_width = tw;
        cfg.options.scaled_height = th;
    }
    ImagePixels *px = pixels_new(tw, th);
    if (!px) return NULL;
    cfg.output.colorspace = MODE_RGBA;
    cfg.output.is_external_memory = 1;
    cfg.output.u.RGBA.rgba = px->data;
    cfg.output.u.RGBA.stride = tw * 4;
    cfg.output.u.RGBA.size = (size_t)tw * (size_t)th * 4;
    if (WebPDecode(d, n, &cfg) != VP8_STATUS_OK) {
        pixels_free(px);
        px = NULL;
    }
    WebPFreeDecBuffer(&cfg.output);
    return px;
}

static ImagePixels *decode_svg(const uint8_t *d, size_t n, int nw, int nh, int ww, int wh)
{
    float sw = 0.0f, sh = 0.0f;
    if (!svg_intrinsic_size((const char *)d, n, &sw, &sh)) return NULL;
    if (sw <= 0.0f) sw = (float)nw;
    if (sh <= 0.0f) sh = (float)nh;
    if (sw <= 0.0f || sh <= 0.0f) { sw = 300.0f; sh = 150.0f; }
    /* Vectors rasterize at the paint size (crisp at any scale); the
     * "natural size" request marker means 1:1. */
    if (ww >= (1 << 20) || wh >= (1 << 20)) ww = wh = 0;
    float scale = 1.0f;
    if (ww > 0 && wh > 0) scale = fmaxf((float)ww / sw, (float)wh / sh);
    if (sw * scale > 4096.0f || sh * scale > 4096.0f) scale = 4096.0f / fmaxf(sw, sh);
    int iw = (int)(sw * scale + 0.5f), ih = (int)(sh * scale + 0.5f);
    uint8_t *rgba = NULL;
    int ow = 0, oh = 0;
    /* An <img> has no color context: currentColor is black. */
    if (iw <= 0 || ih <= 0 || !svg_render_rgba((const char *)d, n, iw, ih, 0x000000FFu, &rgba, &ow, &oh))
        return NULL;
    ImagePixels *px = (ImagePixels *)malloc(sizeof(ImagePixels));
    if (!px) { free(rgba); return NULL; }
    px->width = ow;
    px->height = oh;
    px->data = rgba;
    return px;
}

static ImagePixels *decode_stb(const uint8_t *d, size_t n)
{
    int w = 0, h = 0, ch = 0;
    uint8_t *data = stbi_load_from_memory(d, (int)n, &w, &h, &ch, 4);
    if (!data) return NULL;
    ImagePixels *px = (ImagePixels *)malloc(sizeof(ImagePixels));
    if (!px) { stbi_image_free(data); return NULL; }
    px->width = w;
    px->height = h;
    px->data = data;    /* stb allocates with malloc */
    return px;
}

static ImagePixels *decode_bytes(ImageEntry *e, int ww, int wh)
{
    ImagePixels *px = NULL;
    switch (e->fmt) {
    case FMT_JPEG: px = decode_jpeg(e->bytes, e->nbytes, ww, wh); break;
    case FMT_WEBP: px = decode_webp(e->bytes, e->nbytes, ww, wh); break;
    case FMT_SVG:  px = decode_svg(e->bytes, e->nbytes, atomic_load(&e->nat_w),
                                   atomic_load(&e->nat_h), ww, wh); break;
    default: break;
    }
    if (!px && e->fmt != FMT_SVG && e->fmt != FMT_WEBP) px = decode_stb(e->bytes, e->nbytes);
    return px;
}

/* ------------------------------------------------------------------------- */
/* Fetching                                                                   */
/* ------------------------------------------------------------------------- */

static bool read_file(const char *path, uint8_t **out, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return false; }
    uint8_t *b = (uint8_t *)malloc((size_t)n + 1);
    if (!b) { fclose(f); return false; }
    size_t got = fread(b, 1, (size_t)n, f);
    fclose(f);
    *out = b;
    *len = got;
    return true;
}

static bool http_fetch(const char *url, uint8_t **out, size_t *len)
{
    HttpBuffer buf = { 0 };
    char err[256] = { 0 };
    bool got = false;
    /* CDNs (notably upload.wikimedia.org) answer bursts with HTTP 429:
     * retry when the response says to (Retry-After, capped), else with
     * backoff.  Retrying sooner than asked only draws another 429. */
    unsigned int delay_ms = 0;
    for (int attempt = 0; attempt < 4 && !got; attempt++) {
        if (attempt > 0) {
            platform_sleep_ms(delay_ms);
            http_free_buffer(&buf);
            err[0] = '\0';
        }
        CP_SCOPE_CAT("image-download", "img");
        got = http_get_to_memory(url, &buf, err, sizeof(err));
        if (!got && strstr(err, "429") == NULL) break;
        const char *ra = strstr(err, "retry-after ");
        int ra_s = ra ? atoi(ra + 12) : -1;
        delay_ms = ra_s >= 0 ? (unsigned int)(ra_s > 5 ? 5000 : (ra_s ? ra_s * 1000 : 200))
                             : (unsigned int)(400 * (attempt + 1) * (attempt + 1));
    }
    if (!got || !buf.data || buf.size == 0) {
        LOG_ERROR("Failed to download image %s: %s", url, err);
        http_free_buffer(&buf);
        return false;
    }
    if (buf.headers) { free(buf.headers); buf.headers = NULL; }
    *out = (uint8_t *)buf.data;
    *len = buf.size;
    return true;
}

static bool fetch_entry(ImageEntry *e)
{
    const char *src = e->source;
    if (url_is_data_url(src)) {
        e->bytes = data_url_decode(src, &e->nbytes);
        return e->bytes != NULL;
    }
    if (url_is_network_url(src)) return http_fetch(src, &e->bytes, &e->nbytes);
    return read_file(src, &e->bytes, &e->nbytes);
}

/* ------------------------------------------------------------------------- */
/* Worker pool                                                                */
/* ------------------------------------------------------------------------- */

static void enqueue_locked(ImageCache *c, ImageEntry *e)
{
    if (e->queued) return;
    e->queued = true;
    e->qnext = NULL;
    if (c->qtail) c->qtail->qnext = e; else c->qhead = e;
    c->qtail = e;
    c->busy++;
    pthread_cond_signal(&c->work);
}

static void notify_locked(ImageCache *c, ImageEntry *e)
{
    e->notify_pending = true;
    pthread_cond_broadcast(&c->progress);
}

/* Fetch + sniff + decode one entry (called without the lock). */
static void work_entry(ImageCache *c, ImageEntry *e)
{
    int st = atomic_load(&e->state);
    if (st == ST_QUEUED) {
        atomic_store(&e->state, ST_FETCHING);
        bool ok = fetch_entry(e);
        int w = 0, h = 0;
        if (ok) e->fmt = sniff(e->bytes, e->nbytes, &w, &h);
        if (!ok || e->fmt == FMT_UNKNOWN) {
            /* Unknown magic: let stb try (TGA, PSD, ...). */
            if (ok) {
                int cw, ch, cc;
                if (stbi_info_from_memory(e->bytes, (int)e->nbytes, &cw, &ch, &cc)) {
                    w = cw; h = ch;
                } else {
                    ok = false;
                }
            }
        }
        if (!ok) {
            pthread_mutex_lock(&c->mu);
            atomic_store(&e->state, ST_ERROR);
            notify_locked(c, e);
            pthread_mutex_unlock(&c->mu);
            return;
        }
        atomic_store(&e->nat_w, w);
        atomic_store(&e->nat_h, h);
        pthread_mutex_lock(&c->mu);
        atomic_store(&e->state, ST_SIZED);
        notify_locked(c, e);            /* layout can use the size now */
        pthread_mutex_unlock(&c->mu);
    }
    if (atomic_load(&e->state) == ST_ERROR) return;

    int ww = atomic_load(&e->want_w), wh = atomic_load(&e->want_h);
    int prev = atomic_exchange(&e->state, ST_DECODING);
    ImagePixels *px;
    {
        CP_SCOPE_CAT("image-decode", "img");
        px = decode_bytes(e, ww, wh);
    }
    pthread_mutex_lock(&c->mu);
    if (!px) {
        if (!atomic_load(&e->pixels)) {
            LOG_ERROR("Failed to decode image %.120s", e->source);
            atomic_store(&e->state, ST_ERROR);
        } else {
            atomic_store(&e->state, prev == ST_READY ? ST_READY : ST_ERROR);
        }
    } else {
        ImagePixels *old = atomic_exchange(&e->pixels, px);
        if (old) {
            if (c->nretired == c->capretired) {
                c->capretired = c->capretired ? c->capretired * 2 : 16;
                c->retired = (ImagePixels **)realloc(c->retired, (size_t)c->capretired * sizeof(*c->retired));
            }
            c->retired[c->nretired++] = old;
        }
        e->decoded_full = e->fmt != FMT_SVG &&
                          px->width >= atomic_load(&e->nat_w) && px->height >= atomic_load(&e->nat_h);
        atomic_store(&e->state, ST_READY);
        /* A larger paint size requested while decoding: decode again
         * (only when the request grew, so a decoder that cannot reach the
         * target never loops). */
        if (!e->decoded_full &&
            (atomic_load(&e->want_w) > ww || atomic_load(&e->want_h) > wh)) {
            int tw, th;
            target_size(atomic_load(&e->nat_w), atomic_load(&e->nat_h),
                        atomic_load(&e->want_w), atomic_load(&e->want_h), &tw, &th);
            if (tw > px->width || th > px->height || e->fmt == FMT_SVG) enqueue_locked(c, e);
        }
    }
    notify_locked(c, e);
    pthread_mutex_unlock(&c->mu);
}

static void *worker_main(void *arg)
{
    ImageCache *c = (ImageCache *)arg;
    pthread_mutex_lock(&c->mu);
    for (;;) {
        while (!c->qhead && !c->stop) pthread_cond_wait(&c->work, &c->mu);
        if (c->stop) break;
        ImageEntry *e = c->qhead;
        c->qhead = e->qnext;
        if (!c->qhead) c->qtail = NULL;
        e->queued = false;
        pthread_mutex_unlock(&c->mu);
        work_entry(c, e);
        pthread_mutex_lock(&c->mu);
        c->busy--;
        pthread_cond_broadcast(&c->progress);
    }
    pthread_mutex_unlock(&c->mu);
    return NULL;
}

static void start_workers_locked(ImageCache *c)
{
    if (c->nthreads > 0) return;
    int n = 8;
    const char *v = cyber_getenv("CYBER_IMAGE_THREADS");
    if (v && *v) n = atoi(v);
    if (n < 1) n = 1;
    if (n > 32) n = 32;
    for (int i = 0; i < n; i++) {
        if (pthread_create(&c->threads[c->nthreads], NULL, worker_main, c) != 0) break;
        c->nthreads++;
    }
}

/* ------------------------------------------------------------------------- */
/* Entry table                                                                */
/* ------------------------------------------------------------------------- */

static ImageEntry *find_locked(ImageCache *c, const char *src, uint64_t h)
{
    if (!c->slots) return NULL;
    for (int i = (int)(h & (uint64_t)c->slot_mask);; i = (i + 1) & c->slot_mask) {
        int s = c->slots[i];
        if (!s) return NULL;
        ImageEntry *e = c->entries[s - 1];
        if (e->hash == h && strcmp(e->source, src) == 0) return e;
    }
}

static void rehash_locked(ImageCache *c, int nslots)
{
    free(c->slots);
    c->slots = (int *)calloc((size_t)nslots, sizeof(int));
    c->slot_mask = nslots - 1;
    for (int k = 0; k < c->count; k++) {
        int i = (int)(c->entries[k]->hash & (uint64_t)c->slot_mask);
        while (c->slots[i]) i = (i + 1) & c->slot_mask;
        c->slots[i] = k + 1;
    }
}

/* Find or create the entry for `src`; returns its handle (-1 on failure). */
static int get_entry_locked(ImageCache *c, const char *src, ImageEntry **out)
{
    uint64_t h = hash_str(src);
    ImageEntry *e = find_locked(c, src, h);
    if (e) {
        *out = e;
        for (int k = 0; k < c->count; k++) if (c->entries[k] == e) return k;
    }
    if (c->count == c->capacity) {
        int nc = c->capacity ? c->capacity * 2 : 64;
        ImageEntry **ne = (ImageEntry **)realloc(c->entries, (size_t)nc * sizeof(*ne));
        if (!ne) return -1;
        c->entries = ne;
        c->capacity = nc;
    }
    e = (ImageEntry *)calloc(1, sizeof(ImageEntry));
    if (!e) return -1;
    e->source = strdup(src);
    e->hash = h;
    atomic_store(&e->state, ST_QUEUED);
    int handle = c->count++;
    c->entries[handle] = e;
    if (!c->slots || c->count * 2 > c->slot_mask + 1) {
        int ns = c->slots ? (c->slot_mask + 1) * 2 : 256;
        while (c->count * 2 > ns) ns *= 2;
        rehash_locked(c, ns);
    } else {
        int i = (int)(h & (uint64_t)c->slot_mask);
        while (c->slots[i]) i = (i + 1) & c->slot_mask;
        c->slots[i] = handle + 1;
    }
    start_workers_locked(c);
    enqueue_locked(c, e);
    *out = e;
    return handle;
}

/* Record the paint size and re-decode when it exceeds the decoded size. */
static void want_size_locked(ImageCache *c, ImageEntry *e, int w, int h)
{
    if (w <= 0 || h <= 0) {
        /* Natural size wanted. */
        w = 1 << 20;
        h = 1 << 20;
    }
    int ow = atomic_load(&e->want_w), oh = atomic_load(&e->want_h);
    if (w <= ow && h <= oh) return;
    atomic_store(&e->want_w, w > ow ? w : ow);
    atomic_store(&e->want_h, h > oh ? h : oh);
    ImagePixels *px = atomic_load(&e->pixels);
    if (atomic_load(&e->state) == ST_READY && px && !e->decoded_full) {
        int tw, th;
        if (e->fmt == FMT_SVG) {
            tw = w < (1 << 20) ? w : 0;
            th = h < (1 << 20) ? h : 0;
        } else {
            target_size(atomic_load(&e->nat_w), atomic_load(&e->nat_h),
                        atomic_load(&e->want_w), atomic_load(&e->want_h), &tw, &th);
        }
        if (tw > px->width || th > px->height) enqueue_locked(c, e);
    }
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                 */
/* ------------------------------------------------------------------------- */

ImageCache *image_cache_create(void)
{
    ImageCache *c = (ImageCache *)calloc(1, sizeof(ImageCache));
    if (!c) return NULL;
    pthread_mutex_init(&c->mu, NULL);
    pthread_cond_init(&c->work, NULL);
    pthread_cond_init(&c->progress, NULL);
    return c;
}

void image_cache_destroy(ImageCache *c)
{
    if (!c) return;
    pthread_mutex_lock(&c->mu);
    c->stop = true;
    pthread_cond_broadcast(&c->work);
    pthread_mutex_unlock(&c->mu);
    /* Workers finish the entry they are on (a fetch may take a while). */
    for (int i = 0; i < c->nthreads; i++) pthread_join(c->threads[i], NULL);
    for (int k = 0; k < c->count; k++) {
        ImageEntry *e = c->entries[k];
        pixels_free(atomic_load(&e->pixels));
        free(e->bytes);
        free(e->source);
        free(e);
    }
    for (int i = 0; i < c->nretired; i++) pixels_free(c->retired[i]);
    free(c->retired);
    free(c->entries);
    free(c->slots);
    pthread_cond_destroy(&c->work);
    pthread_cond_destroy(&c->progress);
    pthread_mutex_destroy(&c->mu);
    free(c);
}

void image_cache_set_notify(ImageCache *c, ImageLoadCallback cb, void *user_data)
{
    if (!c) return;
    pthread_mutex_lock(&c->mu);
    c->notify = cb;
    c->notify_ud = user_data;
    pthread_mutex_unlock(&c->mu);
}

int image_cache_request(ImageCache *c, const char *url_or_path, int want_w, int want_h)
{
    if (!c || !url_or_path || !url_or_path[0]) return -1;
    pthread_mutex_lock(&c->mu);
    ImageEntry *e = NULL;
    int handle = get_entry_locked(c, url_or_path, &e);
    if (handle >= 0) want_size_locked(c, e, want_w, want_h);
    pthread_mutex_unlock(&c->mu);
    return handle;
}

bool image_cache_natural_size(ImageCache *c, const char *url_or_path, int *out_w, int *out_h)
{
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    if (!c || !url_or_path || !url_or_path[0]) return false;
    pthread_mutex_lock(&c->mu);
    ImageEntry *e = NULL;
    int handle = get_entry_locked(c, url_or_path, &e);
    int w = 0, h = 0, st = ST_ERROR;
    if (handle >= 0) {
        w = atomic_load(&e->nat_w);
        h = atomic_load(&e->nat_h);
        st = atomic_load(&e->state);
    }
    pthread_mutex_unlock(&c->mu);
    if (st == ST_ERROR || st == ST_QUEUED || st == ST_FETCHING) return false;
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return w > 0 && h > 0;
}

int image_cache_status(ImageCache *c, const char *url_or_path)
{
    if (!c || !url_or_path || !url_or_path[0]) return -1;
    pthread_mutex_lock(&c->mu);
    ImageEntry *e = NULL;
    int handle = get_entry_locked(c, url_or_path, &e);
    int st = handle >= 0 ? atomic_load(&e->state) : ST_ERROR;
    pthread_mutex_unlock(&c->mu);
    if (st == ST_ERROR) return -1;
    return st >= ST_SIZED ? 1 : 0;
}

int image_cache_load_async(ImageCache *c, const char *url_or_path,
                           ImageLoadCallback callback, void *user_data)
{
    if (!c || !url_or_path || !url_or_path[0]) return -1;
    pthread_mutex_lock(&c->mu);
    ImageEntry *e = NULL;
    int handle = get_entry_locked(c, url_or_path, &e);
    if (handle >= 0) {
        want_size_locked(c, e, 0, 0);
        int st = atomic_load(&e->state);
        /* Already decoded: pixels were available before this call, so do not
         * notify again (re-notifying would re-trigger layout forever). */
        if (st != ST_READY && st != ST_ERROR) {
            e->callback = callback;
            e->user_data = user_data;
        }
    }
    pthread_mutex_unlock(&c->mu);
    return handle;
}

int image_cache_load(ImageCache *c, const char *url_or_path)
{
    int handle = image_cache_load_async(c, url_or_path, NULL, NULL);
    if (handle < 0) return -1;
    pthread_mutex_lock(&c->mu);
    ImageEntry *e = c->entries[handle];
    for (;;) {
        int st = atomic_load(&e->state);
        if (st == ST_ERROR) { handle = -1; break; }
        if (st == ST_READY && !e->queued) break;
        pthread_cond_wait(&c->progress, &c->mu);
    }
    pthread_mutex_unlock(&c->mu);
    return handle;
}

bool image_cache_process_pending(ImageCache *c)
{
    if (!c) return false;
    /* Collect notifications under the lock, run callbacks outside it. */
    typedef struct { ImageLoadCallback cb; void *ud; char *url; } Note;
    Note *notes = NULL;
    int nn = 0, cap = 0;
    ImagePixels **retired = NULL;
    int nretired = 0;
    ImageLoadCallback global = NULL;
    void *global_ud = NULL;
    pthread_mutex_lock(&c->mu);
    for (int k = 0; k < c->count; k++) {
        ImageEntry *e = c->entries[k];
        if (!e->notify_pending) continue;
        e->notify_pending = false;
        int st = atomic_load(&e->state);
        if (nn == cap) {
            cap = cap ? cap * 2 : 32;
            notes = (Note *)realloc(notes, (size_t)cap * sizeof(Note));
        }
        notes[nn].cb = (st == ST_READY || st == ST_ERROR) ? e->callback : NULL;
        notes[nn].ud = e->user_data;
        notes[nn].url = strdup(e->source);
        nn++;
        if (st == ST_READY || st == ST_ERROR) e->callback = NULL;
    }
    retired = c->retired;
    nretired = c->nretired;
    c->retired = NULL;
    c->nretired = c->capretired = 0;
    global = c->notify;
    global_ud = c->notify_ud;
    pthread_mutex_unlock(&c->mu);

    /* Replaced pixel buffers: no paint is in progress on this thread. */
    for (int i = 0; i < nretired; i++) pixels_free(retired[i]);
    free(retired);

    for (int i = 0; i < nn; i++) {
        if (notes[i].cb) notes[i].cb(notes[i].url, notes[i].ud);
        if (global) global(notes[i].url, global_ud);
        free(notes[i].url);
    }
    free(notes);
    return nn > 0;
}

void image_cache_wait_pending(ImageCache *c)
{
    if (!c) return;
    pthread_mutex_lock(&c->mu);
    while (c->busy > 0) pthread_cond_wait(&c->progress, &c->mu);
    pthread_mutex_unlock(&c->mu);
    image_cache_process_pending(c);
}

bool image_cache_wait_pending_ms(ImageCache *c, int timeout_ms)
{
    if (!c) return true;
    unsigned long long deadline = platform_get_time_ms() + (unsigned long long)(timeout_ms > 0 ? timeout_ms : 0);
    pthread_mutex_lock(&c->mu);
    while (c->busy > 0) {
        unsigned long long now = platform_get_time_ms();
        if (now >= deadline) break;
        cond_wait_ms(&c->progress, &c->mu, (int)(deadline - now));
    }
    bool done = c->busy == 0;
    pthread_mutex_unlock(&c->mu);
    image_cache_process_pending(c);
    return done;
}

bool image_cache_wait_progress(ImageCache *c, int timeout_ms)
{
    if (!c) return false;
    bool any = false;
    pthread_mutex_lock(&c->mu);
    for (int k = 0; k < c->count && !any; k++) any = c->entries[k]->notify_pending;
    if (!any && c->busy > 0) {
        cond_wait_ms(&c->progress, &c->mu, timeout_ms);
        for (int k = 0; k < c->count && !any; k++) any = c->entries[k]->notify_pending;
    }
    pthread_mutex_unlock(&c->mu);
    return any;
}

void image_cache_pending_counts(ImageCache *c, int *working, int *undelivered)
{
    *working = *undelivered = 0;
    if (!c) return;
    pthread_mutex_lock(&c->mu);
    *working = c->busy;
    for (int k = 0; k < c->count; k++) *undelivered += c->entries[k]->notify_pending;
    pthread_mutex_unlock(&c->mu);
}

bool image_cache_has_work(ImageCache *c)
{
    if (!c) return false;
    pthread_mutex_lock(&c->mu);
    bool busy = c->busy > 0;
    pthread_mutex_unlock(&c->mu);
    return busy;
}

bool image_cache_has_pending(ImageCache *c)
{
    if (!c) return false;
    pthread_mutex_lock(&c->mu);
    bool pending = c->busy > 0;
    for (int k = 0; k < c->count && !pending; k++) pending = c->entries[k]->notify_pending;
    pthread_mutex_unlock(&c->mu);
    return pending;
}

bool image_cache_get(ImageCache *c, int handle,
                     int *out_width, int *out_height, int *out_channels,
                     uint8_t **out_pixels)
{
    if (!c || handle < 0) return false;
    pthread_mutex_lock(&c->mu);
    ImageEntry *e = handle < c->count ? c->entries[handle] : NULL;
    pthread_mutex_unlock(&c->mu);
    if (!e) return false;
    ImagePixels *px = atomic_load(&e->pixels);
    if (!px) return false;
    if (out_width)    *out_width    = px->width;
    if (out_height)   *out_height   = px->height;
    if (out_channels) *out_channels = 4;
    if (out_pixels)   *out_pixels   = px->data;
    return true;
}

bool image_cache_get_natural(ImageCache *c, int handle, int *out_w, int *out_h)
{
    if (!c || handle < 0) return false;
    pthread_mutex_lock(&c->mu);
    ImageEntry *e = handle < c->count ? c->entries[handle] : NULL;
    pthread_mutex_unlock(&c->mu);
    if (!e) return false;
    int w = atomic_load(&e->nat_w), h = atomic_load(&e->nat_h);
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return w > 0 && h > 0;
}

void image_cache_stats(ImageCache *c, ImageCacheStats *s)
{
    memset(s, 0, sizeof(*s));
    if (!c) return;
    pthread_mutex_lock(&c->mu);
    s->entries = c->count;
    for (int k = 0; k < c->count; k++) {
        ImageEntry *e = c->entries[k];
        int st = atomic_load(&e->state);
        if (st == ST_READY) s->ready++;
        else if (st == ST_ERROR) s->errors++;
        else s->pending++;
        ImagePixels *px = atomic_load(&e->pixels);
        if (px) s->pixel_bytes += (size_t)px->width * (size_t)px->height * 4;
        s->compressed_bytes += e->nbytes;
    }
    pthread_mutex_unlock(&c->mu);
}

/* Insert or replace an image whose pixels the caller already has (decoded
 * video frames).  The entry is never fetched; replaced pixels retire like a
 * re-decode's, so a pointer from image_cache_get stays valid until the next
 * image_cache_process_pending(). */
int image_cache_put_rgba(ImageCache *c, const char *key, int w, int h, const uint8_t *rgba)
{
    if (!c || !key || !key[0] || w <= 0 || h <= 0 || !rgba) return -1;
    ImagePixels *px = (ImagePixels *)calloc(1, sizeof(*px));
    if (!px) return -1;
    px->data = (uint8_t *)malloc((size_t)w * (size_t)h * 4);
    if (!px->data) { free(px); return -1; }
    memcpy(px->data, rgba, (size_t)w * (size_t)h * 4);
    px->width = w;
    px->height = h;

    pthread_mutex_lock(&c->mu);
    uint64_t hs = hash_str(key);
    ImageEntry *e = find_locked(c, key, hs);
    int handle = -1;
    if (e) {
        for (int k = 0; k < c->count; k++) if (c->entries[k] == e) { handle = k; break; }
    } else {
        if (c->count == c->capacity) {
            int nc = c->capacity ? c->capacity * 2 : 64;
            ImageEntry **ne = (ImageEntry **)realloc(c->entries, (size_t)nc * sizeof(*ne));
            if (!ne) { pthread_mutex_unlock(&c->mu); pixels_free(px); return -1; }
            c->entries = ne;
            c->capacity = nc;
        }
        e = (ImageEntry *)calloc(1, sizeof(ImageEntry));
        if (!e) { pthread_mutex_unlock(&c->mu); pixels_free(px); return -1; }
        e->source = strdup(key);
        e->hash = hs;
        handle = c->count++;
        c->entries[handle] = e;
        if (!c->slots || c->count * 2 > c->slot_mask + 1) {
            int ns = c->slots ? (c->slot_mask + 1) * 2 : 256;
            while (c->count * 2 > ns) ns *= 2;
            rehash_locked(c, ns);
        } else {
            int i = (int)(hs & (uint64_t)c->slot_mask);
            while (c->slots[i]) i = (i + 1) & c->slot_mask;
            c->slots[i] = handle + 1;
        }
    }
    e->decoded_full = 1;
    atomic_store(&e->nat_w, w);
    atomic_store(&e->nat_h, h);
    ImagePixels *old = atomic_exchange(&e->pixels, px);
    atomic_store(&e->state, ST_READY);
    if (old) {
        if (c->nretired == c->capretired) {
            c->capretired = c->capretired ? c->capretired * 2 : 16;
            c->retired = (ImagePixels **)realloc(c->retired, (size_t)c->capretired * sizeof(*c->retired));
        }
        c->retired[c->nretired++] = old;
    }
    pthread_mutex_unlock(&c->mu);
    return handle;
}

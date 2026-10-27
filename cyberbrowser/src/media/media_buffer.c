/* MSE media buffers: segment framing, demux to samples, buffered ranges and
 * video decode on a worker thread (media_buffer.h). */
#include "media_buffer.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---- MIME types (no ffmpeg needed) ---- */

static bool str_has_ci(const char *s, const char *needle)
{
    size_t n = strlen(needle);
    for (; *s; s++)
        if (strncasecmp(s, needle, n) == 0) return true;
    return false;
}

/* Codecs the decoder set (tools/build_ffmpeg.sh) handles. */
static bool codec_supported(const char *c, bool webm)
{
    while (*c == ' ' || *c == '"' || *c == '\'') c++;
    if (strncasecmp(c, "avc1", 4) == 0 || strncasecmp(c, "avc3", 4) == 0) return !webm;
    if (strncasecmp(c, "mp4a", 4) == 0) return !webm;
    if (strncasecmp(c, "vp9", 3) == 0 || strncasecmp(c, "vp09", 4) == 0) return true;
    if (strncasecmp(c, "opus", 4) == 0) return true;
    return false;
}

bool media_type_supported(const char *mime)
{
    if (!mime || !media_available()) return false;
    while (isspace((unsigned char)*mime)) mime++;
    bool webm = strncasecmp(mime, "video/webm", 10) == 0 || strncasecmp(mime, "audio/webm", 10) == 0;
    bool mp4 = strncasecmp(mime, "video/mp4", 9) == 0 || strncasecmp(mime, "audio/mp4", 9) == 0;
    if (!webm && !mp4) return false;
    const char *codecs = strstr(mime, "codecs=");
    if (!codecs) return true;
    codecs += 7;
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", codecs);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ","))
        if (!codec_supported(tok, webm)) return false;
    return true;
}

#ifndef CYBER_HAVE_FFMPEG

struct MediaBuffer { int unused; };

bool media_available(void) { return false; }
MediaBuffer *media_buffer_create(const char *m) { (void)m; return calloc(1, sizeof(MediaBuffer)); }
void media_buffer_destroy(MediaBuffer *mb) { free(mb); }
bool media_buffer_append(MediaBuffer *mb, const uint8_t *d, size_t n, double o)
{ (void)mb; (void)d; (void)n; (void)o; return true; }
void media_buffer_remove(MediaBuffer *mb, double s, double e) { (void)mb; (void)s; (void)e; }
void media_buffer_reset_parser(MediaBuffer *mb) { (void)mb; }
int media_buffer_ranges(MediaBuffer *mb, MediaRange *o, int m) { (void)mb; (void)o; (void)m; return 0; }
bool media_buffer_has_init(const MediaBuffer *mb) { (void)mb; return false; }
MediaKind media_buffer_kind(const MediaBuffer *mb) { (void)mb; return MEDIA_KIND_UNKNOWN; }
double media_buffer_duration(const MediaBuffer *mb) { (void)mb; return 0.0; }
void media_buffer_video_size(const MediaBuffer *mb, int *w, int *h) { (void)mb; *w = *h = 0; }
bool media_buffer_frame_at(MediaBuffer *mb, double t, const uint8_t **p, int *w, int *h, uint64_t *id)
{ (void)mb; (void)t; (void)p; (void)w; (void)h; (void)id; return false; }
void media_buffer_prefetch(MediaBuffer *mb, double t) { (void)mb; (void)t; }

#else

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <pthread.h>

bool media_available(void) { return true; }

typedef struct Sample {
    double pts, dts, dur;       /* seconds, timestampOffset applied */
    bool key;
    uint8_t *data;
    int size;
} Sample;

struct MediaBuffer {
    bool webm;
    MediaKind kind;

    uint8_t *init;              /* initialization segment */
    size_t init_len;
    uint8_t *pending;           /* incomplete trailing box / element */
    size_t pending_len;

    Sample *s;                  /* decode order */
    int n, cap;

    AVCodecParameters *par;
    double duration;
    int width, height;

    MediaRange *ranges;         /* cache, rebuilt when dirty */
    int nranges;
    bool ranges_dirty;

    /* Video decode, on the buffer's worker thread (started at the first
     * frame asked for).  It decodes the frame painting asks for and the
     * next few after it into slots, so that the next paint finds its frame
     * there; painting waits only for a frame not decoded yet.  mu guards
     * the samples (appends and removals happen on the JS thread, the
     * worker feeds the decoder from them), the slots and the request; the
     * decoder state is the worker's alone. */
    pthread_mutex_t mu;
    pthread_cond_t wake;        /* worker: a request, a reset, stop */
    pthread_cond_t ready;       /* painting: a frame published, or the worker gave up */
    pthread_t worker;
    bool worker_on, stop;
    bool reset;                 /* the samples changed: decode from a keyframe again */
    int64_t want;               /* us: the frame asked for (INT64_MIN = none) */
    bool want_failed;           /* the worker could not produce it */
    uint64_t gen;               /* bumped by each request */
    AVCodecContext *dec;
    int next_feed;              /* index of the next sample to send, -1 = reset */
    int64_t last_out_pts;       /* us, of the newest frame received */
    AVFrame *frame;
    struct SwsContext *sws;
    struct VFrame {
        uint8_t *rgba;
        int w, h;
        int64_t pts;            /* us, INT64_MIN = empty */
        uint64_t id;
    } slot[4];
    int shown;                  /* the slot painting holds (until its next call), -1 */
    uint64_t frame_id;
};
#define VSLOTS 4
#define VAHEAD 3    /* frames decoded past the one asked for */

#define US 1000000.0

/* ---- byte framing ---- */

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

/* One MP4 box at p[0..n): its size (0 = incomplete header), its type. */
static uint64_t mp4_box(const uint8_t *p, size_t n, uint32_t *type)
{
    if (n < 8) return 0;
    uint64_t size = be32(p);
    *type = be32(p + 4);
    if (size == 1) {
        if (n < 16) return 0;
        size = be64(p + 8);
    } else if (size == 0) {
        size = n;               /* to the end of the data */
    }
    return size < 8 ? 8 : size;
}

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

/* EBML variable-length integer: returns its length (0 = incomplete/bad).
 * keep_marker leaves the length marker in (element IDs). */
static int ebml_vint(const uint8_t *p, size_t n, uint64_t *v, bool keep_marker, bool *unknown)
{
    if (n < 1 || p[0] == 0) return 0;
    int len = 1;
    while (!(p[0] & (0x80 >> (len - 1)))) len++;
    if ((size_t)len > n) return 0;
    uint64_t x = keep_marker ? p[0] : (p[0] & (0xFF >> len));
    bool all_ones = (p[0] & (0xFF >> len)) == (0xFF >> len);
    for (int i = 1; i < len; i++) {
        x = x << 8 | p[i];
        if (p[i] != 0xFF) all_ones = false;
    }
    if (unknown) *unknown = !keep_marker && all_ones;
    *v = x;
    return len;
}

/* One EBML element header: total header length (0 = incomplete). */
static int ebml_elem(const uint8_t *p, size_t n, uint64_t *id, uint64_t *size, bool *unknown)
{
    int a = ebml_vint(p, n, id, true, NULL);
    if (!a) return 0;
    int b = ebml_vint(p + a, n - (size_t)a, size, false, unknown);
    if (!b) return 0;
    return a + b;
}

#define EBML_HEADER 0x1A45DFA3u
#define EBML_SEGMENT 0x18538067u
#define EBML_CLUSTER 0x1F43B675u
#define EBML_TRACKS 0x1654AE6Bu

/* Split buf[0..n) into an initialization segment (*init_end > 0 when one
 * is complete) and the complete media data [*media_beg, *media_end).
 * Bytes from *media_end on are kept for the next append. */
static void frame_mp4(const uint8_t *buf, size_t n, size_t *init_end,
                      size_t *media_beg, size_t *media_end)
{
    *init_end = 0;
    *media_beg = *media_end = 0;
    size_t off = 0;
    bool in_media = false;
    while (off < n) {
        uint32_t type = 0;
        uint64_t size = mp4_box(buf + off, n - off, &type);
        if (!size || off + size > n) break;
        size_t end = off + (size_t)size;
        if (type == FOURCC('m', 'o', 'o', 'v')) {
            /* Everything up to here (ftyp, ...) is initialization. */
            *init_end = end;
            in_media = false;
            *media_beg = *media_end = 0;
        } else if (type != FOURCC('f', 't', 'y', 'p')) {
            if (!in_media) { *media_beg = off; in_media = true; }
            /* A fragment is usable once its mdat is complete. */
            if (type == FOURCC('m', 'd', 'a', 't')) *media_end = end;
        }
        off = end;
    }
    if (!*media_end) *media_beg = 0;
}

static void frame_webm(const uint8_t *buf, size_t n, size_t *init_end,
                       size_t *media_beg, size_t *media_end)
{
    *init_end = 0;
    *media_beg = *media_end = 0;
    size_t off = 0;
    uint64_t id, size;
    bool unknown;
    if (n >= 4 && be32(buf) == EBML_HEADER) {
        int h = ebml_elem(buf, n, &id, &size, &unknown);
        if (!h || off + h + size > n) return;
        off += h + (size_t)size;
        h = ebml_elem(buf + off, n - off, &id, &size, &unknown);
        if (!h || id != EBML_SEGMENT) return;
        off += h;                               /* into the Segment */
        bool tracks = false;
        while (off < n) {
            h = ebml_elem(buf + off, n - off, &id, &size, &unknown);
            if (!h) return;
            if (id == EBML_CLUSTER) break;
            if (unknown || off + h + size > n) return;
            if (id == EBML_TRACKS) tracks = true;
            off += h + (size_t)size;
        }
        if (!tracks) return;
        *init_end = off;
    }
    size_t beg = off;
    while (off < n) {
        int h = ebml_elem(buf + off, n - off, &id, &size, &unknown);
        if (!h) break;
        size_t end = unknown ? n : off + h + (size_t)size;
        if (end > n) break;
        if (id == EBML_CLUSTER) { *media_beg = beg; *media_end = end; }
        off = end;
    }
}

/* ---- demux ---- */

typedef struct {
    const uint8_t *a; size_t alen;      /* init segment */
    const uint8_t *b; size_t blen;      /* media data */
    int64_t pos;
} MemReader;

static int mem_read(void *opaque, uint8_t *out, int len)
{
    MemReader *r = opaque;
    int64_t total = (int64_t)(r->alen + r->blen);
    if (r->pos >= total) return AVERROR_EOF;
    int done = 0;
    while (done < len && r->pos < total) {
        const uint8_t *src;
        size_t avail;
        if (r->pos < (int64_t)r->alen) { src = r->a + r->pos; avail = r->alen - (size_t)r->pos; }
        else { src = r->b + (r->pos - (int64_t)r->alen); avail = (size_t)(total - r->pos); }
        size_t k = avail < (size_t)(len - done) ? avail : (size_t)(len - done);
        memcpy(out + done, src, k);
        done += (int)k;
        r->pos += (int64_t)k;
    }
    return done;
}

static int64_t mem_seek(void *opaque, int64_t off, int whence)
{
    MemReader *r = opaque;
    int64_t total = (int64_t)(r->alen + r->blen);
    if (whence & AVSEEK_SIZE) return total;
    whence &= ~AVSEEK_FORCE;
    int64_t p = whence == SEEK_SET ? off : whence == SEEK_CUR ? r->pos + off : total + off;
    if (p < 0 || p > total) return -1;
    r->pos = p;
    return p;
}

/* Open the concatenation init + media; NULL on failure. */
static AVFormatContext *open_demux(MediaBuffer *mb, MemReader *rd)
{
    const int iosz = 64 * 1024;
    uint8_t *iobuf = av_malloc(iosz);
    if (!iobuf) return NULL;
    AVIOContext *io = avio_alloc_context(iobuf, iosz, 0, rd, mem_read, NULL, mem_seek);
    if (!io) { av_free(iobuf); return NULL; }
    AVFormatContext *fmt = avformat_alloc_context();
    if (!fmt) { av_free(io->buffer); avio_context_free(&io); return NULL; }
    fmt->pb = io;
    fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    const AVInputFormat *ifmt = av_find_input_format(mb->webm ? "matroska" : "mov");
    if (avformat_open_input(&fmt, NULL, ifmt, NULL) < 0) {
        /* avformat_open_input frees fmt on failure, not the custom IO. */
        av_free(io->buffer);
        avio_context_free(&io);
        return NULL;
    }
    return fmt;
}

static void close_demux(AVFormatContext *fmt)
{
    if (!fmt) return;
    AVIOContext *io = fmt->pb;
    avformat_close_input(&fmt);
    if (io) { av_free(io->buffer); avio_context_free(&io); }
}

/* The stream this buffer carries: the first audio or video stream. */
static int pick_stream(AVFormatContext *fmt)
{
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        enum AVMediaType t = fmt->streams[i]->codecpar->codec_type;
        if (t == AVMEDIA_TYPE_VIDEO || t == AVMEDIA_TYPE_AUDIO) return (int)i;
    }
    return -1;
}

/* Codec parameters, kind, size and duration from an opened demuxer. */
static void take_stream_info(MediaBuffer *mb, AVFormatContext *fmt, int si)
{
    AVStream *st = fmt->streams[si];
    if (!mb->par) mb->par = avcodec_parameters_alloc();
    if (mb->par) avcodec_parameters_copy(mb->par, st->codecpar);
    mb->kind = st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ? MEDIA_KIND_VIDEO : MEDIA_KIND_AUDIO;
    mb->width = st->codecpar->width;
    mb->height = st->codecpar->height;
    if (fmt->duration != AV_NOPTS_VALUE && fmt->duration > 0)
        mb->duration = (double)fmt->duration / AV_TIME_BASE;
    else if (st->duration != AV_NOPTS_VALUE && st->duration > 0)
        mb->duration = (double)st->duration * av_q2d(st->time_base);
}

/* The initialization segment alone (matroska may want a cluster before it
 * opens; then the first media segment's demux provides the parameters). */
static void read_init(MediaBuffer *mb)
{
    MemReader rd = { mb->init, mb->init_len, NULL, 0, 0 };
    AVFormatContext *fmt = open_demux(mb, &rd);
    if (!fmt) return;
    int si = pick_stream(fmt);
    if (si >= 0) take_stream_info(mb, fmt, si);
    close_demux(fmt);
}

/* The samples changed (JS thread, mu held): the worker starts again from a
 * keyframe. */
static void decoder_reset(MediaBuffer *mb) { mb->reset = true; }

static int sample_dts_cmp(const void *a, const void *b)
{
    double x = ((const Sample *)a)->dts, y = ((const Sample *)b)->dts;
    return x < y ? -1 : x > y;
}

/* Replace what the new samples cover (coded frame removal) and add them. */
static void add_samples(MediaBuffer *mb, Sample *ns, int nn)
{
    if (nn <= 0) return;
    double lo = ns[0].pts, hi = ns[0].pts + ns[0].dur;
    for (int i = 1; i < nn; i++) {
        if (ns[i].pts < lo) lo = ns[i].pts;
        if (ns[i].pts + ns[i].dur > hi) hi = ns[i].pts + ns[i].dur;
    }
    int w = 0;
    for (int i = 0; i < mb->n; i++) {
        if (mb->s[i].pts >= lo - 1e-6 && mb->s[i].pts < hi - 1e-6) { free(mb->s[i].data); continue; }
        mb->s[w++] = mb->s[i];
    }
    bool removed = w != mb->n;
    mb->n = w;
    if (mb->n + nn > mb->cap) {
        int nc = mb->cap ? mb->cap : 256;
        while (nc < mb->n + nn) nc *= 2;
        Sample *s = realloc(mb->s, (size_t)nc * sizeof(Sample));
        if (!s) { for (int i = 0; i < nn; i++) free(ns[i].data); return; }
        mb->s = s;
        mb->cap = nc;
    }
    bool in_order = mb->n == 0 || mb->s[mb->n - 1].dts <= ns[0].dts;
    memcpy(mb->s + mb->n, ns, (size_t)nn * sizeof(Sample));
    mb->n += nn;
    if (!in_order) qsort(mb->s, (size_t)mb->n, sizeof(Sample), sample_dts_cmp);
    if (removed || !in_order) decoder_reset(mb);
    mb->ranges_dirty = true;
}

static bool demux_media(MediaBuffer *mb, const uint8_t *data, size_t len, double offset)
{
    MemReader rd = { mb->init, mb->init_len, data, len, 0 };
    AVFormatContext *fmt = open_demux(mb, &rd);
    if (!fmt) return false;
    int si = pick_stream(fmt);
    if (si < 0) { close_demux(fmt); return false; }
    if (!mb->par) take_stream_info(mb, fmt, si);
    AVRational tb = fmt->streams[si]->time_base;
    double fallback_dur = mb->kind == MEDIA_KIND_VIDEO ? 1.0 / 30.0 : 0.02;

    Sample *ns = NULL;
    int nn = 0, ncap = 0;
    AVPacket *pkt = av_packet_alloc();
    while (pkt && av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == si && pkt->size > 0) {
            if (nn == ncap) {
                ncap = ncap ? ncap * 2 : 128;
                Sample *t = realloc(ns, (size_t)ncap * sizeof(Sample));
                if (!t) { av_packet_unref(pkt); break; }
                ns = t;
            }
            int64_t pts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
            int64_t dts = pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pts;
            Sample *s = &ns[nn];
            s->pts = (double)pts * av_q2d(tb) + offset;
            s->dts = (double)dts * av_q2d(tb) + offset;
            s->dur = pkt->duration > 0 ? (double)pkt->duration * av_q2d(tb) : 0.0;
            s->key = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
            s->data = malloc((size_t)pkt->size + AV_INPUT_BUFFER_PADDING_SIZE);
            if (s->data) {
                memcpy(s->data, pkt->data, (size_t)pkt->size);
                memset(s->data + pkt->size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
                s->size = pkt->size;
                nn++;
            }
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    close_demux(fmt);
    /* Durations the container left out: the gap to the next sample. */
    for (int i = 0; i < nn; i++) {
        if (ns[i].dur > 0.0) continue;
        double next = 1e300;
        for (int j = 0; j < nn; j++)
            if (ns[j].pts > ns[i].pts + 1e-9 && ns[j].pts < next) next = ns[j].pts;
        ns[i].dur = next < 1e299 && next - ns[i].pts < 1.0 ? next - ns[i].pts : fallback_dur;
    }
    pthread_mutex_lock(&mb->mu);
    add_samples(mb, ns, nn);
    pthread_mutex_unlock(&mb->mu);
    free(ns);
    return true;
}

/* ---- public ---- */

MediaBuffer *media_buffer_create(const char *mime)
{
    MediaBuffer *mb = calloc(1, sizeof(*mb));
    if (!mb) return NULL;
    mb->webm = mime && str_has_ci(mime, "webm");
    if (mime && strncasecmp(mime, "video", 5) == 0) mb->kind = MEDIA_KIND_VIDEO;
    else if (mime && strncasecmp(mime, "audio", 5) == 0) mb->kind = MEDIA_KIND_AUDIO;
    mb->next_feed = -1;
    pthread_mutex_init(&mb->mu, NULL);
    pthread_cond_init(&mb->wake, NULL);
    pthread_cond_init(&mb->ready, NULL);
    mb->want = INT64_MIN;
    mb->shown = -1;
    for (int i = 0; i < VSLOTS; i++) mb->slot[i].pts = INT64_MIN;
    return mb;
}

static void decoder_free(MediaBuffer *mb)
{
    if (mb->dec) avcodec_free_context(&mb->dec);
    if (mb->frame) av_frame_free(&mb->frame);
    if (mb->sws) { sws_freeContext(mb->sws); mb->sws = NULL; }
    mb->next_feed = -1;
}

/* Stop the worker and wait for it (JS thread): the decoder is then free to
 * change, and the slots to go. */
static void worker_stop(MediaBuffer *mb)
{
    if (!mb->worker_on) return;
    pthread_mutex_lock(&mb->mu);
    mb->stop = true;
    pthread_cond_broadcast(&mb->wake);
    pthread_mutex_unlock(&mb->mu);
    pthread_join(mb->worker, NULL);
    mb->worker_on = false;
    mb->stop = false;
}

static void slots_clear(MediaBuffer *mb)
{
    for (int i = 0; i < VSLOTS; i++) mb->slot[i].pts = INT64_MIN;
    mb->shown = -1;
}

void media_buffer_destroy(MediaBuffer *mb)
{
    if (!mb) return;
    worker_stop(mb);
    for (int i = 0; i < mb->n; i++) free(mb->s[i].data);
    free(mb->s);
    free(mb->init);
    free(mb->pending);
    free(mb->ranges);
    for (int i = 0; i < VSLOTS; i++) free(mb->slot[i].rgba);
    decoder_free(mb);
    pthread_mutex_destroy(&mb->mu);
    pthread_cond_destroy(&mb->wake);
    pthread_cond_destroy(&mb->ready);
    if (mb->par) avcodec_parameters_free(&mb->par);
    free(mb);
}

bool media_buffer_append(MediaBuffer *mb, const uint8_t *data, size_t len, double offset)
{
    if (!mb || !data || !len) return true;
    uint8_t *buf = realloc(mb->pending, mb->pending_len + len);
    if (!buf) return false;
    memcpy(buf + mb->pending_len, data, len);
    size_t n = mb->pending_len + len;
    mb->pending = NULL;
    mb->pending_len = 0;

    size_t init_end, mbeg, mend;
    if (mb->webm) frame_webm(buf, n, &init_end, &mbeg, &mend);
    else frame_mp4(buf, n, &init_end, &mbeg, &mend);

    bool ok = true;
    if (init_end) {
        free(mb->init);
        mb->init = malloc(init_end);
        if (mb->init) {
            memcpy(mb->init, buf, init_end);
            mb->init_len = init_end;
            /* a new stream: the worker's decoder and frames go */
            worker_stop(mb);
            decoder_free(mb);
            slots_clear(mb);
            read_init(mb);
        }
    }
    size_t keep_from = mend ? mend : init_end;
    if (mend > mbeg && mb->init) ok = demux_media(mb, buf + mbeg, mend - mbeg, offset);
    if (keep_from < n) {
        mb->pending_len = n - keep_from;
        mb->pending = malloc(mb->pending_len);
        if (mb->pending) memcpy(mb->pending, buf + keep_from, mb->pending_len);
        else mb->pending_len = 0;
    }
    free(buf);
    return ok;
}

void media_buffer_reset_parser(MediaBuffer *mb)
{
    if (!mb) return;
    free(mb->pending);
    mb->pending = NULL;
    mb->pending_len = 0;
}

void media_buffer_remove(MediaBuffer *mb, double start, double end)
{
    if (!mb || end <= start) return;
    pthread_mutex_lock(&mb->mu);
    int w = 0;
    for (int i = 0; i < mb->n; i++) {
        if (mb->s[i].pts >= start && mb->s[i].pts < end) { free(mb->s[i].data); continue; }
        mb->s[w++] = mb->s[i];
    }
    if (w != mb->n) {
        mb->n = w;
        mb->ranges_dirty = true;
        decoder_reset(mb);
    }
    pthread_mutex_unlock(&mb->mu);
}

static int range_cmp(const void *a, const void *b)
{
    double x = ((const MediaRange *)a)->start, y = ((const MediaRange *)b)->start;
    return x < y ? -1 : x > y;
}

static void rebuild_ranges(MediaBuffer *mb)
{
    free(mb->ranges);
    mb->ranges = NULL;
    mb->nranges = 0;
    mb->ranges_dirty = false;
    if (mb->n == 0) return;
    MediaRange *r = malloc((size_t)mb->n * sizeof(MediaRange));
    if (!r) return;
    for (int i = 0; i < mb->n; i++) { r[i].start = mb->s[i].pts; r[i].end = mb->s[i].pts + mb->s[i].dur; }
    qsort(r, (size_t)mb->n, sizeof(MediaRange), range_cmp);
    /* Samples closer than this join (frame-duration jitter, B-frame order). */
    const double gap = 0.1;
    int k = 0;
    for (int i = 1; i < mb->n; i++) {
        if (r[i].start <= r[k].end + gap) { if (r[i].end > r[k].end) r[k].end = r[i].end; }
        else r[++k] = r[i];
    }
    mb->nranges = k + 1;
    mb->ranges = r;
}

int media_buffer_ranges(MediaBuffer *mb, MediaRange *out, int max)
{
    if (!mb) return 0;
    if (mb->ranges_dirty) rebuild_ranges(mb);
    int n = mb->nranges < max ? mb->nranges : max;
    if (n > 0) memcpy(out, mb->ranges, (size_t)n * sizeof(MediaRange));
    return mb->nranges < max ? mb->nranges : max;
}

bool media_buffer_has_init(const MediaBuffer *mb) { return mb && mb->init && mb->par; }
MediaKind media_buffer_kind(const MediaBuffer *mb) { return mb ? mb->kind : MEDIA_KIND_UNKNOWN; }
double media_buffer_duration(const MediaBuffer *mb) { return mb ? mb->duration : 0.0; }
void media_buffer_video_size(const MediaBuffer *mb, int *w, int *h)
{
    *w = mb ? mb->width : 0;
    *h = mb ? mb->height : 0;
}

/* ---- video decode ---- */

static bool decoder_open(MediaBuffer *mb)
{
    if (mb->dec) return true;
    if (!mb->par) return false;
    const AVCodec *codec = avcodec_find_decoder(mb->par->codec_id);
    if (!codec) return false;
    mb->dec = avcodec_alloc_context3(codec);
    if (!mb->dec) return false;
    if (avcodec_parameters_to_context(mb->dec, mb->par) < 0) { decoder_free(mb); return false; }
    mb->dec->pkt_timebase = (AVRational){ 1, 1000000 };
    mb->dec->thread_count = 0;
    if (avcodec_open2(mb->dec, codec, NULL) < 0) { decoder_free(mb); return false; }
    mb->frame = av_frame_alloc();
    mb->next_feed = -1;
    return mb->frame != NULL;
}

/* f as RGBA into slot v (worker). */
static bool frame_to_rgba(MediaBuffer *mb, const AVFrame *f, struct VFrame *v)
{
    int w = f->width, h = f->height;
    if (w <= 0 || h <= 0) return false;
    mb->sws = sws_getCachedContext(mb->sws, w, h, (enum AVPixelFormat)f->format,
                                   w, h, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
    if (!mb->sws) return false;
    if (w != v->w || h != v->h || !v->rgba) {
        free(v->rgba);
        v->rgba = malloc((size_t)w * (size_t)h * 4);
        if (!v->rgba) { v->w = v->h = 0; return false; }
        v->w = w;
        v->h = h;
    }
    uint8_t *dst[4] = { v->rgba, NULL, NULL, NULL };
    int stride[4] = { w * 4, 0, 0, 0 };
    sws_scale(mb->sws, (const uint8_t *const *)f->data, f->linesize, 0, h, dst, stride);
    return true;
}

static int64_t frame_pts(const AVFrame *f)
{
    return f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
}

/* The pts (us) of the sample on screen at t: the latest one presented by
 * then, the first one when t precedes everything buffered; INT64_MIN when
 * there is none.  mu held. */
static int64_t sample_at(const MediaBuffer *mb, double t)
{
    int target = -1;
    for (int i = 0; i < mb->n; i++)
        if (mb->s[i].pts <= t + 1e-6 && (target < 0 || mb->s[i].pts > mb->s[target].pts)) target = i;
    if (target < 0)
        for (int i = 0; i < mb->n; i++) if (target < 0 || mb->s[i].pts < mb->s[target].pts) target = i;
    return target < 0 ? INT64_MIN : (int64_t)llround(mb->s[target].pts * US);
}

/* The slot holding the frame of pts want, or -1.  mu held. */
static int slot_of(const MediaBuffer *mb, int64_t want)
{
    for (int i = 0; i < VSLOTS; i++)
        if (mb->slot[i].pts != INT64_MIN && llabs(mb->slot[i].pts - want) <= 1000) return i;
    return -1;
}

/* A slot to decode into: an empty one, else the oldest frame that is not
 * shown and not the one asked for; -1 when the frames ahead fill them all.
 * mu held. */
static int slot_free(const MediaBuffer *mb)
{
    int best = -1;
    for (int i = 0; i < VSLOTS; i++) {
        if (i == mb->shown) continue;
        if (mb->slot[i].pts == INT64_MIN) return i;
        if (llabs(mb->slot[i].pts - mb->want) <= 1000) continue;
        if (mb->slot[i].pts > mb->want) continue;   /* ahead: still wanted */
        if (best < 0 || mb->slot[i].pts < mb->slot[best].pts) best = i;
    }
    return best;
}

/* Frames decoded past the one asked for, still in the slots.  mu held. */
static int slots_ahead(const MediaBuffer *mb)
{
    int n = 0;
    for (int i = 0; i < VSLOTS; i++)
        if (mb->slot[i].pts != INT64_MIN && mb->slot[i].pts > mb->want + 1000) n++;
    return n;
}

/* The worker: decode toward the frame asked for, then a few past it.  The
 * decoder is fed from the samples under mu (send_packet copies the data);
 * decoding and the RGBA conversion run unlocked. */
static void *worker_main(void *arg)
{
    MediaBuffer *mb = arg;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *tmp = av_frame_alloc(), *prev = av_frame_alloc();
    bool drained = false;       /* sent the end of data: no more packets until a flush */
    if (!pkt || !tmp || !prev) {
        pthread_mutex_lock(&mb->mu);
        mb->want_failed = true;
        pthread_cond_broadcast(&mb->ready);
        while (!mb->stop) pthread_cond_wait(&mb->wake, &mb->mu);
        pthread_mutex_unlock(&mb->mu);
        av_packet_free(&pkt);
        av_frame_free(&tmp);
        av_frame_free(&prev);
        return NULL;
    }
    pthread_mutex_lock(&mb->mu);
    while (!mb->stop) {
        int64_t want = mb->want;
        bool have = want != INT64_MIN && slot_of(mb, want) >= 0;
        if (want == INT64_MIN || mb->want_failed || (have && (slots_ahead(mb) >= VAHEAD || slot_free(mb) < 0))) {
            pthread_cond_wait(&mb->wake, &mb->mu);
            continue;
        }
        uint64_t gen = mb->gen;
        if (!have) {
            /* From the keyframe at or before the frame asked for, unless the
               decoder is already on the way there. */
            int key = 0, target = -1;
            for (int i = 0; i < mb->n; i++)
                if (llabs((int64_t)llround(mb->s[i].pts * US) - want) <= 1000) { target = i; break; }
            for (int i = 0; target >= 0 && i <= target; i++) if (mb->s[i].key) key = i;
            if (target < 0) { mb->want_failed = true; pthread_cond_broadcast(&mb->ready); continue; }
            if (mb->reset || mb->next_feed < 0 || mb->next_feed <= key || mb->last_out_pts > want) {
                mb->reset = false;
                pthread_mutex_unlock(&mb->mu);
                avcodec_flush_buffers(mb->dec);
                pthread_mutex_lock(&mb->mu);
                mb->next_feed = key;
                mb->last_out_pts = INT64_MIN;
                drained = false;
                av_frame_unref(prev);
            }
        } else if (mb->reset) {
            /* the samples changed under frames decoded ahead: start over at
               the next request */
            mb->reset = false;
            mb->next_feed = -1;
            continue;
        }
        /* One step: a frame out, else a packet in. */
        pthread_mutex_unlock(&mb->mu);
        int r = avcodec_receive_frame(mb->dec, tmp);
        pthread_mutex_lock(&mb->mu);
        if (r == 0) {
            int64_t fp = frame_pts(tmp);
            mb->last_out_pts = fp;
            if (fp < want - 1000) {
                /* before the frame asked for: kept (by reference) in case
                   the data ends before it, when it is what shows */
                if (gen == mb->gen) {
                    av_frame_unref(prev);
                    av_frame_move_ref(prev, tmp);
                } else {
                    av_frame_unref(tmp);
                }
                continue;
            }
            /* the frame asked for, or one past it */
            int k = slot_free(mb);
            if (k >= 0) {
                struct VFrame *v = &mb->slot[k];
                v->pts = INT64_MIN;     /* being written */
                pthread_mutex_unlock(&mb->mu);
                bool ok = frame_to_rgba(mb, tmp, v);
                pthread_mutex_lock(&mb->mu);
                if (ok) {
                    v->pts = fp;
                    v->id = ++mb->frame_id;
                    pthread_cond_broadcast(&mb->ready);
                }
            }
            av_frame_unref(tmp);
            continue;
        }
        if (r == AVERROR(EAGAIN) && !drained && mb->next_feed >= 0 && mb->next_feed < mb->n) {
            const Sample *sm = &mb->s[mb->next_feed++];
            pkt->data = sm->data;
            pkt->size = sm->size;
            pkt->pts = (int64_t)llround(sm->pts * US);
            pkt->dts = (int64_t)llround(sm->dts * US);
            pkt->flags = sm->key ? AV_PKT_FLAG_KEY : 0;
            avcodec_send_packet(mb->dec, pkt);  /* copies the data (unrefcounted packet) */
            continue;
        }
        if (r == AVERROR(EAGAIN) && !drained && mb->next_feed >= mb->n) {
            avcodec_send_packet(mb->dec, NULL);   /* no more data: drain */
            drained = true;
            continue;
        }
        /* end of data, or an error: the frames there are, no more (the next
           request flushes the decoder) */
        mb->next_feed = -1;
        drained = false;
        if (slot_of(mb, want) < 0) {
            /* the newest frame before it shows, as before the worker */
            int k = prev->buf[0] && gen == mb->gen ? slot_free(mb) : -1;
            if (k >= 0) {
                struct VFrame *v = &mb->slot[k];
                v->pts = INT64_MIN;
                pthread_mutex_unlock(&mb->mu);
                bool ok = frame_to_rgba(mb, prev, v);
                pthread_mutex_lock(&mb->mu);
                if (ok) {
                    v->pts = want;
                    v->id = ++mb->frame_id;
                }
            }
            av_frame_unref(prev);
            if (slot_of(mb, want) < 0) mb->want_failed = true;
            pthread_cond_broadcast(&mb->ready);
        } else {
            pthread_cond_wait(&mb->wake, &mb->mu);
        }
    }
    pthread_mutex_unlock(&mb->mu);
    av_packet_free(&pkt);
    av_frame_free(&tmp);
    av_frame_free(&prev);
    return NULL;
}

/* Ask the worker for the frame at t (starting it if needed); called with
 * mu held.  Returns the frame's pts, INT64_MIN if there is none. */
static int64_t request_frame(MediaBuffer *mb, double t)
{
    int64_t want = sample_at(mb, t);
    if (want == INT64_MIN) return INT64_MIN;
    if (want != mb->want) {
        mb->want = want;
        mb->want_failed = false;
        mb->gen++;
    }
    pthread_cond_broadcast(&mb->wake);
    return want;
}

static bool worker_start(MediaBuffer *mb)
{
    if (mb->worker_on) return true;
    if (!decoder_open(mb)) return false;
    if (pthread_create(&mb->worker, NULL, worker_main, mb) != 0) return false;
    mb->worker_on = true;
    return true;
}

void media_buffer_prefetch(MediaBuffer *mb, double t)
{
    if (!mb || mb->kind != MEDIA_KIND_VIDEO || mb->n == 0 || !worker_start(mb)) return;
    pthread_mutex_lock(&mb->mu);
    request_frame(mb, t);
    pthread_mutex_unlock(&mb->mu);
}

bool media_buffer_frame_at(MediaBuffer *mb, double t, const uint8_t **rgba, int *w, int *h,
                           uint64_t *frame_id)
{
    if (!mb || mb->kind != MEDIA_KIND_VIDEO || mb->n == 0 || !worker_start(mb)) return false;
    pthread_mutex_lock(&mb->mu);
    mb->shown = -1;
    int64_t want = request_frame(mb, t);
    if (want == INT64_MIN) { pthread_mutex_unlock(&mb->mu); return false; }
    /* Decoded ahead already, or wait for the worker. */
    int k;
    while ((k = slot_of(mb, want)) < 0 && !mb->want_failed)
        pthread_cond_wait(&mb->ready, &mb->mu);
    if (k >= 0) {
        mb->shown = k;
        *rgba = mb->slot[k].rgba;
        *w = mb->slot[k].w;
        *h = mb->slot[k].h;
        if (frame_id) *frame_id = mb->slot[k].id;
        pthread_cond_broadcast(&mb->wake);   /* it may decode further ahead now */
    }
    pthread_mutex_unlock(&mb->mu);
    return k >= 0;
}

#endif /* CYBER_HAVE_FFMPEG */

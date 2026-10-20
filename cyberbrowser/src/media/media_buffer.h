#ifndef MEDIA_BUFFER_H
#define MEDIA_BUFFER_H

/* One SourceBuffer's media.  MSE appends an initialization segment and then
 * media segments (fragmented MP4 or WebM); they are demuxed with ffmpeg into
 * timed samples, which give the buffered ranges and, for video, the frame
 * shown at a given time (decoded on demand).
 *
 * Built without ffmpeg (CYBER_HAVE_FFMPEG undefined) nothing is ever parsed:
 * appends succeed and nothing is buffered. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct MediaBuffer MediaBuffer;

typedef enum { MEDIA_KIND_UNKNOWN = 0, MEDIA_KIND_VIDEO, MEDIA_KIND_AUDIO } MediaKind;

typedef struct { double start, end; } MediaRange;

/* True when this build can demux and decode (it was linked with ffmpeg). */
bool media_available(void);

/* Whether a MIME type with codecs ("video/mp4; codecs=\"avc1.64001f\"") can
 * be played: the container and every codec named must be supported. */
bool media_type_supported(const char *mime_type);

MediaBuffer *media_buffer_create(const char *mime_type);
void media_buffer_destroy(MediaBuffer *mb);

/* Append bytes: an initialization segment, media segments, or part of one
 * (the incomplete tail waits for the next append).  timestamp_offset (s) is
 * added to the media timestamps.  False on bytes that cannot be parsed. */
bool media_buffer_append(MediaBuffer *mb, const uint8_t *data, size_t len, double timestamp_offset);

/* SourceBuffer.remove(): drop the samples presented in [start, end). */
void media_buffer_remove(MediaBuffer *mb, double start, double end);

/* SourceBuffer.abort(): forget a partially appended segment. */
void media_buffer_reset_parser(MediaBuffer *mb);

/* Buffered ranges in presentation order, merged; returns the count. */
int media_buffer_ranges(MediaBuffer *mb, MediaRange *out, int max);

bool media_buffer_has_init(const MediaBuffer *mb);
MediaKind media_buffer_kind(const MediaBuffer *mb);
/* Duration the initialization segment declares (s), 0 when unknown. */
double media_buffer_duration(const MediaBuffer *mb);
void media_buffer_video_size(const MediaBuffer *mb, int *w, int *h);

/* RGBA pixels (w*h*4, top-left origin) of the video frame shown at time t.
 * The pointer stays valid until the next call on this buffer.  *frame_id
 * changes whenever a different frame is returned. */
bool media_buffer_frame_at(MediaBuffer *mb, double t, const uint8_t **rgba, int *w, int *h,
                           uint64_t *frame_id);

#endif

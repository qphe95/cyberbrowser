#ifndef WEBGL_DECODE_H
#define WEBGL_DECODE_H

/* The web-GPU process's walk over a tab's WebGL records (SANDBOX_DESIGN.md
 * sections 5.7, 8.7.3), and the tables that map a tab's ids to GL names.
 * Pure C, no library calls, bounded loops: proved with Frama-C/WP
 * (tools/prove.sh webgl) free of undefined behavior for any bytes (P1) and
 * bounded (P7), and:
 *   - a record is accepted only if its op is known and its length matches
 *     that op's grammar (rec_ok below: the generated table for the plain
 *     calls, webgl_forms for the special ops and the queries);
 *   - what it hands the executor (webgl_backend.c, which calls ANGLE) lies
 *     inside the batch, with its counts checked against their caps;
 *   - the replies of a batch's queries add up to at most WEBGL_REPLY_MAX;
 *   - an id maps only to a name in its own context's table, and never
 *     indexes out of it.
 *
 * A batch is records [u32 op][u32 n][n bytes of arguments], little-endian,
 * back to back, filling it exactly. */

#include <stdbool.h>
#include <stdint.h>
#include "webgl_gen.h"

/* The kinds of objects a tab names by id. */
enum {
    WEBGL_BUFFER = 0, WEBGL_FRAMEBUFFER, WEBGL_RENDERBUFFER, WEBGL_TEXTURE, WEBGL_SHADER, WEBGL_PROGRAM,
    WEBGL_LOCATION, WEBGL_VERTEX_ARRAY, WEBGL_QUERY, WEBGL_SAMPLER, WEBGL_SYNC, WEBGL_TRANSFORM_FEEDBACK,
    WEBGL_KINDS
};

#define WEBGL_MAX_IDS 65536         /* per kind, per context */
#define WEBGL_NO_NAME 0x7fff0000u   /* a name GLES never gave: for an id that names nothing */

#define WEBGL_STR_MAX (4u << 20)            /* a source, a name, a list of names */
#define WEBGL_ANGLE_STR_MAX (64u << 10)     /* a string ANGLE answers with (GL_EXTENSIONS ...): cut there */
#define WEBGL_LOG_MAX (1u << 20)            /* an info log */
#define WEBGL_NAME_MAX 1024u                /* an active attribute's or uniform's name, with its NUL */
#define WEBGL_DATA_MAX (256u << 20)         /* readPixels, getBufferSubData */
#define WEBGL_REPLY_MAX (128u << 20)        /* a batch's replies, together */

/* The special ops (WEBGL_OP_SPECIAL and up), and the queries (300 and up),
 * which each append their answer to the reply. */
enum {
    OP_CREATE = 256, OP_DELETE, OP_SHADER_SOURCE, OP_BIND_ATTRIB_LOCATION, OP_BUFFER_DATA, OP_BUFFER_SUB_DATA,
    OP_TEX_IMAGE_2D, OP_TEX_SUB_IMAGE_2D, OP_COMPRESSED_TEX_IMAGE_2D, OP_COMPRESSED_TEX_SUB_IMAGE_2D,
    OP_UNIFORM_V, OP_VERTEX_ATTRIB_V, OP_TEX_IMAGE_3D, OP_TEX_SUB_IMAGE_3D, OP_DRAW_BUFFERS, OP_CLEAR_BUFFER_V,
    OP_INVALIDATE_FRAMEBUFFER, OP_TRANSFORM_FEEDBACK_VARYINGS, OP_PIXEL_STOREI, OP_FENCE_SYNC, OP_WAIT_SYNC,
    OP_GET_ERROR = 300, OP_GET_INTEGER, OP_GET_FLOAT, OP_GET_BOOLEAN, OP_GET_STRING, OP_GET_SHADER_PARAM,
    OP_GET_SHADER_INFO_LOG, OP_GET_PROGRAM_PARAM, OP_GET_PROGRAM_INFO_LOG, OP_GET_UNIFORM_LOCATION,
    OP_GET_ATTRIB_LOCATION, OP_GET_ACTIVE_ATTRIB, OP_GET_ACTIVE_UNIFORM, OP_READ_PIXELS, OP_CHECK_FRAMEBUFFER_STATUS,
    OP_IS_OBJECT, OP_GET_TEX_PARAM, OP_GET_BUFFER_PARAM, OP_GET_RENDERBUFFER_PARAM, OP_GET_FRAMEBUFFER_ATTACHMENT_PARAM,
    OP_GET_SHADER_PRECISION, OP_GET_UNIFORM, OP_GET_VERTEX_ATTRIB, OP_GET_VERTEX_ATTRIB_OFFSET, OP_GET_QUERY_PARAM,
    OP_GET_SAMPLER_PARAM, OP_GET_SYNC_PARAM, OP_CLIENT_WAIT_SYNC, OP_GET_BUFFER_SUB_DATA, OP_GET_INDEXED,
    OP_GET_FRAG_DATA_LOCATION, OP_GET_UNIFORM_BLOCK_INDEX, OP_GET_ACTIVE_UNIFORM_BLOCK_PARAM,
    OP_GET_ACTIVE_UNIFORM_BLOCK_NAME, OP_GET_UNIFORM_INDICES, OP_GET_ACTIVE_UNIFORMS, OP_GET_TRANSFORM_FEEDBACK_VARYING,
    OP_GET_INTERNALFORMAT_PARAM, OP_REQUEST_EXTENSION, OP_GET_EXTENSIONS, OP_GET_RESET_STATUS,
    OP_END
};

/* How a special op's arguments are laid out: `head` bytes of fixed words,
 * then (by form) nothing, or a tail.  `at` is the offset of the word the
 * form reads (a count, a flag, a kind); a count is at most `cap`.  Its reply
 * is at most `reply` + `per` * count bytes. */
enum {
    F_NONE = 0,     /* not an op */
    F_EXACT,        /* n == head */
    F_DATA,         /* head, then bytes */
    F_STR,          /* head, then a string: at most WEBGL_STR_MAX bytes, no NUL */
    F_NAMES,        /* head, then names separated by NULs (count: how many), at most WEBGL_STR_MAX bytes */
    F_WORDS,        /* head, then count u32s */
    F_COUNTED,      /* n == head; count is the word at `at` */
    F_HAS,          /* head (a flag at `at`), then bytes, only if the flag is set */
    F_BUFDATA,      /* F_HAS, and with data, the i64 at 8 is its length */
    F_UNIFORM,      /* head (a kind 1..21 at `at`), then count values of webgl_uniform_per[kind] words */
    F_ATTRIB        /* head (a kind 1..6 at `at`), then webgl_attrib_per[kind] words (count) */
};

typedef struct WebglForm {
    uint8_t form, head, at, per;
    uint32_t cap, reply;
} WebglForm;

extern const WebglForm webgl_forms[OP_END - WEBGL_OP_SPECIAL];
extern const uint8_t webgl_plain_len[WEBGL_PLAIN_OPS + 1];
extern const uint8_t webgl_uniform_per[22];
extern const uint8_t webgl_attrib_per[7];

/* One record, checked: its arguments are recs[at .. at + n), the first
 * head bytes of them fixed words (all of them for a plain call); count as
 * its form says; its reply adds at most `reply` bytes. */
typedef struct WebglRec {
    uint32_t op, at, n, head, count, reply;
} WebglRec;

/*@ logic integer u32_at{L}(uint8_t *p, integer k) =
        p[k] + p[k + 1] * 256 + p[k + 2] * 65536 + p[k + 3] * 16777216;

    predicate no_nul{L}(uint8_t *s, integer k) = \forall integer i; 0 <= i < k ==> s[i] != 0;

    // A special op's or a query's grammar, for its form f.
    predicate form_ok{L}(uint8_t *a, WebglForm f, integer n, integer count) =
        f.form != F_NONE && f.head <= n &&
        (f.form == F_EXACT ==> n == f.head && count == 0) &&
        (f.form == F_DATA ==> count == 0) &&
        (f.form == F_STR ==> n - f.head <= WEBGL_STR_MAX && no_nul(a + f.head, n - f.head) && count == 0) &&
        (f.form == F_NAMES ==> n - f.head <= WEBGL_STR_MAX && count <= f.cap) &&
        (f.form == F_WORDS ==> (n - f.head) % 4 == 0 && count == (n - f.head) / 4 && count <= f.cap) &&
        (f.form == F_COUNTED ==> n == f.head && f.at + 4 <= f.head && count == u32_at(a, f.at) && count <= f.cap) &&
        (f.form == F_HAS ==> f.at + 4 <= f.head && (u32_at(a, f.at) == 0 ==> n == f.head) && count == 0) &&
        (f.form == F_BUFDATA ==> f.at + 4 <= f.head && 16 <= f.head && count == 0 &&
            (u32_at(a, f.at) == 0 ==> n == f.head) &&
            (u32_at(a, f.at) != 0 ==> u32_at(a, 8) == n - f.head && u32_at(a, 12) == 0)) &&
        (f.form == F_UNIFORM ==> f.at + 4 <= f.head && 1 <= u32_at(a, f.at) <= 21 &&
            count * 4 * webgl_uniform_per[u32_at(a, f.at)] == n - f.head) &&
        (f.form == F_ATTRIB ==> f.at + 4 <= f.head && 1 <= u32_at(a, f.at) <= 6 &&
            count == webgl_attrib_per[u32_at(a, f.at)] && n == f.head + 4 * count);

    // A record's grammar: a plain call's length is the generated table's.
    predicate rec_ok{L}(uint8_t *a, integer op, integer n, integer head, integer count) =
        (1 <= op <= WEBGL_PLAIN_OPS && n == webgl_plain_len[op] && head == n && count == 0) ||
        (WEBGL_OP_SPECIAL <= op < OP_END && form_ok(a, webgl_forms[op - WEBGL_OP_SPECIAL], n, count) &&
         head == webgl_forms[op - WEBGL_OP_SPECIAL].head);
 */

/* The record at `at` of a batch of len bytes, when the replies before it
 * take at most `replied` bytes: 1 (in *r; the next is at r->at + r->n), 0
 * at the end of the batch, -1 if the rest is malformed (an unknown op, a
 * length its grammar doesn't allow, a count over its cap, replies over
 * WEBGL_REPLY_MAX, or a record cut short). */
/*@ requires \valid_read(recs + (0 .. len - 1)) && \valid(r);
    requires \separated(r, recs + (0 .. len - 1));
    requires at <= len && replied <= WEBGL_REPLY_MAX;
    assigns *r;
    ensures \result == 0 || \result == 1 || \result == -1;
    ensures \result == 0 <==> at == len;
    ensures \result == 1 ==>
        r->at == at + 8 && r->n <= len - r->at && r->head <= r->n &&
        r->op == u32_at(recs, at) && r->n == u32_at(recs, at + 4) &&
        r->reply <= WEBGL_REPLY_MAX - replied &&
        rec_ok(recs + r->at, r->op, r->n, r->head, r->count);
 */
int webgl_next(const uint8_t *recs, uint32_t len, uint32_t at, uint32_t replied, WebglRec *r);

/* ---- ids ------------------------------------------------------------------------------ */

typedef struct WebglLocation {
    uint32_t program;               /* the GL program it was asked of (0: none) */
    int32_t location;
} WebglLocation;

/* The GL name a tab's id of a kind maps to in a context whose tables are
 * names[0 .. WEBGL_KINDS) (each WEBGL_MAX_IDS entries, or NULL if none made
 * yet): 0 for id 0 ("none"), the name the context's table holds for it, or
 * WEBGL_NO_NAME if it names nothing there (out of range, never made,
 * another context's). */
/*@ requires \valid_read(names + (0 .. WEBGL_KINDS - 1));
    requires \forall integer k; 0 <= k < WEBGL_KINDS ==>
        names[k] == \null || \valid_read(names[k] + (0 .. WEBGL_MAX_IDS - 1));
    assigns \nothing;
    ensures id == 0 ==> \result == 0;
    ensures id != 0 ==> \result == WEBGL_NO_NAME ||
        (kind < WEBGL_KINDS && id < WEBGL_MAX_IDS && names[kind] != \null &&
         \result == names[kind][id] && \result != 0);
 */
uint32_t webgl_name_of(uint32_t *const *names, uint32_t kind, uint32_t id);

/* Whether a tab's id may index a table (1 .. WEBGL_MAX_IDS - 1). */
/*@ assigns \nothing;
    ensures \result <==> 0 < id < WEBGL_MAX_IDS;
 */
bool webgl_id_ok(uint32_t id);

/* A uniform location id's location in a context's table (WEBGL_MAX_IDS
 * entries, or NULL), or -1 if it names none. */
/*@ requires locs == \null || \valid_read(locs + (0 .. WEBGL_MAX_IDS - 1));
    assigns \nothing;
    ensures \result == -1 ||
        (locs != \null && 0 < id < WEBGL_MAX_IDS && locs[id].program != 0 && \result == locs[id].location);
 */
int32_t webgl_location_of(const WebglLocation *locs, uint32_t id);

#endif

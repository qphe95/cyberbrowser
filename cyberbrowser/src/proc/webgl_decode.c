/* webgl_decode.c - the walk over a tab's WebGL records, and its id tables
 * (webgl_decode.h).  No library calls, bounded loops: proved with
 * Frama-C/WP (tools/prove.sh webgl).  Integers are read byte by byte,
 * little-endian, with arithmetic the provers handle. */
#include "webgl_decode.h"

/* The plain calls' argument lengths, from tools/gen_webgl.js's table (the
 * same one the tab's encoder and webgl_exec_plain are made from). */
const uint8_t webgl_plain_len[WEBGL_PLAIN_OPS + 1] = WEBGL_PLAIN_LENS;

/* OP_UNIFORM_V's kinds: 1..4 float vectors, 5..8 int, 9..12 uint, 13..21
 * matrices (2, 3, 4, 2x3, 3x2, 2x4, 4x2, 3x4, 4x3); words per value. */
const uint8_t webgl_uniform_per[22] = { 0, 1, 2, 3, 4, 1, 2, 3, 4, 1, 2, 3, 4, 4, 9, 16, 6, 6, 8, 8, 12, 12 };
/* OP_VERTEX_ATTRIB_V's kinds: 1..4 floats, 5 int4, 6 uint4. */
const uint8_t webgl_attrib_per[7] = { 0, 1, 2, 3, 4, 4, 4 };

/* The grammar of each special op and query.  The executor reads its fixed
 * words at offsets below `head` (the layouts are in run_special's
 * comments), and appends at most `reply` + `per` * count bytes. */
#define EXACT(h, reply) { F_EXACT, h, 0, 0, 0, reply }
#define DATA(h) { F_DATA, h, 0, 0, 0, 0 }
#define STR(h, reply) { F_STR, h, 0, 0, 0, reply }
#define NAMES(h, cap, per, reply) { F_NAMES, h, 0, per, cap, reply }
#define WORDS(h, cap, per) { F_WORDS, h, 0, per, cap, 0 }
#define COUNTED(h, at, cap, per, reply) { F_COUNTED, h, at, per, cap, reply }
#define HAS(h, at) { F_HAS, h, at, 0, 0, 0 }
const WebglForm webgl_forms[OP_END - WEBGL_OP_SPECIAL] = {
    [OP_CREATE - WEBGL_OP_SPECIAL] = EXACT(12, 0),                     /* kind, id, shader type */
    [OP_DELETE - WEBGL_OP_SPECIAL] = EXACT(8, 0),                      /* kind, id */
    [OP_SHADER_SOURCE - WEBGL_OP_SPECIAL] = STR(4, 0),                 /* shader; source */
    [OP_BIND_ATTRIB_LOCATION - WEBGL_OP_SPECIAL] = STR(8, 0),          /* program, index; name */
    [OP_BUFFER_DATA - WEBGL_OP_SPECIAL] = { F_BUFDATA, 20, 16, 0, 0, 0 },  /* target, usage, size (i64), has; data */
    [OP_BUFFER_SUB_DATA - WEBGL_OP_SPECIAL] = DATA(12),                /* target, offset (i64); data */
    [OP_TEX_IMAGE_2D - WEBGL_OP_SPECIAL] = HAS(36, 32),                /* target, level, internal, w, h, border, format, type, has */
    [OP_TEX_SUB_IMAGE_2D - WEBGL_OP_SPECIAL] = DATA(32),               /* target, level, x, y, w, h, format, type */
    [OP_COMPRESSED_TEX_IMAGE_2D - WEBGL_OP_SPECIAL] = DATA(24),        /* target, level, internal, w, h, border */
    [OP_COMPRESSED_TEX_SUB_IMAGE_2D - WEBGL_OP_SPECIAL] = DATA(28),    /* target, level, x, y, w, h, format */
    [OP_UNIFORM_V - WEBGL_OP_SPECIAL] = { F_UNIFORM, 12, 4, 0, 0, 0 },     /* location, kind, transpose; values */
    [OP_VERTEX_ATTRIB_V - WEBGL_OP_SPECIAL] = { F_ATTRIB, 8, 4, 0, 0, 0 }, /* index, kind; values */
    [OP_TEX_IMAGE_3D - WEBGL_OP_SPECIAL] = HAS(40, 36),                /* target, level, internal, w, h, d, border, format, type, has */
    [OP_TEX_SUB_IMAGE_3D - WEBGL_OP_SPECIAL] = DATA(40),               /* target, level, x, y, z, w, h, d, format, type */
    [OP_DRAW_BUFFERS - WEBGL_OP_SPECIAL] = WORDS(0, 16, 0),            /* buffers */
    [OP_CLEAR_BUFFER_V - WEBGL_OP_SPECIAL] = EXACT(28, 0),             /* buffer, drawbuffer, kind, 4 values */
    [OP_INVALIDATE_FRAMEBUFFER - WEBGL_OP_SPECIAL] = WORDS(4, 16, 0),  /* target; attachments */
    [OP_TRANSFORM_FEEDBACK_VARYINGS - WEBGL_OP_SPECIAL] = NAMES(8, 64, 0, 0),  /* program, mode; names */
    [OP_PIXEL_STOREI - WEBGL_OP_SPECIAL] = EXACT(8, 0),                /* pname, value */
    [OP_FENCE_SYNC - WEBGL_OP_SPECIAL] = EXACT(12, 0),                 /* id, condition, flags */
    [OP_WAIT_SYNC - WEBGL_OP_SPECIAL] = EXACT(16, 0),                  /* id, flags, timeout (u64) */

    [OP_GET_ERROR - WEBGL_OP_SPECIAL] = EXACT(0, 4),
    [OP_GET_INTEGER - WEBGL_OP_SPECIAL] = COUNTED(8, 4, 16, 4, 0),     /* pname, count */
    [OP_GET_FLOAT - WEBGL_OP_SPECIAL] = COUNTED(8, 4, 16, 4, 0),
    [OP_GET_BOOLEAN - WEBGL_OP_SPECIAL] = COUNTED(8, 4, 16, 4, 0),
    [OP_GET_STRING - WEBGL_OP_SPECIAL] = EXACT(4, 4 + WEBGL_ANGLE_STR_MAX),    /* pname */
    [OP_GET_SHADER_PARAM - WEBGL_OP_SPECIAL] = EXACT(8, 4),            /* shader, pname */
    [OP_GET_SHADER_INFO_LOG - WEBGL_OP_SPECIAL] = EXACT(4, 4 + WEBGL_LOG_MAX),
    [OP_GET_PROGRAM_PARAM - WEBGL_OP_SPECIAL] = EXACT(8, 4),
    [OP_GET_PROGRAM_INFO_LOG - WEBGL_OP_SPECIAL] = EXACT(4, 4 + WEBGL_LOG_MAX),
    [OP_GET_UNIFORM_LOCATION - WEBGL_OP_SPECIAL] = STR(8, 4),          /* program, the id to give it; name */
    [OP_GET_ATTRIB_LOCATION - WEBGL_OP_SPECIAL] = STR(4, 4),           /* program; name */
    [OP_GET_ACTIVE_ATTRIB - WEBGL_OP_SPECIAL] = EXACT(8, 12 + WEBGL_NAME_MAX), /* program, index */
    [OP_GET_ACTIVE_UNIFORM - WEBGL_OP_SPECIAL] = EXACT(8, 12 + WEBGL_NAME_MAX),
    [OP_READ_PIXELS - WEBGL_OP_SPECIAL] = COUNTED(28, 24, WEBGL_DATA_MAX, 1, 4),   /* x, y, w, h, format, type, bytes */
    [OP_CHECK_FRAMEBUFFER_STATUS - WEBGL_OP_SPECIAL] = EXACT(4, 4),    /* target */
    [OP_IS_OBJECT - WEBGL_OP_SPECIAL] = EXACT(8, 4),                   /* kind, id */
    [OP_GET_TEX_PARAM - WEBGL_OP_SPECIAL] = EXACT(12, 4),              /* target, pname, float? */
    [OP_GET_BUFFER_PARAM - WEBGL_OP_SPECIAL] = EXACT(12, 4),
    [OP_GET_RENDERBUFFER_PARAM - WEBGL_OP_SPECIAL] = EXACT(12, 4),
    [OP_GET_FRAMEBUFFER_ATTACHMENT_PARAM - WEBGL_OP_SPECIAL] = EXACT(12, 4),   /* target, attachment, pname */
    [OP_GET_SHADER_PRECISION - WEBGL_OP_SPECIAL] = EXACT(8, 12),       /* shader type, precision type */
    [OP_GET_UNIFORM - WEBGL_OP_SPECIAL] = COUNTED(16, 12, 16, 4, 0),   /* program, location, as, count */
    [OP_GET_VERTEX_ATTRIB - WEBGL_OP_SPECIAL] = COUNTED(16, 12, 4, 4, 0),  /* index, pname, as, count */
    [OP_GET_VERTEX_ATTRIB_OFFSET - WEBGL_OP_SPECIAL] = EXACT(8, 8),    /* index, pname */
    [OP_GET_QUERY_PARAM - WEBGL_OP_SPECIAL] = EXACT(8, 4),             /* query, pname */
    [OP_GET_SAMPLER_PARAM - WEBGL_OP_SPECIAL] = EXACT(12, 4),          /* sampler, pname, float? */
    [OP_GET_SYNC_PARAM - WEBGL_OP_SPECIAL] = EXACT(8, 4),              /* sync, pname */
    [OP_CLIENT_WAIT_SYNC - WEBGL_OP_SPECIAL] = EXACT(16, 4),           /* sync, flags, timeout */
    [OP_GET_BUFFER_SUB_DATA - WEBGL_OP_SPECIAL] = COUNTED(16, 12, WEBGL_DATA_MAX, 1, 4),  /* target, offset (i64), length */
    [OP_GET_INDEXED - WEBGL_OP_SPECIAL] = EXACT(12, 8),                /* target, index, 64? */
    [OP_GET_FRAG_DATA_LOCATION - WEBGL_OP_SPECIAL] = STR(4, 4),        /* program; name */
    [OP_GET_UNIFORM_BLOCK_INDEX - WEBGL_OP_SPECIAL] = STR(4, 4),       /* program; name */
    [OP_GET_ACTIVE_UNIFORM_BLOCK_PARAM - WEBGL_OP_SPECIAL] = COUNTED(16, 12, 256, 4, 0),  /* program, index, pname, count */
    [OP_GET_ACTIVE_UNIFORM_BLOCK_NAME - WEBGL_OP_SPECIAL] = EXACT(8, 4 + WEBGL_NAME_MAX),  /* program, index */
    [OP_GET_UNIFORM_INDICES - WEBGL_OP_SPECIAL] = NAMES(4, 256, 4, 4), /* program; names */
    [OP_GET_ACTIVE_UNIFORMS - WEBGL_OP_SPECIAL] = WORDS(8, 256, 4),    /* program, pname; indices */
    [OP_GET_TRANSFORM_FEEDBACK_VARYING - WEBGL_OP_SPECIAL] = EXACT(8, 12 + WEBGL_NAME_MAX),
    [OP_GET_INTERNALFORMAT_PARAM - WEBGL_OP_SPECIAL] = COUNTED(16, 12, 64, 4, 0),  /* target, internal, pname, count */
    [OP_REQUEST_EXTENSION - WEBGL_OP_SPECIAL] = STR(0, 4),             /* name */
    [OP_GET_EXTENSIONS - WEBGL_OP_SPECIAL] = EXACT(0, 8 + 2 * WEBGL_ANGLE_STR_MAX),
    [OP_GET_RESET_STATUS - WEBGL_OP_SPECIAL] = EXACT(0, 4),
};

/*@ requires \valid_read(p + (0 .. 3));
    assigns \nothing;
    ensures \result == u32_at(p, 0);
 */
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] + (uint32_t)p[1] * 0x100u + (uint32_t)p[2] * 0x10000u + (uint32_t)p[3] * 0x1000000u;
}

/*@ requires \valid_read(s + (0 .. k - 1));
    assigns \nothing;
    ensures \result <==> no_nul(s, k);
 */
static bool nul_free(const uint8_t *s, uint32_t k) {
    /*@ loop invariant 0 <= i <= k;
        loop invariant no_nul(s, i);
        loop assigns i;
        loop variant k - i;
     */
    for (uint32_t i = 0; i < k; i++)
        if (s[i] == 0) return false;
    return true;
}

/* How many names k bytes separated by NULs hold, as the executor splits
 * them: one, and another after each NUL but a last one. */
/*@ requires \valid_read(s + (0 .. k - 1));
    assigns \nothing;
    ensures \result <= k;
 */
static uint32_t names_in(const uint8_t *s, uint32_t k) {
    if (k == 0) return 0;
    uint32_t c = 1;
    /*@ loop invariant 0 <= i <= k - 1;
        loop invariant 1 <= c <= i + 1;
        loop assigns i, c;
        loop variant k - 1 - i;
     */
    for (uint32_t i = 0; i + 1 < k; i++)
        if (s[i] == 0) c++;
    return c;
}

int webgl_next(const uint8_t *recs, uint32_t len, uint32_t at, uint32_t replied, WebglRec *r) {
    if (at == len) return 0;
    if (len - at < 8) return -1;
    uint32_t op = rd32(recs + at), n = rd32(recs + at + 4);
    if (n > len - at - 8) return -1;
    const uint8_t *a = recs + at + 8;
    r->op = op;
    r->at = at + 8;
    r->n = n;
    r->head = n;
    r->count = 0;
    r->reply = 0;
    if (op >= 1 && op <= WEBGL_PLAIN_OPS) return n == webgl_plain_len[op] ? 1 : -1;
    if (op < WEBGL_OP_SPECIAL || op >= OP_END) return -1;
    WebglForm f = webgl_forms[op - WEBGL_OP_SPECIAL];
    if (f.form == F_NONE || n < f.head) return -1;
    r->head = f.head;
    uint32_t tail = n - f.head, count = 0, word = 0;
    if (f.form >= F_COUNTED) {              /* the forms that read a word of the head */
        if ((uint32_t)f.at + 4u > f.head) return -1;
        word = rd32(a + f.at);
    }
    switch (f.form) {
    case F_EXACT:
        if (tail) return -1;
        break;
    case F_DATA:
        break;
    case F_STR:
        if (tail > WEBGL_STR_MAX || !nul_free(a + f.head, tail)) return -1;
        break;
    case F_NAMES:
        if (tail > WEBGL_STR_MAX) return -1;
        count = names_in(a + f.head, tail);
        if (count > f.cap) return -1;
        break;
    case F_WORDS:
        if (tail % 4u) return -1;
        count = tail / 4u;
        if (count > f.cap) return -1;
        break;
    case F_COUNTED:
        if (tail || word > f.cap) return -1;
        count = word;
        break;
    case F_HAS:
        if (!word && tail) return -1;
        break;
    case F_BUFDATA:
        if (f.head < 16) return -1;
        if (!word ? tail != 0 : rd32(a + 8) != tail || rd32(a + 12) != 0) return -1;
        break;
    case F_UNIFORM: {
        if (word < 1 || word > 21) return -1;
        uint32_t unit = 4u * webgl_uniform_per[word];
        if (unit == 0 || tail % unit) return -1;
        count = tail / unit;
        /*@ assert count * unit == tail; */
        break;
    }
    case F_ATTRIB:
        if (word < 1 || word > 6) return -1;
        count = webgl_attrib_per[word];
        if (tail != 4u * count) return -1;
        break;
    default:
        return -1;
    }
    uint64_t reply = (uint64_t)f.reply + (uint64_t)f.per * count;
    if (reply > (uint64_t)(WEBGL_REPLY_MAX - replied)) return -1;
    r->count = count;
    r->reply = (uint32_t)reply;
    return 1;
}

/* ---- ids ------------------------------------------------------------------------------ */

uint32_t webgl_name_of(uint32_t *const *names, uint32_t kind, uint32_t id) {
    if (id == 0) return 0;
    if (kind >= (uint32_t)WEBGL_KINDS || id >= WEBGL_MAX_IDS || !names[kind]) return WEBGL_NO_NAME;
    uint32_t v = names[kind][id];
    return v ? v : WEBGL_NO_NAME;
}

bool webgl_id_ok(uint32_t id) {
    return id > 0 && id < WEBGL_MAX_IDS;
}

int32_t webgl_location_of(const WebglLocation *locs, uint32_t id) {
    if (!locs || !webgl_id_ok(id) || !locs[id].program) return -1;
    return locs[id].location;
}

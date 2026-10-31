/*
 * encoding.c - TextEncoder and TextDecoder (Encoding Standard 7).
 *
 * TextEncoder writes UTF-8 (an unpaired surrogate as U+FFFD).  TextDecoder
 * reads UTF-8, UTF-16LE, UTF-16BE, windows-1252 (which latin1, ascii and
 * iso-8859-1 name too) and x-user-defined, with fatal, ignoreBOM and
 * streaming; any other label is a RangeError, the legacy multi-byte
 * encodings included.
 *
 * A decoder keeps its state between decode(..., {stream: true}) calls: the
 * UTF-8 decoder's bytes needed/seen, code point and bounds (8.1.1), or the
 * UTF-16 decoder's lead byte and lead surrogate (14.2.1).
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <quickjs.h>
#include <quickjs_gc_unified.h>
#include "browser_api_impl.h"
#include "browser_api_impl_internal.h"

enum { ENC_UTF8, ENC_UTF16LE, ENC_UTF16BE, ENC_1252, ENC_USER };

static const char *const g_enc_names[] = { "utf-8", "utf-16le", "utf-16be", "windows-1252", "x-user-defined" };

/* Every label and the encoding it names (Encoding Standard 4.2), by label. */
static const struct { const char *label, *name; } g_labels[] = {
    { "866", "IBM866" }, { "ansi_x3.4-1968", "windows-1252" }, { "arabic", "ISO-8859-6" },
    { "ascii", "windows-1252" }, { "asmo-708", "ISO-8859-6" }, { "big5", "Big5" }, { "big5-hkscs", "Big5" },
    { "chinese", "GBK" }, { "cn-big5", "Big5" }, { "cp1250", "windows-1250" }, { "cp1251", "windows-1251" },
    { "cp1252", "windows-1252" }, { "cp1253", "windows-1253" }, { "cp1254", "windows-1254" },
    { "cp1255", "windows-1255" }, { "cp1256", "windows-1256" }, { "cp1257", "windows-1257" },
    { "cp1258", "windows-1258" }, { "cp819", "windows-1252" }, { "cp866", "IBM866" }, { "csbig5", "Big5" },
    { "cseuckr", "EUC-KR" }, { "cseucpkdfmtjapanese", "EUC-JP" }, { "csgb2312", "GBK" }, { "csibm866", "IBM866" },
    { "csiso2022jp", "ISO-2022-JP" }, { "csiso2022kr", "replacement" }, { "csiso58gb231280", "GBK" },
    { "csiso88596e", "ISO-8859-6" }, { "csiso88596i", "ISO-8859-6" }, { "csiso88598e", "ISO-8859-8" },
    { "csiso88598i", "ISO-8859-8-I" }, { "csisolatin1", "windows-1252" }, { "csisolatin2", "ISO-8859-2" },
    { "csisolatin3", "ISO-8859-3" }, { "csisolatin4", "ISO-8859-4" }, { "csisolatin5", "windows-1254" },
    { "csisolatin6", "ISO-8859-10" }, { "csisolatin9", "ISO-8859-15" }, { "csisolatinarabic", "ISO-8859-6" },
    { "csisolatincyrillic", "ISO-8859-5" }, { "csisolatingreek", "ISO-8859-7" },
    { "csisolatinhebrew", "ISO-8859-8" }, { "cskoi8r", "KOI8-R" }, { "csksc56011987", "EUC-KR" },
    { "csmacintosh", "macintosh" }, { "csshiftjis", "Shift_JIS" }, { "csunicode", "UTF-16LE" },
    { "cyrillic", "ISO-8859-5" }, { "dos-874", "windows-874" }, { "ecma-114", "ISO-8859-6" },
    { "ecma-118", "ISO-8859-7" }, { "elot_928", "ISO-8859-7" }, { "euc-jp", "EUC-JP" }, { "euc-kr", "EUC-KR" },
    { "gb18030", "gb18030" }, { "gb2312", "GBK" }, { "gb_2312", "GBK" }, { "gb_2312-80", "GBK" }, { "gbk", "GBK" },
    { "greek", "ISO-8859-7" }, { "greek8", "ISO-8859-7" }, { "hebrew", "ISO-8859-8" },
    { "hz-gb-2312", "replacement" }, { "ibm819", "windows-1252" }, { "ibm866", "IBM866" },
    { "iso-10646-ucs-2", "UTF-16LE" }, { "iso-2022-cn", "replacement" }, { "iso-2022-cn-ext", "replacement" },
    { "iso-2022-jp", "ISO-2022-JP" }, { "iso-2022-kr", "replacement" }, { "iso-8859-1", "windows-1252" },
    { "iso-8859-10", "ISO-8859-10" }, { "iso-8859-11", "windows-874" }, { "iso-8859-13", "ISO-8859-13" },
    { "iso-8859-14", "ISO-8859-14" }, { "iso-8859-15", "ISO-8859-15" }, { "iso-8859-16", "ISO-8859-16" },
    { "iso-8859-2", "ISO-8859-2" }, { "iso-8859-3", "ISO-8859-3" }, { "iso-8859-4", "ISO-8859-4" },
    { "iso-8859-5", "ISO-8859-5" }, { "iso-8859-6", "ISO-8859-6" }, { "iso-8859-6-e", "ISO-8859-6" },
    { "iso-8859-6-i", "ISO-8859-6" }, { "iso-8859-7", "ISO-8859-7" }, { "iso-8859-8", "ISO-8859-8" },
    { "iso-8859-8-e", "ISO-8859-8" }, { "iso-8859-8-i", "ISO-8859-8-I" }, { "iso-8859-9", "windows-1254" },
    { "iso-ir-100", "windows-1252" }, { "iso-ir-101", "ISO-8859-2" }, { "iso-ir-109", "ISO-8859-3" },
    { "iso-ir-110", "ISO-8859-4" }, { "iso-ir-126", "ISO-8859-7" }, { "iso-ir-127", "ISO-8859-6" },
    { "iso-ir-138", "ISO-8859-8" }, { "iso-ir-144", "ISO-8859-5" }, { "iso-ir-148", "windows-1254" },
    { "iso-ir-149", "EUC-KR" }, { "iso-ir-157", "ISO-8859-10" }, { "iso-ir-58", "GBK" },
    { "iso8859-1", "windows-1252" }, { "iso8859-10", "ISO-8859-10" }, { "iso8859-11", "windows-874" },
    { "iso8859-13", "ISO-8859-13" }, { "iso8859-14", "ISO-8859-14" }, { "iso8859-15", "ISO-8859-15" },
    { "iso8859-2", "ISO-8859-2" }, { "iso8859-3", "ISO-8859-3" }, { "iso8859-4", "ISO-8859-4" },
    { "iso8859-5", "ISO-8859-5" }, { "iso8859-6", "ISO-8859-6" }, { "iso8859-7", "ISO-8859-7" },
    { "iso8859-8", "ISO-8859-8" }, { "iso8859-9", "windows-1254" }, { "iso88591", "windows-1252" },
    { "iso885910", "ISO-8859-10" }, { "iso885911", "windows-874" }, { "iso885913", "ISO-8859-13" },
    { "iso885914", "ISO-8859-14" }, { "iso885915", "ISO-8859-15" }, { "iso88592", "ISO-8859-2" },
    { "iso88593", "ISO-8859-3" }, { "iso88594", "ISO-8859-4" }, { "iso88595", "ISO-8859-5" },
    { "iso88596", "ISO-8859-6" }, { "iso88597", "ISO-8859-7" }, { "iso88598", "ISO-8859-8" },
    { "iso88599", "windows-1254" }, { "iso_8859-1", "windows-1252" }, { "iso_8859-15", "ISO-8859-15" },
    { "iso_8859-1:1987", "windows-1252" }, { "iso_8859-2", "ISO-8859-2" }, { "iso_8859-2:1987", "ISO-8859-2" },
    { "iso_8859-3", "ISO-8859-3" }, { "iso_8859-3:1988", "ISO-8859-3" }, { "iso_8859-4", "ISO-8859-4" },
    { "iso_8859-4:1988", "ISO-8859-4" }, { "iso_8859-5", "ISO-8859-5" }, { "iso_8859-5:1988", "ISO-8859-5" },
    { "iso_8859-6", "ISO-8859-6" }, { "iso_8859-6:1987", "ISO-8859-6" }, { "iso_8859-7", "ISO-8859-7" },
    { "iso_8859-7:1987", "ISO-8859-7" }, { "iso_8859-8", "ISO-8859-8" }, { "iso_8859-8:1988", "ISO-8859-8" },
    { "iso_8859-9", "windows-1254" }, { "iso_8859-9:1989", "windows-1254" }, { "koi", "KOI8-R" },
    { "koi8", "KOI8-R" }, { "koi8-r", "KOI8-R" }, { "koi8-ru", "KOI8-U" }, { "koi8-u", "KOI8-U" },
    { "koi8_r", "KOI8-R" }, { "korean", "EUC-KR" }, { "ks_c_5601-1987", "EUC-KR" }, { "ks_c_5601-1989", "EUC-KR" },
    { "ksc5601", "EUC-KR" }, { "ksc_5601", "EUC-KR" }, { "l1", "windows-1252" }, { "l2", "ISO-8859-2" },
    { "l3", "ISO-8859-3" }, { "l4", "ISO-8859-4" }, { "l5", "windows-1254" }, { "l6", "ISO-8859-10" },
    { "l9", "ISO-8859-15" }, { "latin1", "windows-1252" }, { "latin2", "ISO-8859-2" }, { "latin3", "ISO-8859-3" },
    { "latin4", "ISO-8859-4" }, { "latin5", "windows-1254" }, { "latin6", "ISO-8859-10" },
    { "logical", "ISO-8859-8-I" }, { "mac", "macintosh" }, { "macintosh", "macintosh" }, { "ms932", "Shift_JIS" },
    { "ms_kanji", "Shift_JIS" }, { "replacement", "replacement" }, { "shift-jis", "Shift_JIS" },
    { "shift_jis", "Shift_JIS" }, { "sjis", "Shift_JIS" }, { "sun_eu_greek", "ISO-8859-7" },
    { "tis-620", "windows-874" }, { "ucs-2", "UTF-16LE" }, { "unicode", "UTF-16LE" },
    { "unicode-1-1-utf-8", "UTF-8" }, { "unicode11utf8", "UTF-8" }, { "unicode20utf8", "UTF-8" },
    { "unicodefeff", "UTF-16LE" }, { "unicodefffe", "UTF-16BE" }, { "us-ascii", "windows-1252" },
    { "utf-16", "UTF-16LE" }, { "utf-16be", "UTF-16BE" }, { "utf-16le", "UTF-16LE" }, { "utf-8", "UTF-8" },
    { "utf8", "UTF-8" }, { "visual", "ISO-8859-8" }, { "windows-1250", "windows-1250" },
    { "windows-1251", "windows-1251" }, { "windows-1252", "windows-1252" }, { "windows-1253", "windows-1253" },
    { "windows-1254", "windows-1254" }, { "windows-1255", "windows-1255" }, { "windows-1256", "windows-1256" },
    { "windows-1257", "windows-1257" }, { "windows-1258", "windows-1258" }, { "windows-31j", "Shift_JIS" },
    { "windows-874", "windows-874" }, { "windows-949", "EUC-KR" }, { "x-cp1250", "windows-1250" },
    { "x-cp1251", "windows-1251" }, { "x-cp1252", "windows-1252" }, { "x-cp1253", "windows-1253" },
    { "x-cp1254", "windows-1254" }, { "x-cp1255", "windows-1255" }, { "x-cp1256", "windows-1256" },
    { "x-cp1257", "windows-1257" }, { "x-cp1258", "windows-1258" }, { "x-euc-jp", "EUC-JP" }, { "x-gbk", "GBK" },
    { "x-mac-cyrillic", "x-mac-cyrillic" }, { "x-mac-roman", "macintosh" }, { "x-mac-ukrainian", "x-mac-cyrillic" },
    { "x-sjis", "Shift_JIS" }, { "x-unicode20utf8", "UTF-8" }, { "x-user-defined", "x-user-defined" },
    { "x-x-big5", "Big5" },
};

const char *encoding_name_for_label(const char *s, size_t n) {
    while (n && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\f' || *s == '\r')) s++, n--;
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' || s[n - 1] == '\f' || s[n - 1] == '\r')) n--;
    char low[32];
    if (n == 0 || n >= sizeof low) return NULL;
    for (size_t i = 0; i < n; i++) low[i] = (char)(s[i] >= 'A' && s[i] <= 'Z' ? s[i] + 32 : s[i]);
    low[n] = 0;
    size_t lo = 0, hi = sizeof g_labels / sizeof g_labels[0];
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(low, g_labels[mid].label);
        if (!c) return g_labels[mid].name;
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return NULL;
}

/* windows-1252's 0x80..0x9F (its index; the rest are Latin-1). */
static const uint16_t g_1252[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

/* TextDecoder's decoder for a label (ASCII whitespace around it and case
 * aside), or -1: the label names no encoding, or one there is no decoder for
 * here (the legacy multi-byte ones, replacement). */
static int encoding_of(const char *s, size_t n) {
    const char *name = encoding_name_for_label(s, n);
    if (!name) return -1;
    if (!strcmp(name, "UTF-8")) return ENC_UTF8;
    if (!strcmp(name, "UTF-16LE")) return ENC_UTF16LE;
    if (!strcmp(name, "UTF-16BE")) return ENC_UTF16BE;
    if (!strcmp(name, "windows-1252")) return ENC_1252;
    if (!strcmp(name, "x-user-defined")) return ENC_USER;
    return -1;
}

/* ---- objects ---- */

typedef struct Dec {
    uint8_t enc;
    bool fatal, ignore_bom, bom_seen, do_not_flush;
    uint8_t needed, seen, lower, upper;     /* UTF-8 */
    uint32_t cp;
    int lead_byte;                          /* UTF-16: -1 for none */
    uint16_t lead_surrogate;                /*   0 for none */
} Dec;

typedef struct Cell { Dec *d; } Cell;

static JSClassID g_enc_class, g_dec_class;

static void dec_finalizer(JSRuntimeHandle rt, GCValue v) {
    (void)rt;
    GCHandle h = JS_GetOpaqueHandle(v, g_dec_class);
    Cell *c = h != GC_HANDLE_NULL ? (Cell *)gc_deref(h) : NULL;
    if (c) { free(c->d); c->d = NULL; }
}
static void dec_mark(JSRuntimeHandle rt, GCValue v, JS_MarkFunc *m) {
    GCHandle h = JS_GetOpaqueHandle(v, g_dec_class);
    if (h != GC_HANDLE_NULL) m(rt, h);
}
static JSClassDef g_enc_def = { .class_name = "TextEncoder" };
static JSClassDef g_dec_def = { .class_name = "TextDecoder", .finalizer = dec_finalizer, .gc_mark = dec_mark };

static Dec *dec_of(JSContextHandle ctx, GCValue obj) {
    GCHandle h = JS_GetOpaqueHandle2(ctx, obj, g_dec_class);
    Cell *c = h != GC_HANDLE_NULL ? (Cell *)gc_deref(h) : NULL;
    return c ? c->d : NULL;
}

static GCValue illegal(JSContextHandle ctx) { return JS_ThrowTypeError(ctx, "Illegal invocation"); }

/* For a constructor: new.target's prototype (subclasses), else the class's. */
static GCValue ctor_proto(JSContextHandle ctx, GCValue new_target, const char *name) {
    GCValue p = JS_IsObject(new_target) ? JS_GetPropertyStr(ctx, new_target, "prototype") : JS_UNDEFINED;
    if (JS_IsObject(p)) return p;
    GCValue c = JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), name);
    return JS_IsObject(c) ? JS_GetPropertyStr(ctx, c, "prototype") : JS_UNDEFINED;
}

/* A dictionary argument's member as a boolean (undefined and null: no
 * dictionary); -1 after an exception. */
static int dict_bool(JSContextHandle ctx, int argc, GCValue *argv, int i, const char *member, const char *type) {
    if (argc <= i || JS_IsUndefined(argv[i]) || JS_IsNull(argv[i])) return 0;
    if (!JS_IsObject(argv[i])) {
        JS_ThrowTypeError(ctx, "The provided value is not of type '%s'.", type);
        return -1;
    }
    GCValue v = JS_GetPropertyStr(ctx, argv[i], member);
    if (JS_IsException(v)) return -1;
    return JS_ToBool(ctx, v);
}

/* ---- TextEncoder ---- */

static GCValue js_encoder_ctor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    if (JS_IsUndefined(new_target))
        return JS_ThrowTypeError(ctx, "Failed to construct 'TextEncoder': Please use the 'new' operator");
    GCValue proto = ctor_proto(ctx, new_target, "TextEncoder");
    return JS_IsObject(proto) ? JS_NewObjectProtoClass(ctx, proto, g_enc_class) : JS_NewObjectClass(ctx, g_enc_class);
}

static bool is_encoder(GCValue v) { return JS_IsObject(v) && JS_GetClassID(v) == g_enc_class; }

static GCValue js_encoder_encoding(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    if (!is_encoder(this_val)) return illegal(ctx);
    return JS_NewString(ctx, "utf-8");
}

/* The string's UTF-8, an unpaired surrogate as U+FFFD (malloc'd; *n bytes). */
static char *utf8_of(JSContextHandle ctx, GCValue s, size_t *n) {
    size_t len = 0;
    const char *p = JS_ToCStringLen2(ctx, &len, s, 0);
    if (!p) return NULL;
    char *u = malloc(len + 1);
    if (!u) { JS_ThrowOutOfMemory(ctx); return NULL; }
    memcpy(u, p, len);
    u[len] = 0;
    /* (the engine writes an unpaired surrogate as its three bytes: ED A0..BF xx) */
    for (size_t i = 0; i + 2 < len; i++)
        if ((uint8_t)u[i] == 0xED && (uint8_t)u[i + 1] >= 0xA0) {
            u[i] = (char)0xEF; u[i + 1] = (char)0xBF; u[i + 2] = (char)0xBD;
            i += 2;
        }
    *n = len;
    return u;
}

static GCValue new_uint8_array(JSContextHandle ctx, const uint8_t *p, size_t n) {
    GCValue ab = JS_NewArrayBufferCopy(ctx, p, n);
    if (JS_IsException(ab)) return ab;
    GCValue args[3] = { ab, JS_UNDEFINED, JS_UNDEFINED };    /* (it reads all three) */
    return JS_NewTypedArray(ctx, 3, args, JS_TYPED_ARRAY_UINT8);
}

static GCValue js_encoder_encode(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (!is_encoder(this_val)) return illegal(ctx);
    size_t n = 0;
    char *u = argc > 0 && !JS_IsUndefined(argv[0]) ? utf8_of(ctx, argv[0], &n) : calloc(1, 1);
    if (!u) return JS_EXCEPTION;
    GCValue r = new_uint8_array(ctx, (const uint8_t *)u, n);
    free(u);
    return r;
}

/* encodeInto(source, destination): as many whole characters as fit. */
static GCValue js_encoder_encode_into(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (!is_encoder(this_val)) return illegal(ctx);
    if (argc < 2) return JS_ThrowTypeError(ctx, "Failed to execute 'encodeInto' on 'TextEncoder': 2 arguments required.");
    if (JS_GetTypedArrayKind(argv[1]) != JS_TYPED_ARRAY_UINT8)
        return JS_ThrowTypeError(ctx, "Failed to execute 'encodeInto' on 'TextEncoder': parameter 2 is not of type 'Uint8Array'.");
    size_t n = 0;
    char *u = utf8_of(ctx, argv[0], &n);
    if (!u) return JS_EXCEPTION;
    size_t cap = 0;
    uint8_t *dst = (uint8_t *)JS_GetBufferSourceBytes(ctx, argv[1], &cap);
    size_t i = 0, read = 0;
    while (i < n) {
        uint8_t c = (uint8_t)u[i];
        size_t l = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        if (i + l > cap) break;
        memcpy(dst + i, u + i, l);
        i += l;
        read += l == 4 ? 2 : 1;
    }
    free(u);
    GCValue r = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, r, "read", JS_NewInt64(ctx, (int64_t)read));
    JS_SetPropertyStr(ctx, r, "written", JS_NewInt64(ctx, (int64_t)i));
    return r;
}

/* ---- TextDecoder ---- */

static void dec_reset(Dec *d) {
    d->needed = d->seen = 0;
    d->cp = 0;
    d->lower = 0x80;
    d->upper = 0xBF;
    d->lead_byte = -1;
    d->lead_surrogate = 0;
}

static GCValue js_decoder_ctor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    if (JS_IsUndefined(new_target))
        return JS_ThrowTypeError(ctx, "Failed to construct 'TextDecoder': Please use the 'new' operator");
    int enc = ENC_UTF8;
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        size_t n = 0;
        const char *label = JS_ToCStringLen(ctx, &n, argv[0]);
        if (!label) return JS_EXCEPTION;
        enc = encoding_of(label, n);
        if (enc < 0)
            return JS_ThrowRangeError(ctx, "Failed to construct 'TextDecoder': The encoding label provided ('%.64s') is invalid.",
                                      label);
    }
    int fatal = dict_bool(ctx, argc, argv, 1, "fatal", "TextDecoderOptions");
    if (fatal < 0) return JS_EXCEPTION;
    int ignore_bom = dict_bool(ctx, argc, argv, 1, "ignoreBOM", "TextDecoderOptions");
    if (ignore_bom < 0) return JS_EXCEPTION;
    GCValue proto = ctor_proto(ctx, new_target, "TextDecoder");
    GCValue obj = JS_IsObject(proto) ? JS_NewObjectProtoClass(ctx, proto, g_dec_class) : JS_NewObjectClass(ctx, g_dec_class);
    if (JS_IsException(obj)) return obj;
    Dec *d = calloc(1, sizeof *d);
    GCHandle h = d ? gc_allocz(sizeof(Cell), JS_GC_OBJ_TYPE_DATA) : GC_HANDLE_NULL;
    if (h == GC_HANDLE_NULL) { free(d); return JS_ThrowOutOfMemory(ctx); }
    d->enc = (uint8_t)enc;
    d->fatal = fatal;
    d->ignore_bom = ignore_bom;
    dec_reset(d);
    ((Cell *)gc_deref(h))->d = d;
    JS_SetOpaqueHandle(obj, h);
    return obj;
}

static GCValue js_decoder_get(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv, int magic) {
    (void)argc; (void)argv;
    Dec *d = dec_of(ctx, this_val);
    if (!d) return illegal(ctx);
    switch (magic) {
    case 0: return JS_NewString(ctx, g_enc_names[d->enc]);
    case 1: return JS_NewBool(ctx, d->fatal);
    default: return JS_NewBool(ctx, d->ignore_bom);
    }
}

typedef struct Out { uint16_t *p; size_t n; } Out;

/* A code point out (the first, if a BOM and not to be kept, dropped). */
static void put(Dec *d, Out *o, uint32_t cp) {
    if (!d->bom_seen) {
        d->bom_seen = true;
        if (cp == 0xFEFF && !d->ignore_bom) return;
    }
    if (cp > 0xFFFF) {
        cp -= 0x10000;
        o->p[o->n++] = (uint16_t)(0xD800 | (cp >> 10));
        o->p[o->n++] = (uint16_t)(0xDC00 | (cp & 0x3FF));
    } else {
        o->p[o->n++] = (uint16_t)cp;
    }
}

/* An error: false if fatal, else U+FFFD out. */
#define ERR() do { if (d->fatal) { dec_reset(d); return false; } put(d, o, 0xFFFD); } while (0)

static bool run_utf8(Dec *d, const uint8_t *s, size_t n, bool flush, Out *o) {
    size_t i = 0;
    while (i < n) {
        uint8_t b = s[i];
        if (!d->needed) {
            i++;
            if (b < 0x80) put(d, o, b);
            else if (b >= 0xC2 && b <= 0xDF) { d->needed = 1; d->cp = b & 0x1F; }
            else if (b >= 0xE0 && b <= 0xEF) {
                if (b == 0xE0) d->lower = 0xA0;
                if (b == 0xED) d->upper = 0x9F;
                d->needed = 2;
                d->cp = b & 0xF;
            } else if (b >= 0xF0 && b <= 0xF4) {
                if (b == 0xF0) d->lower = 0x90;
                if (b == 0xF4) d->upper = 0x8F;
                d->needed = 3;
                d->cp = b & 7;
            } else ERR();
            continue;
        }
        if (b < d->lower || b > d->upper) {
            dec_reset(d);
            ERR();              /* (and b again, as a first byte) */
            continue;
        }
        i++;
        d->lower = 0x80;
        d->upper = 0xBF;
        d->cp = (d->cp << 6) | (b & 0x3F);
        if (++d->seen == d->needed) {
            uint32_t cp = d->cp;
            dec_reset(d);
            put(d, o, cp);
        }
    }
    if (flush && d->needed) {
        dec_reset(d);
        ERR();
    }
    return true;
}

static bool run_utf16(Dec *d, const uint8_t *s, size_t n, bool flush, Out *o) {
    bool be = d->enc == ENC_UTF16BE;
    for (size_t i = 0; i < n; i++) {
        if (d->lead_byte < 0) { d->lead_byte = s[i]; continue; }
        uint16_t u = be ? (uint16_t)((d->lead_byte << 8) | s[i]) : (uint16_t)((s[i] << 8) | d->lead_byte);
        d->lead_byte = -1;
        if (d->lead_surrogate) {
            uint16_t lead = d->lead_surrogate;
            d->lead_surrogate = 0;
            if (u >= 0xDC00 && u <= 0xDFFF) {
                put(d, o, 0x10000 + ((uint32_t)(lead - 0xD800) << 10) + (u - 0xDC00));
                continue;
            }
            ERR();              /* (and u again) */
        }
        if (u >= 0xD800 && u <= 0xDBFF) d->lead_surrogate = u;
        else if (u >= 0xDC00 && u <= 0xDFFF) ERR();
        else put(d, o, u);
    }
    if (flush && (d->lead_byte >= 0 || d->lead_surrogate)) {
        dec_reset(d);
        ERR();
    }
    return true;
}

static void run_single_byte(Dec *d, const uint8_t *s, size_t n, Out *o) {
    for (size_t i = 0; i < n; i++) {
        uint8_t b = s[i];
        if (b < 0x80) o->p[o->n++] = b;
        else if (d->enc == ENC_USER) o->p[o->n++] = (uint16_t)(0xF780 + b - 0x80);
        else o->p[o->n++] = b < 0xA0 ? g_1252[b - 0x80] : b;
    }
}

/* decode(input, {stream}) */
static GCValue js_decoder_decode(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    Dec *d = dec_of(ctx, this_val);
    if (!d) return illegal(ctx);
    bool has_input = argc > 0 && !JS_IsUndefined(argv[0]);
    size_t n = 0;
    if (has_input && !JS_GetBufferSourceBytes(ctx, argv[0], &n))
        return JS_ThrowTypeError(ctx, "Failed to execute 'decode' on 'TextDecoder': The provided value is not of type "
                                      "'(ArrayBuffer or ArrayBufferView)'.");
    int stream = dict_bool(ctx, argc, argv, 1, "stream", "TextDecodeOptions");
    if (stream < 0) return JS_EXCEPTION;
    if (!d->do_not_flush) {
        dec_reset(d);
        d->bom_seen = false;
    }
    d->do_not_flush = stream;
    /* (the bytes again: reading the options ran script; nothing below does,
     * nor allocates on the heap the buffer is in until they are decoded) */
    const uint8_t *s = has_input ? JS_GetBufferSourceBytes(ctx, argv[0], &n) : NULL;
    if (!s) n = 0;
    Out o = { malloc((n + 4) * sizeof(uint16_t)), 0 };
    if (!o.p) return JS_ThrowOutOfMemory(ctx);
    bool ok = true;
    if (d->enc == ENC_UTF8) ok = run_utf8(d, s, n, !stream, &o);
    else if (d->enc == ENC_UTF16LE || d->enc == ENC_UTF16BE) ok = run_utf16(d, s, n, !stream, &o);
    else run_single_byte(d, s, n, &o);
    if (!ok) {
        free(o.p);
        d->do_not_flush = false;
        return JS_ThrowTypeError(ctx, "Failed to execute 'decode' on 'TextDecoder': The encoded data was not valid.");
    }
    GCValue r = JS_NewStringUTF16(ctx, o.p, o.n);
    free(o.p);
    return r;
}

/* ---- installing ---- */

static void getter(JSContextHandle ctx, GCValue proto, const char *name, JSCFunctionMagic *fn, int magic) {
    JSAtom a = JS_NewAtom(ctx, name);
    JS_DefinePropertyGetSet(ctx, proto, a, JS_NewCFunctionMagic(ctx, fn, name, 0, JS_CFUNC_generic_magic, magic),
                            JS_UNDEFINED, JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, a);
}

static void method(JSContextHandle ctx, GCValue proto, const char *name, JSCFunction *fn, int length) {
    JS_DefinePropertyValueStr(ctx, proto, name, JS_NewCFunction(ctx, fn, name, length),
                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
}

static GCValue interface(JSContextHandle ctx, GCValue global, const char *name, JSCFunction *ctor) {
    GCValue proto = JS_NewObject(ctx);
    GCValue c = JS_NewCFunction2(ctx, ctor, name, 0, JS_CFUNC_constructor_or_func, 0);
    JS_SetConstructor(ctx, c, proto);
    JS_DefinePropertyValueStr(ctx, global, name, c, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    GCValue win = JS_GetPropertyStr(ctx, global, "window");
    if (JS_IsObject(win) && !JS_StrictEq(ctx, win, global))
        JS_DefinePropertyValueStr(ctx, win, name, c, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    GCValue sym = JS_GetPropertyStr(ctx, global, "Symbol");
    JSAtom tag = JS_ValueToAtom(ctx, JS_GetPropertyStr(ctx, sym, "toStringTag"));
    JS_DefinePropertyValue(ctx, proto, tag, JS_NewString(ctx, name), JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, tag);
    return proto;
}

static GCValue js_encoder_encoding_magic(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv, int magic) {
    (void)magic;
    return js_encoder_encoding(ctx, this_val, argc, argv);
}

void encoding_install(JSContextHandle ctx, GCValue global) {
    JSRuntimeHandle rt = JS_GetRuntime(ctx);
    if (!g_enc_class) JS_NewClassID(&g_enc_class);
    if (!g_dec_class) JS_NewClassID(&g_dec_class);
    if (!JS_IsRegisteredClass(rt, g_enc_class)) JS_NewClass(rt, g_enc_class, &g_enc_def);
    if (!JS_IsRegisteredClass(rt, g_dec_class)) JS_NewClass(rt, g_dec_class, &g_dec_def);

    GCValue ep = interface(ctx, global, "TextEncoder", (JSCFunction *)js_encoder_ctor);
    getter(ctx, ep, "encoding", js_encoder_encoding_magic, 0);
    method(ctx, ep, "encode", js_encoder_encode, 0);
    method(ctx, ep, "encodeInto", js_encoder_encode_into, 2);

    GCValue dp = interface(ctx, global, "TextDecoder", (JSCFunction *)js_decoder_ctor);
    getter(ctx, dp, "encoding", js_decoder_get, 0);
    getter(ctx, dp, "fatal", js_decoder_get, 1);
    getter(ctx, dp, "ignoreBOM", js_decoder_get, 2);
    method(ctx, dp, "decode", js_decoder_decode, 0);
}

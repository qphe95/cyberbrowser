/*
 * url_whatwg.c - the WHATWG URL Standard's basic URL parser, host parser
 * and serializers (https://url.spec.whatwg.org/).
 *
 * The parser follows the standard's state machine step by step (state
 * names and step order match the specification text), including the
 * "state override" entry points used by the URL setters.  Hosts go through
 * the host parser: IPv6, IPv4 (with hex/octal parts), opaque hosts, and
 * domains, which are mapped with a simplified UTS #46 processing (case
 * folding, NFKC, ignored and dot-like code points) and Punycode.
 */
#include "url_whatwg.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "quickjs/cutils.h"
#include "quickjs/libunicode.h"

#define EOFC 0xFFFFFFFFu

/* ------------------------------------------------------------------------
 * Small buffers
 * ------------------------------------------------------------------------ */

typedef struct { char *s; size_t len, cap; } SB;   /* byte string */
typedef struct { uint32_t *c; int len, cap; } CB;  /* code points */

static int sb_reserve(SB *b, size_t extra)
{
    if (b->len + extra + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 32;
        while (nc < b->len + extra + 1)
            nc *= 2;
        char *n = realloc(b->s, nc);
        if (!n)
            return -1;
        b->s = n;
        b->cap = nc;
    }
    return 0;
}

static void sb_putc(SB *b, char c)
{
    if (sb_reserve(b, 1))
        return;
    b->s[b->len++] = c;
    b->s[b->len] = 0;
}

static void sb_put(SB *b, const char *s, size_t n)
{
    if (sb_reserve(b, n))
        return;
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = 0;
}

static void sb_puts(SB *b, const char *s) { sb_put(b, s, strlen(s)); }

static char *sb_take(SB *b)
{
    char *s = b->s;
    if (!s) {
        s = malloc(1);
        if (s)
            s[0] = 0;
    }
    b->s = NULL;
    b->len = b->cap = 0;
    return s;
}

static void cb_push(CB *b, uint32_t c)
{
    if (b->len + 1 > b->cap) {
        int nc = b->cap ? b->cap * 2 : 32;
        uint32_t *n = realloc(b->c, nc * sizeof(uint32_t));
        if (!n)
            return;
        b->c = n;
        b->cap = nc;
    }
    b->c[b->len++] = c;
}

static void cb_free(CB *b) { free(b->c); b->c = NULL; b->len = b->cap = 0; }

static char *xstrdup(const char *s) { return s ? strdup(s) : NULL; }

/* UTF-8 -> code points; invalid sequences and lone surrogates become
   U+FFFD (USVString conversion). */
static uint32_t *utf8_to_cps(const char *s, size_t len, int *pn)
{
    uint32_t *out = malloc((len + 1) * sizeof(uint32_t));
    const uint8_t *p = (const uint8_t *)s, *end = p + len;
    int n = 0;
    if (!out)
        return NULL;
    while (p < end) {
        if (*p < 0x80) {
            out[n++] = *p++;
        } else {
            const uint8_t *q;
            int c = unicode_from_utf8(p, end - p, &q);
            if (c < 0 || q <= p) {
                out[n++] = 0xFFFD;
                p++;
            } else {
                if (c >= 0xD800 && c <= 0xDFFF)
                    c = 0xFFFD;
                out[n++] = c;
                p = q;
            }
        }
    }
    *pn = n;
    return out;
}

static void sb_put_cp(SB *b, uint32_t c)
{
    uint8_t buf[8];
    int n = unicode_to_utf8(buf, c);
    sb_put(b, (const char *)buf, n);
}

/* ------------------------------------------------------------------------
 * Percent-encode sets
 * ------------------------------------------------------------------------ */

enum {
    SET_C0, SET_FRAGMENT, SET_QUERY, SET_SPECIAL_QUERY, SET_PATH,
    SET_USERINFO, SET_COMPONENT, SET_FORM
};

static int in_set(uint32_t c, int set)
{
    if (c < 0x20 || c > 0x7E)
        return 1;
    switch (set) {
    case SET_FORM:
        if (c == '!' || c == '\'' || c == '(' || c == ')' || c == '~')
            return 1;
        /* fall through */
    case SET_COMPONENT:
        if ((c >= '$' && c <= '&') || c == '+' || c == ',')
            return 1;
        /* fall through */
    case SET_USERINFO:
        if (c == '/' || c == ':' || c == ';' || c == '=' || c == '@' ||
            (c >= '[' && c <= ']') || c == '|')
            return 1;
        /* fall through */
    case SET_PATH:
        if (c == '?' || c == '^' || c == '`' || c == '{' || c == '}')
            return 1;
        /* fall through */
    case SET_QUERY:
        if (c == ' ' || c == '"' || c == '#' || c == '<' || c == '>')
            return 1;
        return 0;
    case SET_SPECIAL_QUERY:
        if (c == '\'')
            return 1;
        return in_set(c, SET_QUERY);
    case SET_FRAGMENT:
        if (c == ' ' || c == '"' || c == '<' || c == '>' || c == '`')
            return 1;
        return 0;
    case SET_C0:
    default:
        return 0;
    }
}

static void sb_pct_cp(SB *b, uint32_t c, int set)
{
    if (!in_set(c, set)) {
        sb_putc(b, (char)c);
    } else {
        uint8_t buf[8];
        int n = unicode_to_utf8(buf, c);
        for (int i = 0; i < n; i++) {
            char t[4];
            snprintf(t, sizeof(t), "%%%02X", buf[i]);
            sb_put(b, t, 3);
        }
    }
}

static int hexval(uint32_t c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* percent-decode bytes */
static char *pct_decode(const char *s, size_t len, size_t *out_len, int plus_space)
{
    char *out = malloc(len + 1);
    size_t j = 0;
    if (!out)
        return NULL;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '%' && i + 2 < len &&
            hexval((uint8_t)s[i + 1]) >= 0 && hexval((uint8_t)s[i + 2]) >= 0) {
            out[j++] = (char)(hexval((uint8_t)s[i + 1]) * 16 + hexval((uint8_t)s[i + 2]));
            i += 2;
        } else if (plus_space && s[i] == '+') {
            out[j++] = ' ';
        } else {
            out[j++] = s[i];
        }
    }
    out[j] = 0;
    *out_len = j;
    return out;
}

/* ------------------------------------------------------------------------
 * URL record helpers
 * ------------------------------------------------------------------------ */

static const struct { const char *scheme; int port; } special_schemes[] = {
    { "ftp", 21 }, { "file", -1 }, { "http", 80 }, { "https", 443 },
    { "ws", 80 }, { "wss", 443 }, { NULL, 0 }
};

static int scheme_is_special(const char *s)
{
    for (int i = 0; special_schemes[i].scheme; i++)
        if (s && !strcmp(s, special_schemes[i].scheme))
            return 1;
    return 0;
}

static int default_port(const char *s)
{
    for (int i = 0; special_schemes[i].scheme; i++)
        if (s && !strcmp(s, special_schemes[i].scheme))
            return special_schemes[i].port;
    return -1;
}

bool wurl_is_special(const WUrl *u) { return scheme_is_special(u->scheme); }

static void path_clear(WUrl *u)
{
    for (int i = 0; i < u->path_len; i++)
        free(u->path[i]);
    u->path_len = 0;
}

static void path_push(WUrl *u, char *seg)
{
    if (u->path_len + 1 > u->path_cap) {
        int nc = u->path_cap ? u->path_cap * 2 : 8;
        char **n = realloc(u->path, nc * sizeof(char *));
        if (!n) {
            free(seg);
            return;
        }
        u->path = n;
        u->path_cap = nc;
    }
    u->path[u->path_len++] = seg;
}

void wurl_free(WUrl *u)
{
    free(u->scheme);
    free(u->username);
    free(u->password);
    free(u->host);
    path_clear(u);
    free(u->path);
    free(u->opaque_path);
    free(u->query);
    free(u->fragment);
    memset(u, 0, sizeof(*u));
    u->port = -1;
}

int wurl_copy(WUrl *d, const WUrl *s)
{
    memset(d, 0, sizeof(*d));
    d->scheme = xstrdup(s->scheme);
    d->username = xstrdup(s->username);
    d->password = xstrdup(s->password);
    d->host = xstrdup(s->host);
    d->port = s->port;
    for (int i = 0; i < s->path_len; i++)
        path_push(d, strdup(s->path[i]));
    d->opaque_path = xstrdup(s->opaque_path);
    d->query = xstrdup(s->query);
    d->fragment = xstrdup(s->fragment);
    return 0;
}

static void set_str(char **dst, const char *v)
{
    char *n = v ? strdup(v) : NULL;
    free(*dst);
    *dst = n;
}

static int includes_credentials(const WUrl *u)
{
    return (u->username && u->username[0]) || (u->password && u->password[0]);
}

static int cannot_have_credentials_or_port(const WUrl *u)
{
    return !u->host || !u->host[0] || (u->scheme && !strcmp(u->scheme, "file"));
}

static int is_win_drive(const uint32_t *c, int n, int normalized)
{
    if (n != 2)
        return 0;
    if (!((c[0] >= 'a' && c[0] <= 'z') || (c[0] >= 'A' && c[0] <= 'Z')))
        return 0;
    return c[1] == ':' || (!normalized && c[1] == '|');
}

static int str_is_normalized_drive(const char *s)
{
    return s && ((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z')) &&
        s[1] == ':' && s[2] == 0;
}

/* code points from p start with a Windows drive letter */
static int starts_with_win_drive(const uint32_t *in, int n, int p)
{
    if (n - p < 2)
        return 0;
    if (!is_win_drive(in + p, 2, 0))
        return 0;
    if (n - p == 2)
        return 1;
    uint32_t c = in[p + 2];
    return c == '/' || c == '\\' || c == '?' || c == '#';
}

static void shorten_path(WUrl *u)
{
    if (u->scheme && !strcmp(u->scheme, "file") && u->path_len == 1 &&
        str_is_normalized_drive(u->path[0]))
        return;
    if (u->path_len > 0) {
        free(u->path[u->path_len - 1]);
        u->path_len--;
    }
}

static int ci_eq(const uint32_t *c, int n, const char *s)
{
    int i;
    for (i = 0; i < n && s[i]; i++) {
        uint32_t a = c[i];
        if (a >= 'A' && a <= 'Z')
            a += 32;
        if (a != (uint8_t)s[i])
            return 0;
    }
    return i == n && s[i] == 0;
}

static int is_single_dot(const uint32_t *c, int n)
{
    return ci_eq(c, n, ".") || ci_eq(c, n, "%2e");
}

static int is_double_dot(const uint32_t *c, int n)
{
    return ci_eq(c, n, "..") || ci_eq(c, n, ".%2e") || ci_eq(c, n, "%2e.") ||
        ci_eq(c, n, "%2e%2e");
}

static char *cps_to_utf8(const uint32_t *c, int n)
{
    SB b = {0};
    for (int i = 0; i < n; i++)
        sb_put_cp(&b, c[i]);
    return sb_take(&b);
}

/* ------------------------------------------------------------------------
 * Hosts
 * ------------------------------------------------------------------------ */

static int is_forbidden_host_cp(uint32_t c)
{
    switch (c) {
    case 0: case '\t': case '\n': case '\r': case ' ': case '#': case '/':
    case ':': case '<': case '>': case '?': case '@': case '[': case '\\':
    case ']': case '^': case '|':
        return 1;
    }
    return 0;
}

static int is_forbidden_domain_cp(uint32_t c)
{
    return is_forbidden_host_cp(c) || c <= 0x1F || c == '%' || c == 0x7F;
}

/* IPv6 */
static int ipv6_parse(const uint32_t *in, int n, uint16_t addr[8])
{
    int piece = 0, compress = -1, p = 0;
#define C (p < n ? in[p] : EOFC)
    memset(addr, 0, 16);
    if (C == ':') {
        if (!(p + 1 < n && in[p + 1] == ':'))
            return -1;
        p += 2;
        piece++;
        compress = piece;
    }
    while (C != EOFC) {
        if (piece == 8)
            return -1;
        if (C == ':') {
            if (compress != -1)
                return -1;
            p++;
            piece++;
            compress = piece;
            continue;
        }
        uint32_t value = 0;
        int length = 0;
        while (length < 4 && C != EOFC && hexval(C) >= 0) {
            value = value * 16 + hexval(C);
            p++;
            length++;
        }
        if (C == '.') {
            int numbers_seen = 0;
            if (length == 0)
                return -1;
            p -= length;
            if (piece > 6)
                return -1;
            while (C != EOFC) {
                int v4 = -1;
                if (numbers_seen > 0) {
                    if (C == '.' && numbers_seen < 4)
                        p++;
                    else
                        return -1;
                }
                if (C == EOFC || C < '0' || C > '9')
                    return -1;
                while (C != EOFC && C >= '0' && C <= '9') {
                    int num = C - '0';
                    if (v4 == -1)
                        v4 = num;
                    else if (v4 == 0)
                        return -1;
                    else
                        v4 = v4 * 10 + num;
                    if (v4 > 255)
                        return -1;
                    p++;
                }
                addr[piece] = addr[piece] * 0x100 + v4;
                numbers_seen++;
                if (numbers_seen == 2 || numbers_seen == 4)
                    piece++;
            }
            if (numbers_seen != 4)
                return -1;
            break;
        } else if (C == ':') {
            p++;
            if (C == EOFC)
                return -1;
        } else if (C != EOFC) {
            return -1;
        }
        addr[piece] = value;
        piece++;
    }
    if (compress != -1) {
        int swaps = piece - compress;
        piece = 7;
        while (piece != 0 && swaps > 0) {
            uint16_t t = addr[piece];
            addr[piece] = addr[compress + swaps - 1];
            addr[compress + swaps - 1] = t;
            piece--;
            swaps--;
        }
    } else if (piece != 8) {
        return -1;
    }
#undef C
    return 0;
}

static char *ipv6_serialize(const uint16_t a[8])
{
    SB b = {0};
    int best = -1, best_len = 0;
    for (int i = 0; i < 8;) {
        if (a[i] == 0) {
            int j = i;
            while (j < 8 && a[j] == 0)
                j++;
            if (j - i > best_len && j - i > 1) {
                best = i;
                best_len = j - i;
            }
            i = j;
        } else {
            i++;
        }
    }
    sb_putc(&b, '[');
    int ignore0 = 0;
    for (int i = 0; i < 8; i++) {
        if (ignore0 && a[i] == 0)
            continue;
        ignore0 = 0;
        if (best == i) {
            sb_puts(&b, i == 0 ? "::" : ":");
            ignore0 = 1;
            continue;
        }
        char t[8];
        snprintf(t, sizeof(t), "%x", a[i]);
        sb_puts(&b, t);
        if (i != 7)
            sb_putc(&b, ':');
    }
    sb_putc(&b, ']');
    return sb_take(&b);
}

/* IPv4 number parser: returns 0 and *out, or -1. Values are capped. */
static int ipv4_number(const char *s, size_t n, uint64_t *out)
{
    int r = 10;
    uint64_t v = 0;
    if (n == 0)
        return -1;
    if (n >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        r = 16;
        s += 2;
        n -= 2;
    } else if (n >= 2 && s[0] == '0') {
        r = 8;
        s += 1;
        n -= 1;
    }
    if (n == 0) {
        *out = 0;
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        int d = hexval((uint8_t)s[i]);
        if (d < 0 || d >= r)
            return -1;
        v = v * r + d;
        if (v > 0xFFFFFFFFFFull)
            v = 0xFFFFFFFFFFull;
    }
    *out = v;
    return 0;
}

static int ends_in_number(const char *s)
{
    size_t n = strlen(s);
    const char *last, *end = s + n;
    if (n && s[n - 1] == '.') {
        /* drop the trailing empty part, unless it is the only part */
        const char *q = s;
        int parts = 1;
        for (; *q; q++)
            if (*q == '.')
                parts++;
        if (parts == 1)
            return 0;
        end--;
    }
    last = end;
    while (last > s && last[-1] != '.')
        last--;
    if (last == end)
        return 0;
    int all_digits = 1;
    for (const char *q = last; q < end; q++)
        if (*q < '0' || *q > '9')
            all_digits = 0;
    if (all_digits)
        return 1;
    uint64_t v;
    return ipv4_number(last, end - last, &v) == 0;
}

static char *ipv4_parse(const char *s)
{
    uint64_t nums[5];
    int count = 0;
    size_t n = strlen(s);
    const char *p = s, *end = s + n;
    /* split on '.' */
    const char *parts[260];
    size_t lens[260];
    int nparts = 0;
    const char *st = p;
    for (const char *q = p; q <= end; q++) {
        if (q == end || *q == '.') {
            if (nparts >= 255)
                return NULL;
            parts[nparts] = st;
            lens[nparts] = q - st;
            nparts++;
            st = q + 1;
        }
    }
    if (nparts > 1 && lens[nparts - 1] == 0)
        nparts--;
    if (nparts > 4)
        return NULL;
    for (int i = 0; i < nparts; i++) {
        if (ipv4_number(parts[i], lens[i], &nums[count]))
            return NULL;
        count++;
    }
    for (int i = 0; i < count - 1; i++)
        if (nums[i] > 255)
            return NULL;
    uint64_t limit = 1;
    for (int i = 0; i < 5 - count; i++)
        limit *= 256;
    if (nums[count - 1] >= limit)
        return NULL;
    uint64_t ip = nums[count - 1];
    for (int i = 0; i < count - 1; i++) {
        uint64_t m = 1;
        for (int k = 0; k < 3 - i; k++)
            m *= 256;
        ip += nums[i] * m;
    }
    char *out = malloc(16);
    if (out)
        snprintf(out, 16, "%u.%u.%u.%u", (unsigned)(ip >> 24) & 255,
                 (unsigned)(ip >> 16) & 255, (unsigned)(ip >> 8) & 255,
                 (unsigned)ip & 255);
    return out;
}

static int uts46_disallowed(uint32_t c);

/* Punycode (RFC 3492) */
#define PC_BASE 36
#define PC_TMIN 1
#define PC_TMAX 26
#define PC_SKEW 38
#define PC_DAMP 700
#define PC_INITIAL_BIAS 72
#define PC_INITIAL_N 128

static uint32_t pc_adapt(uint32_t delta, uint32_t numpoints, int first)
{
    uint32_t k = 0;
    delta = first ? delta / PC_DAMP : delta / 2;
    delta += delta / numpoints;
    while (delta > ((PC_BASE - PC_TMIN) * PC_TMAX) / 2) {
        delta /= PC_BASE - PC_TMIN;
        k += PC_BASE;
    }
    return k + (PC_BASE - PC_TMIN + 1) * delta / (delta + PC_SKEW);
}

static char pc_digit(uint32_t d) { return d < 26 ? 'a' + d : '0' + (d - 26); }

static int punycode_encode(const uint32_t *in, int n, SB *out)
{
    uint32_t cp = PC_INITIAL_N, delta = 0, bias = PC_INITIAL_BIAS, h, b = 0;
    for (int i = 0; i < n; i++)
        if (in[i] < 0x80) {
            sb_putc(out, (char)in[i]);
            b++;
        }
    h = b;
    if (b > 0)
        sb_putc(out, '-');
    while (h < (uint32_t)n) {
        uint32_t m = 0xFFFFFFFF;
        for (int i = 0; i < n; i++)
            if (in[i] >= cp && in[i] < m)
                m = in[i];
        if ((uint64_t)(m - cp) * (h + 1) + delta > 0xFFFFFFFFu)
            return -1;
        delta += (m - cp) * (h + 1);
        cp = m;
        for (int i = 0; i < n; i++) {
            if (in[i] < cp && ++delta == 0)
                return -1;
            if (in[i] == cp) {
                uint32_t q = delta;
                for (uint32_t k = PC_BASE;; k += PC_BASE) {
                    uint32_t t = k <= bias ? PC_TMIN : k >= bias + PC_TMAX ? PC_TMAX : k - bias;
                    if (q < t)
                        break;
                    sb_putc(out, pc_digit(t + (q - t) % (PC_BASE - t)));
                    q = (q - t) / (PC_BASE - t);
                }
                sb_putc(out, pc_digit(q));
                bias = pc_adapt(delta, h + 1, h == b);
                delta = 0;
                h++;
            }
        }
        delta++;
        cp++;
    }
    return 0;
}

/* Validate a Punycode label (without "xn--"); returns 0 if it decodes to
   a non-empty, non-ASCII-only string. */
static int punycode_valid(const char *s, size_t n)
{
    if (n == 0)
        return 0;  /* "xn--" alone decodes to the empty label */
    uint32_t cp = PC_INITIAL_N, i = 0, bias = PC_INITIAL_BIAS, out = 0;
    size_t b = 0, j;
    int has_non_ascii = 0;
    for (j = 0; j < n; j++)
        if (s[j] == '-')
            b = j;
    if (b > 0)
        out = b;
    for (j = 0; j < b; j++)
        if ((uint8_t)s[j] >= 0x80)
            return -1;
    for (size_t in = b > 0 ? b + 1 : 0; in < n;) {
        uint32_t oldi = i, w = 1;
        for (uint32_t k = PC_BASE;; k += PC_BASE) {
            if (in >= n)
                return -1;
            char c = s[in++];
            uint32_t d;
            if (c >= '0' && c <= '9') d = c - '0' + 26;
            else if (c >= 'a' && c <= 'z') d = c - 'a';
            else if (c >= 'A' && c <= 'Z') d = c - 'A';
            else return -1;
            if (d > (0xFFFFFFFFu - i) / w)
                return -1;
            i += d * w;
            uint32_t t = k <= bias ? PC_TMIN : k >= bias + PC_TMAX ? PC_TMAX : k - bias;
            if (d < t)
                break;
            if (w > 0xFFFFFFFFu / (PC_BASE - t))
                return -1;
            w *= PC_BASE - t;
        }
        out++;
        bias = pc_adapt(i - oldi, out, oldi == 0);
        if (i / out > 0x10FFFF - cp)
            return -1;
        cp += i / out;
        i %= out;
        if (cp < 0xA0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF) ||
            uts46_disallowed(cp))
            return -1;
        has_non_ascii = 1;
        i++;
    }
    return has_non_ascii ? 0 : -1;
}

static int uts46_ignored(uint32_t c)
{
    return c == 0xAD || c == 0x180E || (c >= 0x206A && c <= 0x206F) || c == 0x34F || (c >= 0x180B && c <= 0x180D) || c == 0x180F ||
        c == 0x200B || c == 0x2060 || c == 0x2064 || (c >= 0xFE00 && c <= 0xFE0F) ||
        c == 0xFEFF || (c >= 0x1BCA0 && c <= 0x1BCA3) || (c >= 0xE0100 && c <= 0xE01EF);
}

static int uts46_disallowed(uint32_t c)
{
    return c == 0xFFFD || (c >= 0xFDD0 && c <= 0xFDEF) || (c & 0xFFFE) == 0xFFFE ||
        c == 0x1680 || c == 0x06DD || (c >= 0x2FF0 && c <= 0x2FFF) ||
        (c >= 0x2024 && c <= 0x2026) || c == 0x0378 || c == 0x0379 ||
        (c >= 0xE0000 && c <= 0xE007F) || (c >= 0x80 && c <= 0x9F) || (c >= 0xFFF9 && c <= 0xFFFC) ||
        (c >= 0xE000 && c <= 0xF8FF) || c == 0x2028 || c == 0x2029 ||
        (c >= 0x200E && c <= 0x200F) || (c >= 0x202A && c <= 0x202E) ||
        (c >= 0x2066 && c <= 0x2069);
}

/* domain to ASCII (UTS #46 ToASCII, beStrict = false), simplified */
static char *domain_to_ascii(const uint32_t *in, int n)
{
    int ascii = 1;
    for (int i = 0; i < n; i++)
        if (in[i] >= 0x80)
            ascii = 0;
    CB m = {0};
    if (ascii) {
        for (int i = 0; i < n; i++)
            cb_push(&m, (in[i] >= 'A' && in[i] <= 'Z') ? in[i] + 32 : in[i]);
    } else {
        CB folded = {0};
        for (int i = 0; i < n; i++) {
            uint32_t c = in[i];
            if (uts46_ignored(c))
                continue;
            if (uts46_disallowed(c)) {
                cb_free(&folded);
                return NULL;
            }
            if (c == 0x3002 || c == 0xFF0E || c == 0xFF61) {
                cb_push(&folded, '.');
                continue;
            }
            uint32_t res[3];
            /* lower-casing, not case folding: UTS #46 non-transitional
               processing keeps deviations such as U+00DF and U+03C2 */
            int k = lre_case_conv(res, c, 1);
            for (int j = 0; j < k; j++)
                cb_push(&folded, res[j]);
        }
        uint32_t *norm = NULL;
        int nn = unicode_normalize(&norm, folded.c, folded.len, UNICODE_NFKC, NULL, unicode_libc_realloc);
        cb_free(&folded);
        if (nn < 0)
            return NULL;
        for (int i = 0; i < nn; i++) {
            uint32_t c = norm[i];
            if (c == 0x3002 || c == 0xFF0E || c == 0xFF61)
                c = '.';
            if (c >= 'A' && c <= 'Z')
                c += 32;
            cb_push(&m, c);
        }
        unicode_libc_realloc(NULL, norm, 0);
    }
    /* labels */
    SB out = {0};
    int start = 0;
    for (int i = 0; i <= m.len; i++) {
        if (i == m.len || m.c[i] == '.') {
            int ln = i - start, non_ascii = 0;
            for (int j = start; j < i; j++)
                if (m.c[j] >= 0x80)
                    non_ascii = 1;
            if (non_ascii) {
                int rtl = 0, ltr = 0;
                if (ln >= 4 && m.c[start] == 'x' && m.c[start + 1] == 'n' &&
                    m.c[start + 2] == '-' && m.c[start + 3] == '-')
                    goto fail_label;  /* "xn--" label with non-ASCII */
                for (int j = start; j < i; j++) {
                    uint32_t c = m.c[j];
                    if ((c >= 0x0590 && c <= 0x08FF) || (c >= 0xFB1D && c <= 0xFDFF) ||
                        (c >= 0xFE70 && c <= 0xFEFF))
                        rtl = 1;
                    else if ((c >= 'a' && c <= 'z') || (c >= 0xC0 && c <= 0x24F))
                        ltr = 1;
                }
                if (rtl && ltr)
                    goto fail_label;  /* simplified Bidi rule */
                for (int j = start; j < i; j++) {
                    /* CheckJoiners (simplified): ZWJ/ZWNJ only after a
                       virama, ZWNJ also between Arabic-script letters */
                    uint32_t c = m.c[j];
                    if (c != 0x200C && c != 0x200D)
                        continue;
                    uint32_t prev = j > start ? m.c[j - 1] : 0;
                    uint32_t next = j + 1 < i ? m.c[j + 1] : 0;
                    int virama = prev == 0x094D || prev == 0x09CD || prev == 0x0A4D ||
                        prev == 0x0ACD || prev == 0x0B4D || prev == 0x0BCD ||
                        prev == 0x0C4D || prev == 0x0CCD || prev == 0x0D4D ||
                        prev == 0x0DCA || prev == 0x0E3A || prev == 0x1039 ||
                        prev == 0x17D2 || prev == 0x1B44 || prev == 0xA8C4;
                    int arabic = c == 0x200C && prev >= 0x0620 && prev <= 0x06FF &&
                        next >= 0x0620 && next <= 0x06FF;
                    if (!virama && !arabic)
                        goto fail_label;
                }
                sb_puts(&out, "xn--");
                if (punycode_encode(m.c + start, ln, &out)) {
                    cb_free(&m);
                    free(out.s);
                    return NULL;
                }
            } else {
                if (!ascii && ln >= 4 && m.c[start] == 'x' && m.c[start + 1] == 'n' &&
                    m.c[start + 2] == '-' && m.c[start + 3] == '-') {
                    /* full UTS #46 processing validates A-labels (the
                       all-ASCII fast path of the URL Standard does not) */
                    char tmp[256];
                    int tl = 0;
                    for (int j = start + 4; j < i && tl < 255; j++)
                        tmp[tl++] = (char)m.c[j];
                    if (ln - 4 > 255 || punycode_valid(tmp, tl))
                        goto fail_label;
                }
                for (int j = start; j < i; j++)
                    sb_putc(&out, (char)m.c[j]);
            }
            if (i < m.len)
                sb_putc(&out, '.');
            start = i + 1;
        }
    }
    cb_free(&m);
    {
        char *r = sb_take(&out);
        if (r && !r[0]) {
            free(r);
            return NULL;
        }
        return r;
    }
 fail_label:
    cb_free(&m);
    free(out.s);
    return NULL;
}

/* host parser: returns malloc'd serialized host or NULL on failure */
static char *host_parse(const uint32_t *in, int n, int is_opaque)
{
    if (n > 0 && in[0] == '[') {
        uint16_t a[8];
        if (in[n - 1] != ']')
            return NULL;
        if (ipv6_parse(in + 1, n - 2, a))
            return NULL;
        return ipv6_serialize(a);
    }
    if (is_opaque) {
        SB b = {0};
        for (int i = 0; i < n; i++) {
            if (is_forbidden_host_cp(in[i])) {
                free(b.s);
                return NULL;
            }
            sb_pct_cp(&b, in[i], SET_C0);
        }
        return sb_take(&b);
    }
    for (int i = 0; i < n; i++)
        if (in[i] == 0)
            return NULL;  /* forbidden domain code point */
    /* domain = UTF-8 decode (without BOM) of percent-decode(UTF-8 encode) */
    char *enc = cps_to_utf8(in, n);
    size_t dl;
    char *dec = pct_decode(enc, strlen(enc), &dl, 0);
    free(enc);
    if (!dec)
        return NULL;
    int dn;
    uint32_t *dcp = utf8_to_cps(dec, dl, &dn);
    free(dec);
    if (!dcp)
        return NULL;
    for (int i = 0; i < dn; i++) {
        if (dcp[i] == 0) {  /* forbidden domain code point */
            free(dcp);
            return NULL;
        }
    }
    char *ascii = domain_to_ascii(dcp, dn);
    free(dcp);
    if (!ascii)
        return NULL;
    for (char *q = ascii; *q; q++) {
        if (is_forbidden_domain_cp((uint8_t)*q)) {
            free(ascii);
            return NULL;
        }
    }
    if (ends_in_number(ascii)) {
        char *v4 = ipv4_parse(ascii);
        free(ascii);
        return v4;
    }
    return ascii;
}

/* ------------------------------------------------------------------------
 * Basic URL parser
 * ------------------------------------------------------------------------ */

enum {
    ST_NONE = -1,
    ST_SCHEME_START, ST_SCHEME, ST_NO_SCHEME, ST_SPECIAL_RELATIVE_OR_AUTHORITY,
    ST_PATH_OR_AUTHORITY, ST_RELATIVE, ST_RELATIVE_SLASH,
    ST_SPECIAL_AUTHORITY_SLASHES, ST_SPECIAL_AUTHORITY_IGNORE_SLASHES,
    ST_AUTHORITY, ST_HOST, ST_HOSTNAME, ST_PORT, ST_FILE, ST_FILE_SLASH,
    ST_FILE_HOST, ST_PATH_START, ST_PATH, ST_OPAQUE_PATH, ST_QUERY,
    ST_FRAGMENT
};

#define PARSE_OK 0
#define PARSE_FAIL -1
#define PARSE_RETURN 1  /* state override: stop without failure */

static int basic_parse(const uint32_t *in, int n, const WUrl *base, WUrl *url,
                       int state_override)
{
    int state = state_override != ST_NONE ? state_override : ST_SCHEME_START;
    CB buf = {0};
    int at_seen = 0, inside_brackets = 0, password_seen = 0;
    int p = 0;
    int ret = PARSE_OK;
    int special;

#define CUR (p >= 0 && p < n ? in[p] : EOFC)
#define REMAIN_STARTS(ch) (p + 1 < n && in[p + 1] == (ch))
#define FAIL do { ret = PARSE_FAIL; goto done; } while (0)
#define RETURN do { ret = PARSE_RETURN; goto done; } while (0)

    for (;;) {
        uint32_t c = CUR;
        special = scheme_is_special(url->scheme);
        switch (state) {
        case ST_SCHEME_START:
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
                cb_push(&buf, c | 0x20);
                state = ST_SCHEME;
            } else if (state_override == ST_NONE) {
                state = ST_NO_SCHEME;
                p--;
            } else {
                FAIL;
            }
            break;
        case ST_SCHEME:
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.') {
                cb_push(&buf, (c >= 'A' && c <= 'Z') ? c + 32 : c);
            } else if (c == ':') {
                char *sch = cps_to_utf8(buf.c, buf.len);
                if (state_override != ST_NONE) {
                    int bs = scheme_is_special(sch);
                    if ((special && !bs) || (!special && bs) ||
                        ((includes_credentials(url) || url->port != -1) && !strcmp(sch, "file")) ||
                        (url->scheme && !strcmp(url->scheme, "file") && url->host && !url->host[0])) {
                        free(sch);
                        RETURN;
                    }
                }
                free(url->scheme);
                url->scheme = sch;
                special = scheme_is_special(sch);
                if (state_override != ST_NONE) {
                    if (url->port != -1 && url->port == default_port(url->scheme))
                        url->port = -1;
                    RETURN;
                }
                buf.len = 0;
                if (!strcmp(url->scheme, "file")) {
                    state = ST_FILE;
                } else if (special && base && base->scheme && !strcmp(base->scheme, url->scheme)) {
                    state = ST_SPECIAL_RELATIVE_OR_AUTHORITY;
                } else if (special) {
                    state = ST_SPECIAL_AUTHORITY_SLASHES;
                } else if (REMAIN_STARTS('/')) {
                    state = ST_PATH_OR_AUTHORITY;
                    p++;
                } else {
                    set_str(&url->opaque_path, "");
                    state = ST_OPAQUE_PATH;
                }
            } else if (state_override == ST_NONE) {
                buf.len = 0;
                state = ST_NO_SCHEME;
                p = -1;
            } else {
                FAIL;
            }
            break;
        case ST_NO_SCHEME:
            if (!base || (base->opaque_path && c != '#')) {
                FAIL;
            } else if (base->opaque_path && c == '#') {
                set_str(&url->scheme, base->scheme);
                set_str(&url->opaque_path, base->opaque_path);
                set_str(&url->query, base->query);
                set_str(&url->fragment, "");
                state = ST_FRAGMENT;
            } else if (strcmp(base->scheme, "file")) {
                state = ST_RELATIVE;
                p--;
            } else {
                state = ST_FILE;
                p--;
            }
            break;
        case ST_SPECIAL_RELATIVE_OR_AUTHORITY:
            if (c == '/' && REMAIN_STARTS('/')) {
                state = ST_SPECIAL_AUTHORITY_IGNORE_SLASHES;
                p++;
            } else {
                state = ST_RELATIVE;
                p--;
            }
            break;
        case ST_PATH_OR_AUTHORITY:
            if (c == '/') {
                state = ST_AUTHORITY;
            } else {
                state = ST_PATH;
                p--;
            }
            break;
        case ST_RELATIVE:
            set_str(&url->scheme, base->scheme);
            special = scheme_is_special(url->scheme);
            if (c == '/') {
                state = ST_RELATIVE_SLASH;
            } else if (special && c == '\\') {
                state = ST_RELATIVE_SLASH;
            } else {
                set_str(&url->username, base->username);
                set_str(&url->password, base->password);
                set_str(&url->host, base->host);
                url->port = base->port;
                path_clear(url);
                for (int i = 0; i < base->path_len; i++)
                    path_push(url, strdup(base->path[i]));
                set_str(&url->query, base->query);
                if (c == '?') {
                    set_str(&url->query, "");
                    state = ST_QUERY;
                } else if (c == '#') {
                    set_str(&url->fragment, "");
                    state = ST_FRAGMENT;
                } else if (c != EOFC) {
                    set_str(&url->query, NULL);
                    shorten_path(url);
                    state = ST_PATH;
                    p--;
                }
            }
            break;
        case ST_RELATIVE_SLASH:
            if (special && (c == '/' || c == '\\')) {
                state = ST_SPECIAL_AUTHORITY_IGNORE_SLASHES;
            } else if (c == '/') {
                state = ST_AUTHORITY;
            } else {
                set_str(&url->username, base->username);
                set_str(&url->password, base->password);
                set_str(&url->host, base->host);
                url->port = base->port;
                state = ST_PATH;
                p--;
            }
            break;
        case ST_SPECIAL_AUTHORITY_SLASHES:
            if (c == '/' && REMAIN_STARTS('/')) {
                state = ST_SPECIAL_AUTHORITY_IGNORE_SLASHES;
                p++;
            } else {
                state = ST_SPECIAL_AUTHORITY_IGNORE_SLASHES;
                p--;
            }
            break;
        case ST_SPECIAL_AUTHORITY_IGNORE_SLASHES:
            if (c != '/' && c != '\\') {
                state = ST_AUTHORITY;
                p--;
            }
            break;
        case ST_AUTHORITY:
            if (c == '@') {
                if (at_seen) {
                    CB nb = {0};
                    cb_push(&nb, '%');
                    cb_push(&nb, '4');
                    cb_push(&nb, '0');
                    for (int i = 0; i < buf.len; i++)
                        cb_push(&nb, buf.c[i]);
                    cb_free(&buf);
                    buf = nb;
                }
                at_seen = 1;
                {
                    SB u = {0}, pw = {0};
                    if (url->username)
                        sb_puts(&u, url->username);
                    if (url->password)
                        sb_puts(&pw, url->password);
                    for (int i = 0; i < buf.len; i++) {
                        if (buf.c[i] == ':' && !password_seen) {
                            password_seen = 1;
                            continue;
                        }
                        sb_pct_cp(password_seen ? &pw : &u, buf.c[i], SET_USERINFO);
                    }
                    free(url->username);
                    url->username = sb_take(&u);
                    free(url->password);
                    url->password = sb_take(&pw);
                }
                buf.len = 0;
            } else if (c == EOFC || c == '/' || c == '?' || c == '#' ||
                       (special && c == '\\')) {
                if (at_seen && buf.len == 0)
                    FAIL;
                p -= buf.len + 1;
                buf.len = 0;
                state = ST_HOST;
            } else {
                cb_push(&buf, c);
            }
            break;
        case ST_HOST:
        case ST_HOSTNAME:
            if (state_override != ST_NONE && url->scheme && !strcmp(url->scheme, "file")) {
                p--;
                state = ST_FILE_HOST;
            } else if (c == ':' && !inside_brackets) {
                if (buf.len == 0)
                    FAIL;
                if (state_override == ST_HOSTNAME)
                    FAIL;
                char *h = host_parse(buf.c, buf.len, !special);
                if (!h)
                    FAIL;
                free(url->host);
                url->host = h;
                buf.len = 0;
                state = ST_PORT;
            } else if (c == EOFC || c == '/' || c == '?' || c == '#' ||
                       (special && c == '\\')) {
                p--;
                if (special && buf.len == 0) {
                    FAIL;
                } else if (state_override != ST_NONE && buf.len == 0 &&
                           (includes_credentials(url) || url->port != -1)) {
                    RETURN;
                }
                char *h = host_parse(buf.c, buf.len, !special);
                if (!h)
                    FAIL;
                free(url->host);
                url->host = h;
                buf.len = 0;
                state = ST_PATH_START;
                if (state_override != ST_NONE)
                    RETURN;
            } else {
                if (c == '[')
                    inside_brackets = 1;
                if (c == ']')
                    inside_brackets = 0;
                cb_push(&buf, c);
            }
            break;
        case ST_PORT:
            if (c >= '0' && c <= '9') {
                cb_push(&buf, c);
            } else if (c == EOFC || c == '/' || c == '?' || c == '#' ||
                       (special && c == '\\') || state_override != ST_NONE) {
                if (buf.len > 0) {
                    uint64_t port = 0;
                    for (int i = 0; i < buf.len; i++) {
                        port = port * 10 + (buf.c[i] - '0');
                        if (port > 65535)
                            FAIL;
                    }
                    url->port = ((int)port == default_port(url->scheme)) ? -1 : (int)port;
                    buf.len = 0;
                    if (state_override != ST_NONE)
                        RETURN;
                }
                if (state_override != ST_NONE)
                    RETURN;
                state = ST_PATH_START;
                p--;
            } else {
                FAIL;
            }
            break;
        case ST_FILE:
            set_str(&url->scheme, "file");
            special = 1;
            set_str(&url->host, "");
            if (c == '/' || c == '\\') {
                state = ST_FILE_SLASH;
            } else if (base && base->scheme && !strcmp(base->scheme, "file")) {
                set_str(&url->host, base->host);
                path_clear(url);
                for (int i = 0; i < base->path_len; i++)
                    path_push(url, strdup(base->path[i]));
                set_str(&url->query, base->query);
                if (c == '?') {
                    set_str(&url->query, "");
                    state = ST_QUERY;
                } else if (c == '#') {
                    set_str(&url->fragment, "");
                    state = ST_FRAGMENT;
                } else if (c != EOFC) {
                    set_str(&url->query, NULL);
                    if (!starts_with_win_drive(in, n, p))
                        shorten_path(url);
                    else
                        path_clear(url);
                    state = ST_PATH;
                    p--;
                }
            } else {
                state = ST_PATH;
                p--;
            }
            break;
        case ST_FILE_SLASH:
            if (c == '/' || c == '\\') {
                state = ST_FILE_HOST;
            } else {
                if (base && base->scheme && !strcmp(base->scheme, "file")) {
                    set_str(&url->host, base->host);
                    if (!starts_with_win_drive(in, n, p) && base->path_len > 0 &&
                        str_is_normalized_drive(base->path[0]))
                        path_push(url, strdup(base->path[0]));
                }
                state = ST_PATH;
                p--;
            }
            break;
        case ST_FILE_HOST:
            if (c == EOFC || c == '/' || c == '\\' || c == '?' || c == '#') {
                p--;
                if (state_override == ST_NONE && is_win_drive(buf.c, buf.len, 0)) {
                    state = ST_PATH;
                } else if (buf.len == 0) {
                    set_str(&url->host, "");
                    if (state_override != ST_NONE)
                        RETURN;
                    state = ST_PATH_START;
                } else {
                    char *h = host_parse(buf.c, buf.len, !special);
                    if (!h)
                        FAIL;
                    if (!strcmp(h, "localhost"))
                        h[0] = 0;
                    free(url->host);
                    url->host = h;
                    if (state_override != ST_NONE)
                        RETURN;
                    buf.len = 0;
                    state = ST_PATH_START;
                }
            } else {
                cb_push(&buf, c);
            }
            break;
        case ST_PATH_START:
            if (special) {
                state = ST_PATH;
                if (c != '/' && c != '\\')
                    p--;
            } else if (state_override == ST_NONE && c == '?') {
                set_str(&url->query, "");
                state = ST_QUERY;
            } else if (state_override == ST_NONE && c == '#') {
                set_str(&url->fragment, "");
                state = ST_FRAGMENT;
            } else if (c != EOFC) {
                state = ST_PATH;
                if (c != '/')
                    p--;
            } else if (state_override != ST_NONE && !url->host) {
                path_push(url, strdup(""));
            }
            break;
        case ST_PATH:
            if (c == EOFC || c == '/' || (special && c == '\\') ||
                (state_override == ST_NONE && (c == '?' || c == '#'))) {
                int slash = (c == '/' || (special && c == '\\'));
                if (is_double_dot(buf.c, buf.len)) {
                    shorten_path(url);
                    if (!slash)
                        path_push(url, strdup(""));
                } else if (is_single_dot(buf.c, buf.len) && !slash) {
                    path_push(url, strdup(""));
                } else if (!is_single_dot(buf.c, buf.len)) {
                    if (url->scheme && !strcmp(url->scheme, "file") && url->path_len == 0 &&
                        is_win_drive(buf.c, buf.len, 0))
                        buf.c[1] = ':';
                    path_push(url, cps_to_utf8(buf.c, buf.len));
                }
                buf.len = 0;
                if (c == '?') {
                    set_str(&url->query, "");
                    state = ST_QUERY;
                }
                if (c == '#') {
                    set_str(&url->fragment, "");
                    state = ST_FRAGMENT;
                }
            } else {
                SB e = {0};
                sb_pct_cp(&e, c, SET_PATH);
                for (size_t i = 0; i < e.len; i++)
                    cb_push(&buf, (uint8_t)e.s[i]);
                free(e.s);
            }
            break;
        case ST_OPAQUE_PATH:
            if (c == '?') {
                set_str(&url->query, "");
                state = ST_QUERY;
            } else if (c == '#') {
                set_str(&url->fragment, "");
                state = ST_FRAGMENT;
            } else if (c == ' ') {
                SB o = {0};
                sb_puts(&o, url->opaque_path ? url->opaque_path : "");
                if (p + 1 < n && (in[p + 1] == '?' || in[p + 1] == '#'))
                    sb_puts(&o, "%20");
                else
                    sb_putc(&o, ' ');
                free(url->opaque_path);
                url->opaque_path = sb_take(&o);
            } else if (c != EOFC) {
                SB o = {0};
                sb_puts(&o, url->opaque_path ? url->opaque_path : "");
                sb_pct_cp(&o, c, SET_C0);
                free(url->opaque_path);
                url->opaque_path = sb_take(&o);
            }
            break;
        case ST_QUERY:
            if ((state_override == ST_NONE && c == '#') || c == EOFC) {
                SB q = {0};
                int set = special ? SET_SPECIAL_QUERY : SET_QUERY;
                sb_puts(&q, url->query ? url->query : "");
                for (int i = 0; i < buf.len; i++)
                    sb_pct_cp(&q, buf.c[i], set);
                free(url->query);
                url->query = sb_take(&q);
                buf.len = 0;
                if (c == '#') {
                    set_str(&url->fragment, "");
                    state = ST_FRAGMENT;
                }
            } else if (c != EOFC) {
                cb_push(&buf, c);
            }
            break;
        case ST_FRAGMENT:
            if (c != EOFC) {
                SB f = {0};
                sb_puts(&f, url->fragment ? url->fragment : "");
                sb_pct_cp(&f, c, SET_FRAGMENT);
                free(url->fragment);
                url->fragment = sb_take(&f);
            }
            break;
        }
        if (p >= n)
            break;
        p++;
    }
 done:
    cb_free(&buf);
    return ret;
#undef CUR
#undef REMAIN_STARTS
#undef FAIL
#undef RETURN
}

/* Input preprocessing; strip_c0_space for a parse without a URL. */
static uint32_t *prep_input(const char *s, size_t len, int strip_c0_space, int *pn)
{
    int n;
    uint32_t *cps = utf8_to_cps(s, len, &n);
    if (!cps)
        return NULL;
    int st = 0, en = n;
    if (strip_c0_space) {
        while (st < en && cps[st] <= 0x20)
            st++;
        while (en > st && cps[en - 1] <= 0x20)
            en--;
    }
    int k = 0;
    for (int i = st; i < en; i++) {
        uint32_t c = cps[i];
        if (c == '\t' || c == '\n' || c == '\r')
            continue;
        cps[k++] = c;
    }
    *pn = k;
    return cps;
}

static void wurl_init(WUrl *u)
{
    memset(u, 0, sizeof(*u));
    u->username = strdup("");
    u->password = strdup("");
    u->port = -1;
}

int wurl_parse(const char *input, size_t len, const WUrl *base, WUrl *out)
{
    int n;
    uint32_t *cps = prep_input(input, len, 1, &n);
    wurl_init(out);
    if (!cps)
        return -1;
    int r = basic_parse(cps, n, base, out, ST_NONE);
    free(cps);
    if (r == PARSE_FAIL) {
        wurl_free(out);
        return -1;
    }
    return 0;
}

/* Run a state-override parse.  As in the standard, the URL is modified in
   place: a failure part-way keeps what was already set (e.g. a host
   setter with an invalid port still sets the host). */
static void override_parse(WUrl *url, const char *value, size_t len, int state)
{
    int n;
    uint32_t *cps = prep_input(value, len, 0, &n);
    if (!cps)
        return;
    basic_parse(cps, n, NULL, url, state);
    free(cps);
}

static void strip_opaque_path_trailing_spaces(WUrl *u)
{
    if (!u->opaque_path || u->fragment || u->query)
        return;
    size_t l = strlen(u->opaque_path);
    while (l > 0 && u->opaque_path[l - 1] == ' ')
        u->opaque_path[--l] = 0;
}

int wurl_set(WUrl *url, WUrlSetter which, const char *value, size_t len)
{
    switch (which) {
    case WURL_SET_HREF: {
        WUrl nu;
        if (wurl_parse(value, len, NULL, &nu))
            return -1;
        wurl_free(url);
        *url = nu;
        return 0;
    }
    case WURL_SET_PROTOCOL: {
        char *v = malloc(len + 2);
        if (!v)
            return 0;
        memcpy(v, value, len);
        v[len] = ':';
        v[len + 1] = 0;
        override_parse(url, v, len + 1, ST_SCHEME_START);
        free(v);
        return 0;
    }
    case WURL_SET_USERNAME:
    case WURL_SET_PASSWORD: {
        if (cannot_have_credentials_or_port(url))
            return 0;
        int n;
        uint32_t *cps = utf8_to_cps(value, len, &n);
        SB b = {0};
        for (int i = 0; i < n; i++)
            sb_pct_cp(&b, cps[i], SET_USERINFO);
        free(cps);
        if (which == WURL_SET_USERNAME) {
            free(url->username);
            url->username = sb_take(&b);
        } else {
            free(url->password);
            url->password = sb_take(&b);
        }
        return 0;
    }
    case WURL_SET_HOST:
        if (url->opaque_path)
            return 0;
        override_parse(url, value, len, ST_HOST);
        return 0;
    case WURL_SET_HOSTNAME:
        if (url->opaque_path)
            return 0;
        override_parse(url, value, len, ST_HOSTNAME);
        return 0;
    case WURL_SET_PORT:
        if (cannot_have_credentials_or_port(url))
            return 0;
        if (len == 0) {
            url->port = -1;
            return 0;
        }
        override_parse(url, value, len, ST_PORT);
        return 0;
    case WURL_SET_PATHNAME: {
        if (url->opaque_path)
            return 0;
        WUrl tmp;
        int n;
        uint32_t *cps = prep_input(value, len, 0, &n);
        if (!cps)
            return 0;
        wurl_copy(&tmp, url);
        path_clear(&tmp);
        if (basic_parse(cps, n, NULL, &tmp, ST_PATH_START) == PARSE_FAIL) {
            wurl_free(&tmp);
        } else {
            wurl_free(url);
            *url = tmp;
        }
        free(cps);
        return 0;
    }
    case WURL_SET_SEARCH: {
        if (len == 0) {
            set_str(&url->query, NULL);
            strip_opaque_path_trailing_spaces(url);
            return 0;
        }
        if (value[0] == '?') {
            value++;
            len--;
        }
        WUrl tmp;
        int n;
        uint32_t *cps = prep_input(value, len, 0, &n);
        if (!cps)
            return 0;
        wurl_copy(&tmp, url);
        set_str(&tmp.query, "");
        if (basic_parse(cps, n, NULL, &tmp, ST_QUERY) == PARSE_FAIL) {
            wurl_free(&tmp);
        } else {
            wurl_free(url);
            *url = tmp;
        }
        free(cps);
        return 0;
    }
    case WURL_SET_HASH: {
        if (len == 0) {
            set_str(&url->fragment, NULL);
            strip_opaque_path_trailing_spaces(url);
            return 0;
        }
        if (value[0] == '#') {
            value++;
            len--;
        }
        WUrl tmp;
        int n;
        uint32_t *cps = prep_input(value, len, 0, &n);
        if (!cps)
            return 0;
        wurl_copy(&tmp, url);
        set_str(&tmp.fragment, "");
        if (basic_parse(cps, n, NULL, &tmp, ST_FRAGMENT) == PARSE_FAIL) {
            wurl_free(&tmp);
        } else {
            wurl_free(url);
            *url = tmp;
        }
        free(cps);
        return 0;
    }
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * Serializers
 * ------------------------------------------------------------------------ */

static void ser_path(const WUrl *u, SB *b)
{
    if (u->opaque_path) {
        sb_puts(b, u->opaque_path);
        return;
    }
    for (int i = 0; i < u->path_len; i++) {
        sb_putc(b, '/');
        sb_puts(b, u->path[i]);
    }
}

char *wurl_href(const WUrl *u)
{
    SB b = {0};
    sb_puts(&b, u->scheme ? u->scheme : "");
    sb_putc(&b, ':');
    if (u->host) {
        sb_puts(&b, "//");
        if (includes_credentials(u)) {
            sb_puts(&b, u->username);
            if (u->password && u->password[0]) {
                sb_putc(&b, ':');
                sb_puts(&b, u->password);
            }
            sb_putc(&b, '@');
        }
        sb_puts(&b, u->host);
        if (u->port != -1) {
            char t[16];
            snprintf(t, sizeof(t), ":%d", u->port);
            sb_puts(&b, t);
        }
    } else if (!u->opaque_path && u->path_len > 1 && !u->path[0][0]) {
        sb_puts(&b, "/.");
    }
    ser_path(u, &b);
    if (u->query) {
        sb_putc(&b, '?');
        sb_puts(&b, u->query);
    }
    if (u->fragment) {
        sb_putc(&b, '#');
        sb_puts(&b, u->fragment);
    }
    return sb_take(&b);
}

char *wurl_origin(const WUrl *u)
{
    const char *s = u->scheme ? u->scheme : "";
    if (!strcmp(s, "blob")) {
        char *path = wurl_pathname(u);
        WUrl inner;
        char *r = NULL;
        if (path && !wurl_parse(path, strlen(path), NULL, &inner)) {
            if (inner.scheme && (!strcmp(inner.scheme, "http") || !strcmp(inner.scheme, "https")))
                r = wurl_origin(&inner);
            wurl_free(&inner);
        }
        free(path);
        return r ? r : strdup("null");
    }
    if (!strcmp(s, "ftp") || !strcmp(s, "http") || !strcmp(s, "https") ||
        !strcmp(s, "ws") || !strcmp(s, "wss")) {
        SB b = {0};
        sb_puts(&b, s);
        sb_puts(&b, "://");
        sb_puts(&b, u->host ? u->host : "");
        if (u->port != -1) {
            char t[16];
            snprintf(t, sizeof(t), ":%d", u->port);
            sb_puts(&b, t);
        }
        return sb_take(&b);
    }
    return strdup("null");
}

char *wurl_protocol(const WUrl *u)
{
    SB b = {0};
    sb_puts(&b, u->scheme ? u->scheme : "");
    sb_putc(&b, ':');
    return sb_take(&b);
}

char *wurl_host(const WUrl *u)
{
    SB b = {0};
    if (u->host) {
        sb_puts(&b, u->host);
        if (u->port != -1) {
            char t[16];
            snprintf(t, sizeof(t), ":%d", u->port);
            sb_puts(&b, t);
        }
    }
    return sb_take(&b);
}

char *wurl_hostname(const WUrl *u) { return strdup(u->host ? u->host : ""); }

char *wurl_port(const WUrl *u)
{
    char t[16] = "";
    if (u->port != -1)
        snprintf(t, sizeof(t), "%d", u->port);
    return strdup(t);
}

char *wurl_pathname(const WUrl *u)
{
    SB b = {0};
    ser_path(u, &b);
    return sb_take(&b);
}

char *wurl_search(const WUrl *u)
{
    if (!u->query || !u->query[0])
        return strdup("");
    SB b = {0};
    sb_putc(&b, '?');
    sb_puts(&b, u->query);
    return sb_take(&b);
}

char *wurl_hash(const WUrl *u)
{
    if (!u->fragment || !u->fragment[0])
        return strdup("");
    SB b = {0};
    sb_putc(&b, '#');
    sb_puts(&b, u->fragment);
    return sb_take(&b);
}

/* ------------------------------------------------------------------------
 * application/x-www-form-urlencoded
 * ------------------------------------------------------------------------ */

char *wurl_form_encode(const char *s, size_t len)
{
    int n;
    uint32_t *cps = utf8_to_cps(s, len, &n);
    SB b = {0};
    if (!cps)
        return strdup("");
    for (int i = 0; i < n; i++) {
        if (cps[i] == ' ')
            sb_putc(&b, '+');
        else
            sb_pct_cp(&b, cps[i], SET_FORM);
    }
    free(cps);
    return sb_take(&b);
}

char *wurl_form_decode(const char *s, size_t len, size_t *out_len)
{
    size_t dl;
    char *d = pct_decode(s, len, &dl, 1);
    if (!d)
        return NULL;
    /* UTF-8 decode without BOM or fail: invalid sequences -> U+FFFD */
    int n;
    uint32_t *cps = utf8_to_cps(d, dl, &n);
    free(d);
    if (!cps)
        return NULL;
    SB b = {0};
    for (int i = 0; i < n; i++)
        sb_put_cp(&b, cps[i]);
    free(cps);
    *out_len = b.len;
    return sb_take(&b);
}

char *wurl_resolve(const char *base, const char *href)
{
    if (!href) return NULL;
    WUrl b, u;
    bool have_base = false;
    if (base && base[0] && wurl_parse(base, strlen(base), NULL, &b) == 0) have_base = true;
    char *out = NULL;
    if (wurl_parse(href, strlen(href), have_base ? &b : NULL, &u) == 0) {
        out = wurl_href(&u);
        wurl_free(&u);
    }
    if (have_base) wurl_free(&b);
    return out;
}

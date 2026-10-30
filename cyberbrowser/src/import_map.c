/*
 * import_map.c - the document's import map (import_map.h), after HTML
 * §8.1.5.3 "parse an import map string", "merge existing and new import
 * maps" and "resolve a module specifier".
 */
#include "import_map.h"
#include "url_whatwg.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef struct ImportEntry {
    char *key;          /* normalized specifier key */
    char *url;          /* absolute URL, or NULL (a key that blocks) */
} ImportEntry;

typedef struct ImportTable {
    ImportEntry *e;
    int n, cap;
} ImportTable;

typedef struct ImportScope {
    char *prefix;       /* absolute URL */
    ImportTable map;
} ImportScope;

static ImportTable g_imports;
static ImportScope *g_scopes;
static int g_nscopes, g_cap_scopes;

static void table_free(ImportTable *t) {
    for (int i = 0; i < t->n; i++) { free(t->e[i].key); free(t->e[i].url); }
    free(t->e);
    memset(t, 0, sizeof(*t));
}

void import_map_reset(void) {
    table_free(&g_imports);
    for (int i = 0; i < g_nscopes; i++) { free(g_scopes[i].prefix); table_free(&g_scopes[i].map); }
    free(g_scopes);
    g_scopes = NULL;
    g_nscopes = g_cap_scopes = 0;
}

/* ---- a small JSON reader: strings, and skipping any other value ---- */

typedef struct Json { const char *p, *end; } Json;

static void ws(Json *j) {
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) j->p++;
}

static int hex4(const char *p) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return -1;
    }
    return v;
}

static void put_utf8(char *out, size_t *n, unsigned cp) {
    if (cp < 0x80) out[(*n)++] = (char)cp;
    else if (cp < 0x800) { out[(*n)++] = (char)(0xC0 | cp >> 6); out[(*n)++] = (char)(0x80 | (cp & 63)); }
    else if (cp < 0x10000) {
        out[(*n)++] = (char)(0xE0 | cp >> 12);
        out[(*n)++] = (char)(0x80 | ((cp >> 6) & 63));
        out[(*n)++] = (char)(0x80 | (cp & 63));
    } else {
        out[(*n)++] = (char)(0xF0 | cp >> 18);
        out[(*n)++] = (char)(0x80 | ((cp >> 12) & 63));
        out[(*n)++] = (char)(0x80 | ((cp >> 6) & 63));
        out[(*n)++] = (char)(0x80 | (cp & 63));
    }
}

/* A string at j->p (malloc'd, UTF-8), or NULL when it is not one. */
static char *json_string(Json *j) {
    ws(j);
    if (j->p >= j->end || *j->p != '"') return NULL;
    const char *s = ++j->p;
    /* Escapes only shrink: the raw length bounds the decoded one. */
    char *out = (char *)malloc((size_t)(j->end - s) + 1);
    if (!out) return NULL;
    size_t n = 0;
    while (j->p < j->end && *j->p != '"') {
        char c = *j->p++;
        if (c != '\\') { out[n++] = c; continue; }
        if (j->p >= j->end) break;
        c = *j->p++;
        switch (c) {
        case 'b': out[n++] = '\b'; break;
        case 'f': out[n++] = '\f'; break;
        case 'n': out[n++] = '\n'; break;
        case 'r': out[n++] = '\r'; break;
        case 't': out[n++] = '\t'; break;
        case 'u': {
            int cp = j->end - j->p >= 4 ? hex4(j->p) : -1;
            if (cp < 0) { free(out); return NULL; }
            j->p += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && j->end - j->p >= 6 && j->p[0] == '\\' && j->p[1] == 'u') {
                int lo = hex4(j->p + 2);
                if (lo >= 0xDC00 && lo < 0xE000) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    j->p += 6;
                }
            }
            put_utf8(out, &n, (unsigned)cp);
            break;
        }
        default: out[n++] = c; break;
        }
    }
    if (j->p >= j->end) { free(out); return NULL; }
    j->p++;
    out[n] = '\0';
    return out;
}

/* Skip any value; false on malformed JSON. */
static bool json_skip(Json *j) {
    ws(j);
    if (j->p >= j->end) return false;
    if (*j->p == '"') { char *s = json_string(j); free(s); return s != NULL; }
    if (*j->p == '{' || *j->p == '[') {
        char close = *j->p == '{' ? '}' : ']';
        j->p++;
        ws(j);
        if (j->p < j->end && *j->p == close) { j->p++; return true; }
        for (;;) {
            if (close == '}') {
                char *k = json_string(j);
                if (!k) return false;
                free(k);
                ws(j);
                if (j->p >= j->end || *j->p != ':') return false;
                j->p++;
            }
            if (!json_skip(j)) return false;
            ws(j);
            if (j->p < j->end && *j->p == ',') { j->p++; continue; }
            if (j->p < j->end && *j->p == close) { j->p++; return true; }
            return false;
        }
    }
    /* A number, true, false or null. */
    const char *s = j->p;
    while (j->p < j->end && *j->p != ',' && *j->p != '}' && *j->p != ']' &&
           *j->p != ' ' && *j->p != '\t' && *j->p != '\n' && *j->p != '\r') j->p++;
    return j->p > s;
}

/* Calls each(key, value position) for an object's members; the callback
 * consumes the value.  False on malformed JSON or a non-object. */
typedef bool (*MemberFn)(Json *j, char *key, void *opaque);
static bool json_object(Json *j, MemberFn each, void *opaque) {
    ws(j);
    if (j->p >= j->end || *j->p != '{') return false;
    j->p++;
    ws(j);
    if (j->p < j->end && *j->p == '}') { j->p++; return true; }
    for (;;) {
        char *k = json_string(j);
        if (!k) return false;
        ws(j);
        if (j->p >= j->end || *j->p != ':') { free(k); return false; }
        j->p++;
        if (!each(j, k, opaque)) return false;   /* (each owns k) */
        ws(j);
        if (j->p < j->end && *j->p == ',') { j->p++; continue; }
        if (j->p < j->end && *j->p == '}') { j->p++; return true; }
        return false;
    }
}

/* ---- the map ---- */

static bool url_like(const char *s) {
    return s[0] == '/' || strncmp(s, "./", 2) == 0 || strncmp(s, "../", 3) == 0;
}

/* "Resolve a URL-like module specifier": NULL when it is neither URL-like
 * nor an absolute URL. */
static char *resolve_url_like(const char *spec, const char *base) {
    if (url_like(spec)) return base ? wurl_resolve(base, spec) : NULL;
    return wurl_resolve(NULL, spec);
}

static bool special_url(const char *u) {
    return strncmp(u, "http:", 5) == 0 || strncmp(u, "https:", 6) == 0 || strncmp(u, "file:", 5) == 0 ||
           strncmp(u, "ws:", 3) == 0 || strncmp(u, "wss:", 4) == 0 || strncmp(u, "ftp:", 4) == 0;
}

static void table_put(ImportTable *t, char *key, char *url) {
    for (int i = 0; i < t->n; i++)
        if (strcmp(t->e[i].key, key) == 0) { free(key); free(url); return; }   /* first wins */
    if (t->n == t->cap) {
        int cap = t->cap ? t->cap * 2 : 16;
        ImportEntry *e = (ImportEntry *)realloc(t->e, (size_t)cap * sizeof(*e));
        if (!e) { free(key); free(url); return; }
        t->e = e;
        t->cap = cap;
    }
    /* Kept in descending code-unit order: a longer prefix comes first. */
    int i = t->n;
    while (i > 0 && strcmp(t->e[i - 1].key, key) < 0) { t->e[i] = t->e[i - 1]; i--; }
    t->e[i].key = key;
    t->e[i].url = url;
    t->n++;
}

typedef struct AddCtx { const char *base; ImportTable *table; } AddCtx;

/* "Sort and normalize a specifier map": one member. */
static bool add_specifier(Json *j, char *key, void *opaque) {
    AddCtx *c = (AddCtx *)opaque;
    ws(j);
    if (!key[0] || j->p >= j->end || *j->p != '"') {
        free(key);
        return json_skip(j);
    }
    char *value = json_string(j);
    if (!value) { free(key); return false; }
    char *nkey = resolve_url_like(key, c->base);
    if (nkey) { free(key); key = nkey; }
    char *url = resolve_url_like(value, c->base);
    /* A key ending in "/" maps only to an address ending in "/". */
    size_t kl = strlen(key);
    if (url && key[kl - 1] == '/' && url[strlen(url) - 1] != '/') { free(url); url = NULL; }
    free(value);
    table_put(c->table, key, url);
    return true;
}

static bool add_imports(Json *j, char *key, void *opaque) {
    (void)key;
    return json_object(j, add_specifier, opaque);
}

static bool add_scope(Json *j, char *key, void *opaque) {
    AddCtx *c = (AddCtx *)opaque;
    char *prefix = c->base ? wurl_resolve(c->base, key) : NULL;
    free(key);
    if (!prefix) return json_skip(j);
    ImportScope *s = NULL;
    for (int i = 0; i < g_nscopes; i++)
        if (strcmp(g_scopes[i].prefix, prefix) == 0) { s = &g_scopes[i]; break; }
    if (!s) {
        if (g_nscopes == g_cap_scopes) {
            int cap = g_cap_scopes ? g_cap_scopes * 2 : 4;
            ImportScope *ns = (ImportScope *)realloc(g_scopes, (size_t)cap * sizeof(*ns));
            if (!ns) { free(prefix); return json_skip(j); }
            g_scopes = ns;
            g_cap_scopes = cap;
        }
        int i = g_nscopes++;
        while (i > 0 && strcmp(g_scopes[i - 1].prefix, prefix) < 0) { g_scopes[i] = g_scopes[i - 1]; i--; }
        s = &g_scopes[i];
        memset(s, 0, sizeof(*s));
        s->prefix = prefix;
    } else {
        free(prefix);
    }
    AddCtx sc = { c->base, &s->map };
    return json_object(j, add_specifier, &sc);
}

static bool add_top(Json *j, char *key, void *opaque) {
    bool ok;
    if (strcmp(key, "imports") == 0) {
        AddCtx c = { (const char *)opaque, &g_imports };
        ok = add_imports(j, NULL, &c);
    } else if (strcmp(key, "scopes") == 0) {
        AddCtx c = { (const char *)opaque, NULL };
        ok = json_object(j, add_scope, &c);
    } else {
        ok = json_skip(j);  /* "integrity", and keys this map doesn't know */
    }
    free(key);
    return ok;
}

void import_map_add(const char *json, size_t len, const char *base_url) {
    if (!json) return;
    /* Malformed JSON is an error: check it all before adding any of it. */
    Json check = { json, json + len };
    if (!json_skip(&check)) return;
    ws(&check);
    if (check.p != check.end) return;
    Json j = { json, json + len };
    json_object(&j, add_top, (void *)base_url);
}

/* "Resolve an imports match": NULL with *hit false when no key matches. */
static char *match(const ImportTable *t, const char *spec, bool spec_url_special, bool *hit) {
    for (int i = 0; i < t->n; i++) {
        const ImportEntry *e = &t->e[i];
        size_t kl = strlen(e->key);
        if (strcmp(e->key, spec) == 0) {
            *hit = true;
            return e->url ? strdup(e->url) : NULL;
        }
        if (e->key[kl - 1] == '/' && strncmp(spec, e->key, kl) == 0 && spec_url_special) {
            *hit = true;
            if (!e->url) return NULL;
            char *u = wurl_resolve(e->url, spec + kl);
            /* Backtracking above the mapped prefix is blocked. */
            if (u && strncmp(u, e->url, strlen(e->url)) != 0) { free(u); u = NULL; }
            return u;
        }
    }
    return NULL;
}

char *import_map_resolve(const char *specifier, const char *base_url) {
    if (!specifier || !specifier[0] || (!g_imports.n && !g_nscopes)) return NULL;
    char *as_url = resolve_url_like(specifier, base_url);
    const char *norm = as_url ? as_url : specifier;
    /* A bare specifier, or a URL of a special scheme, may match a prefix. */
    bool prefixable = !as_url || special_url(as_url);
    bool hit = false;
    char *out = NULL;
    for (int i = 0; base_url && i < g_nscopes && !hit; i++) {
        const char *p = g_scopes[i].prefix;
        size_t pl = strlen(p);
        if (strcmp(p, base_url) == 0 || (p[pl - 1] == '/' && strncmp(base_url, p, pl) == 0))
            out = match(&g_scopes[i].map, norm, prefixable, &hit);
    }
    if (!hit) out = match(&g_imports, norm, prefixable, &hit);
    free(as_url);
    return out;
}

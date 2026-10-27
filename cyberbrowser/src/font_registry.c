/* Font registry: system font index, @font-face web fonts, generic family
 * mapping, CSS font matching and the per-glyph fallback chain.  See
 * font_registry.h. */

#include "font_registry.h"
#include "dom_fetch.h"

#include <ctype.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

#include "font_loader.h"
#include "http_download.h"
#include "text_shaper.h"
#include "url_whatwg.h"

#define FONT_PIXELS 16.0f
#define FAMILY_MAX 64

/* ---- Tables ---- */

typedef struct {
    char family[FAMILY_MAX];      /* lowercase legacy family (name ID 1) */
    char typo[FAMILY_MAX];        /* lowercase typographic family (ID 16), or "" */
    char *path;
    int index;                    /* face index inside a collection */
    int weight;
    bool italic;
    int slot;                     /* -1 not loaded, -2 failed */
} SysFace;

enum { WEB_IDLE, WEB_QUEUED, WEB_READY, WEB_FAILED };

typedef struct {
    char family[FAMILY_MAX];
    char *url;
    int wmin, wmax;
    bool italic;
    uint32_t *ranges;             /* [lo, hi] pairs; NULL = everything */
    int nranges;
    int state;
    int slot;
    /* Variable fonts: the decoded file and the static instances made from it
     * (one per weight asked for). */
    uint8_t *var_sfnt;
    size_t var_len;
    struct { int weight, slot; } inst[16];
    int ninst;
} WebFace;

/* The slot drawing a variable web face at `weight` (clamped to the face's
 * declared range and the font's wght axis): a static instance made on first
 * use; synthetic bold over the default instance if instancing fails. */
static int add_slot_locked(TextShaper *s);
static int web_instance_locked(WebFace *w, int weight)
{
    if (weight < w->wmin) weight = w->wmin;
    if (weight > w->wmax) weight = w->wmax;
    float amin = 0, adef = 400, amax = 0;
    bool has_wght = font_axis_range(w->var_sfnt, w->var_len, FONT_AXIS_TAG('w', 'g', 'h', 't'), &amin, &adef, &amax);
    if (has_wght) {
        if (weight < (int)amin) weight = (int)amin;
        if (weight > (int)amax) weight = (int)amax;
    }
    for (int i = 0; i < w->ninst; i++) if (w->inst[i].weight == weight) return w->inst[i].slot;
    TextShaper *s = NULL;
    uint8_t *out = NULL;
    size_t olen = 0;
    FontAxisValue v = { FONT_AXIS_TAG('w', 'g', 'h', 't'), (float)weight };
    if (has_wght && font_instance(w->var_sfnt, w->var_len, &v, 1, &out, &olen))
        s = text_shaper_create_from_memory(out, olen, 0, FONT_PIXELS);
    if (!s) {
        uint8_t *copy = (uint8_t *)malloc(w->var_len);
        if (copy) memcpy(copy, w->var_sfnt, w->var_len);
        s = text_shaper_create_from_memory_ex(copy, w->var_len, 0, FONT_PIXELS,
                                              weight >= 600 && adef < 600 ? 1 : 0);
    }
    int slot = s ? add_slot_locked(s) : -1;
    if (slot >= 0 && w->ninst < (int)(sizeof(w->inst) / sizeof(w->inst[0]))) {
        w->inst[w->ninst].weight = weight;
        w->inst[w->ninst].slot = slot;
        w->ninst++;
    }
    if (getenv("CYBER_FONT_LOG"))
        fprintf(stderr, "[fonts] instance %s wght %d -> slot %d\n", w->family, weight, slot);
    return slot >= 0 ? slot : w->slot;
}

typedef struct {
    char *key;
    int slot;
} ResolveEntry;

#define RESOLVE_CACHE 2048

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static bool g_inited;
static TextShaper *_Atomic g_slots[FONT_SLOTS_MAX];
static int g_nslots;
static int g_override_slot = -1;

static SysFace *g_sys;
static int g_nsys, g_capsys;
static WebFace *g_web;
static int g_nweb, g_capweb;
static ResolveEntry g_cache[RESOLVE_CACHE];

/* ---- Platform defaults ---- */

typedef struct { const char *generic; const char *const *names; } GenericMap;

#ifdef _WIN32
static const char *const k_sans[] = { "arial", "segoe ui", NULL };
static const char *const k_serif[] = { "times new roman", "cambria", NULL };
static const char *const k_mono[] = { "consolas", "courier new", NULL };
static const char *const k_cursive[] = { "comic sans ms", NULL };
static const char *const k_fantasy[] = { "impact", NULL };
static const char *const k_system[] = { "segoe ui", "arial", NULL };
static const char *const k_emoji[] = { "segoe ui emoji", "segoe ui symbol", NULL };
static const char *const k_math[] = { "cambria math", NULL };
static const char *const k_fallbacks[] = {
    "segoe ui", "arial", "segoe ui symbol", "segoe ui emoji", "microsoft yahei", "yu gothic",
    "malgun gothic", "nirmala ui", "ebrima", "leelawadee ui", "gadugi", "microsoft himalaya",
    "mongolian baiti", "segoe ui historic", "myanmar text", "javanese text", "sylfaen",
    "cambria math", "simsun", "mingliu", NULL };
static const char *const k_font_dirs[] = { NULL };   /* filled at run time */
#elif defined(__APPLE__)
static const char *const k_sans[] = { "helvetica", "helvetica neue", "arial", NULL };
static const char *const k_serif[] = { "times", "times new roman", NULL };
static const char *const k_mono[] = { "menlo", "courier", "courier new", NULL };
static const char *const k_cursive[] = { "apple chancery", NULL };
static const char *const k_fantasy[] = { "papyrus", NULL };
static const char *const k_system[] = { "helvetica neue", "helvetica", NULL };
static const char *const k_emoji[] = { "apple symbols", NULL };
static const char *const k_math[] = { "stix two math", NULL };
static const char *const k_fallbacks[] = {
    "helvetica neue", "apple symbols", "pingfang sc", "hiragino sans", "apple sd gothic neo",
    "arial unicode ms", "kohinoor devanagari", "geeza pro", "thonburi", NULL };
static const char *const k_font_dirs[] = { "/System/Library/Fonts", "/Library/Fonts", "~/Library/Fonts", NULL };
#else
static const char *const k_sans[] = { "dejavu sans", "liberation sans", "noto sans", "freesans", "arial", NULL };
static const char *const k_serif[] = { "dejavu serif", "liberation serif", "noto serif", "freeserif", "times new roman", NULL };
static const char *const k_mono[] = { "dejavu sans mono", "liberation mono", "noto sans mono", "freemono", "courier new", NULL };
static const char *const k_cursive[] = { "comic neue", NULL };
static const char *const k_fantasy[] = { NULL };
static const char *const k_system[] = { "cantarell", "ubuntu", "noto sans", "dejavu sans", NULL };
static const char *const k_emoji[] = { "noto emoji", "symbola", NULL };
static const char *const k_math[] = { "dejavu math tex gyre", NULL };
static const char *const k_fallbacks[] = {
    "noto sans", "dejavu sans", "noto sans cjk sc", "noto sans symbols", "noto sans symbols2",
    "symbola", "freesans", "unifont", NULL };
static const char *const k_font_dirs[] = { "/usr/share/fonts", "/usr/local/share/fonts", "~/.fonts",
                                           "~/.local/share/fonts", NULL };
#endif

static const GenericMap k_generics[] = {
    { "sans-serif", k_sans }, { "serif", k_serif }, { "monospace", k_mono },
    { "cursive", k_cursive }, { "fantasy", k_fantasy }, { "system-ui", k_system },
    { "ui-sans-serif", k_system }, { "-apple-system", k_system }, { "blinkmacsystemfont", k_system },
    { "ui-serif", k_serif }, { "ui-monospace", k_mono }, { "ui-rounded", k_system },
    { "emoji", k_emoji }, { "math", k_math },
};

/* Families sites name that the OS ships under another name. */
static const struct { const char *name, *as; } k_aliases[] = {
    { "helvetica", "sans-serif" }, { "helvetica neue", "sans-serif" }, { "arial", "arial" },
    { "courier", "monospace" }, { "times", "serif" }, { "menlo", "monospace" },
    { "monaco", "monospace" }, { "sf mono", "monospace" }, { "segoe ui", "segoe ui" },
};

#define FALLBACK_COUNT ((int)(sizeof(k_fallbacks) / sizeof(k_fallbacks[0])) - 1)
static TextShaper *_Atomic g_fallback[64];
static _Atomic int g_fallback_tried[64];

/* ---- Small helpers ---- */

static void lower_copy(char *dst, size_t cap, const char *src, size_t n)
{
    size_t k = 0;
    for (size_t i = 0; i < n && k + 1 < cap; i++) dst[k++] = (char)tolower((unsigned char)src[i]);
    dst[k] = '\0';
}

static int add_slot_locked(TextShaper *s)
{
    if (!s || g_nslots >= FONT_SLOTS_MAX) { text_shaper_destroy(s); return -1; }
    int slot = g_nslots++;
    text_shaper_set_slot(s, slot);
    atomic_store(&g_slots[slot], s);
    return slot;
}

static void cache_clear_locked(void)
{
    for (int i = 0; i < RESOLVE_CACHE; i++) { free(g_cache[i].key); g_cache[i].key = NULL; }
}

static uint32_t str_hash(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return h;
}

/* ---- System font index ---- */

static bool read_at(FILE *f, long off, void *buf, size_t n)
{
    return fseek(f, off, SEEK_SET) == 0 && fread(buf, 1, n, f) == n;
}

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* Name `id` from a name table (UTF-16BE Windows names preferred, US English
 * first), lowercased ASCII/UTF-8 into out. */
static bool name_lookup(const uint8_t *name, size_t len, int id, char *out, size_t cap)
{
    if (len < 6) return false;
    int count = be16(name + 2), str_off = be16(name + 4);
    int best = -1, best_score = -1;
    for (int i = 0; i < count && 6 + (size_t)(i + 1) * 12 <= len; i++) {
        const uint8_t *r = name + 6 + i * 12;
        int plat = be16(r), enc = be16(r + 2), lang = be16(r + 4), nid = be16(r + 6);
        if (nid != id) continue;
        int score = -1;
        if (plat == 3 && (enc == 1 || enc == 10)) score = lang == 0x409 ? 3 : 2;
        else if (plat == 0) score = 1;
        else if (plat == 1 && enc == 0) score = lang == 0 ? 1 : 0;
        if (score > best_score) { best_score = score; best = i; }
    }
    if (best < 0) return false;
    const uint8_t *r = name + 6 + best * 12;
    int plat = be16(r), slen = be16(r + 8), soff = be16(r + 10);
    if ((size_t)str_off + soff + slen > len) return false;
    const uint8_t *s = name + str_off + soff;
    size_t k = 0;
    if (plat == 1) {
        for (int i = 0; i < slen && k + 1 < cap; i++) out[k++] = (char)tolower(s[i] < 0x80 ? s[i] : '?');
    } else {
        for (int i = 0; i + 1 < slen && k + 4 < cap; i += 2) {
            uint32_t c = be16(s + i);
            if (c < 0x80) out[k++] = (char)tolower((int)c);
            else if (c < 0x800) { out[k++] = (char)(0xC0 | (c >> 6)); out[k++] = (char)(0x80 | (c & 0x3F)); }
            else {
                out[k++] = (char)(0xE0 | (c >> 12));
                out[k++] = (char)(0x80 | ((c >> 6) & 0x3F));
                out[k++] = (char)(0x80 | (c & 0x3F));
            }
        }
    }
    out[k] = '\0';
    return k > 0;
}

static void index_face(FILE *f, const char *path, long base, int index)
{
    uint8_t hdr[12];
    if (!read_at(f, base, hdr, 12)) return;
    int ntables = be16(hdr + 4);
    if (ntables <= 0 || ntables > 200) return;
    uint8_t dir[200 * 16];
    if (!read_at(f, base + 12, dir, (size_t)ntables * 16)) return;
    long name_off = 0, os2_off = 0, head_off = 0;
    uint32_t name_len = 0, os2_len = 0;
    for (int i = 0; i < ntables; i++) {
        uint32_t tag = be32(dir + i * 16), off = be32(dir + i * 16 + 8), len = be32(dir + i * 16 + 12);
        if (tag == 0x6E616D65u) { name_off = (long)off; name_len = len; }        /* name */
        else if (tag == 0x4F532F32u) { os2_off = (long)off; os2_len = len; }   /* OS/2 */
        else if (tag == 0x68656164u) head_off = (long)off;                     /* head */
    }
    if (!name_off || name_len < 6 || name_len > (1u << 20)) return;
    uint8_t *name = (uint8_t *)malloc(name_len);
    if (!name) return;
    SysFace face;
    memset(&face, 0, sizeof(face));
    bool ok = read_at(f, name_off, name, name_len) &&
              name_lookup(name, name_len, 1, face.family, sizeof(face.family));
    if (ok) name_lookup(name, name_len, 16, face.typo, sizeof(face.typo));
    free(name);
    if (!ok) return;
    face.weight = 400;
    uint8_t os2[64];
    if (os2_off && os2_len >= 64 && read_at(f, os2_off, os2, 64)) {
        face.weight = be16(os2 + 4);
        face.italic = (be16(os2 + 62) & 0x0201) != 0;
    } else if (head_off) {
        uint8_t head[46];
        if (read_at(f, head_off, head, 46)) face.italic = (be16(head + 44) & 2) != 0;
    }
    if (face.weight < 1 || face.weight > 1000) face.weight = 400;
    face.path = strdup(path);
    face.index = index;
    face.slot = -1;
    if (g_nsys == g_capsys) {
        int nc = g_capsys ? g_capsys * 2 : 256;
        SysFace *ns = (SysFace *)realloc(g_sys, (size_t)nc * sizeof(*ns));
        if (!ns) { free(face.path); return; }
        g_sys = ns;
        g_capsys = nc;
    }
    g_sys[g_nsys++] = face;
}

static void index_file(const char *path)
{
    size_t n = strlen(path);
    if (n < 5) return;
    const char *ext = path + n - 4;
    if (strcasecmp(ext, ".ttf") != 0 && strcasecmp(ext, ".otf") != 0 && strcasecmp(ext, ".ttc") != 0)
        return;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    uint8_t hdr[12];
    if (read_at(f, 0, hdr, 12)) {
        if (be32(hdr) == 0x74746366u) {                              /* ttcf */
            uint32_t nfonts = be32(hdr + 8);
            if (nfonts > 0 && nfonts < 64) {
                uint8_t offs[64 * 4];
                if (read_at(f, 12, offs, nfonts * 4))
                    for (uint32_t i = 0; i < nfonts; i++) index_face(f, path, (long)be32(offs + i * 4), (int)i);
            }
        } else {
            index_face(f, path, 0, 0);
        }
    }
    fclose(f);
}

static void index_dir(const char *dir, int depth)
{
    if (depth > 4) return;
#ifdef _WIN32
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.') continue;
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) index_dir(path, depth + 1);
        else index_file(path);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) index_dir(path, depth + 1);
        else index_file(path);
    }
    closedir(d);
#endif
}

void font_registry_init(void)
{
    pthread_mutex_lock(&g_mu);
    if (g_inited) { pthread_mutex_unlock(&g_mu); return; }
    g_inited = true;
#ifdef _WIN32
    char win[MAX_PATH];
    UINT n = GetWindowsDirectoryA(win, sizeof(win));
    if (n > 0 && n < sizeof(win)) {
        char dir[MAX_PATH];
        snprintf(dir, sizeof(dir), "%s\\Fonts", win);
        index_dir(dir, 0);
    }
    const char *local = getenv("LOCALAPPDATA");
    if (local) {
        char dir[MAX_PATH];
        snprintf(dir, sizeof(dir), "%s\\Microsoft\\Windows\\Fonts", local);
        index_dir(dir, 0);
    }
    (void)k_font_dirs;
#else
    const char *home = getenv("HOME");
    for (int i = 0; k_font_dirs[i]; i++) {
        char dir[1024];
        if (k_font_dirs[i][0] == '~') {
            if (!home) continue;
            snprintf(dir, sizeof(dir), "%s%s", home, k_font_dirs[i] + 1);
        } else {
            snprintf(dir, sizeof(dir), "%s", k_font_dirs[i]);
        }
        index_dir(dir, 0);
    }
#endif
    pthread_mutex_unlock(&g_mu);
}

/* ---- Matching ---- */

/* CSS Fonts §5.2 weight distance: lower is better. */
static int weight_distance(int want, int wmin, int wmax)
{
    if (want >= wmin && want <= wmax) return 0;
    int w = want < wmin ? wmin : wmax;
    if (want < 400) return w < want ? want - w : 1000 + w - want;
    if (want > 500) return w > want ? w - want : 1000 + want - w;
    if (w > want && w <= 500) return w - want;
    if (w < want) return 500 + want - w;
    return 1000 + w - want;
}

static TextShaper *load_sys_locked(SysFace *f)
{
    if (f->slot >= 0) return atomic_load(&g_slots[f->slot]);
    if (f->slot == -2) return NULL;
    TextShaper *s = NULL;
#ifdef _WIN32
    /* Mapped, not read: a system font's pages are the file's (shared, and
       dropped under memory pressure), not the process's private memory. */
    HANDLE fh = CreateFileA(f->path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER sz;
        HANDLE map = GetFileSizeEx(fh, &sz) && sz.QuadPart > 0
            ? CreateFileMappingA(fh, NULL, PAGE_READONLY, 0, 0, NULL) : NULL;
        if (map) {
            uint8_t *view = (uint8_t *)MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0);
            if (view)
                s = text_shaper_create_from_mapped(view, (size_t)sz.QuadPart, f->index, FONT_PIXELS);
            CloseHandle(map);   /* the view keeps the mapping */
        }
        CloseHandle(fh);
    }
#else
    FILE *fp = fopen(f->path, "rb");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        long sz = ftell(fp);
        rewind(fp);
        uint8_t *data = sz > 0 ? (uint8_t *)malloc((size_t)sz) : NULL;
        if (data && fread(data, 1, (size_t)sz, fp) == (size_t)sz)
            s = text_shaper_create_from_memory(data, (size_t)sz, f->index, FONT_PIXELS);
        else
            free(data);
        fclose(fp);
    }
#endif
    f->slot = s ? add_slot_locked(s) : -2;
    if (f->slot < 0) f->slot = -2;
    return f->slot >= 0 ? atomic_load(&g_slots[f->slot]) : NULL;
}

/* Best system face of `family` for weight/style; its slot or -1. */
static int match_system_locked(const char *family, int weight, bool italic)
{
    int best = -1, best_score = 1 << 30;
    for (int i = 0; i < g_nsys; i++) {
        SysFace *f = &g_sys[i];
        if (f->slot == -2) continue;
        if (strcmp(f->family, family) != 0 && strcmp(f->typo, family) != 0) continue;
        int score = weight_distance(weight, f->weight, f->weight) + (f->italic != italic ? 10000 : 0);
        /* A face whose legacy family is exactly the name (Arial vs "Arial Black"). */
        if (strcmp(f->family, family) != 0) score += 1;
        if (score < best_score) { best_score = score; best = i; }
    }
    if (best < 0) return -1;
    return load_sys_locked(&g_sys[best]) ? g_sys[best].slot : -1;
}

static bool range_has(const WebFace *w, uint32_t cp)
{
    if (!w->ranges) return true;
    for (int i = 0; i < w->nranges; i++)
        if (cp >= w->ranges[2 * i] && cp <= w->ranges[2 * i + 1]) return true;
    return false;
}

/* Best web face of `family`; -1 when there is none, -2 when the best one is
 * still loading (it has been queued). */
static int match_web_locked(const char *family, int weight, bool italic)
{
    int best = -1, best_score = 1 << 30;
    for (int i = 0; i < g_nweb; i++) {
        WebFace *w = &g_web[i];
        if (w->state == WEB_FAILED || strcmp(w->family, family) != 0) continue;
        /* Text is mostly Latin: of faces split by unicode-range, the one
         * covering basic Latin wins; other subsets come from fallback. */
        int score = weight_distance(weight, w->wmin, w->wmax) + (w->italic != italic ? 10000 : 0) +
                    (range_has(w, 'a') ? 0 : 100000);
        if (score < best_score) { best_score = score; best = i; }
    }
    if (best < 0) return -1;
    WebFace *w = &g_web[best];
    if (w->state == WEB_READY) return w->var_sfnt ? web_instance_locked(w, weight) : w->slot;
    if (w->state == WEB_IDLE) w->state = WEB_QUEUED;
    return -2;
}

static int resolve_name_locked(const char *name, int weight, bool italic, bool *pending)
{
    int s = match_web_locked(name, weight, italic);
    if (s >= 0) return s;
    if (s == -2) { *pending = true; return -1; }
    for (size_t g = 0; g < sizeof(k_generics) / sizeof(k_generics[0]); g++) {
        if (strcmp(name, k_generics[g].generic) != 0) continue;
        for (int i = 0; k_generics[g].names[i]; i++) {
            s = match_system_locked(k_generics[g].names[i], weight, italic);
            if (s >= 0) return s;
        }
        return -1;
    }
    s = match_system_locked(name, weight, italic);
    if (s >= 0) return s;
    for (size_t a = 0; a < sizeof(k_aliases) / sizeof(k_aliases[0]); a++)
        if (strcmp(name, k_aliases[a].name) == 0 && strcmp(name, k_aliases[a].as) != 0)
            return resolve_name_locked(k_aliases[a].as, weight, italic, pending);
    return -1;
}

int font_registry_resolve(const char *family_list, int weight, bool italic)
{
    font_registry_init();
    if (weight <= 0) weight = 400;
    char key[256];
    snprintf(key, sizeof(key), "%d|%d|%s", weight, italic ? 1 : 0, family_list ? family_list : "");
    uint32_t h = str_hash(key) & (RESOLVE_CACHE - 1);
    pthread_mutex_lock(&g_mu);
    if (g_override_slot >= 0) { pthread_mutex_unlock(&g_mu); return g_override_slot; }
    for (int i = 0; i < 8; i++) {
        ResolveEntry *e = &g_cache[(h + i) & (RESOLVE_CACHE - 1)];
        if (!e->key) break;
        if (strcmp(e->key, key) == 0) { int s = e->slot; pthread_mutex_unlock(&g_mu); return s; }
    }
    bool pending = false;
    int slot = -1;
    const char *p = family_list ? family_list : "";
    while (*p && slot < 0) {
        while (*p == ',' || isspace((unsigned char)*p)) p++;
        if (!*p) break;
        const char *st = p, *en;
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            st = p;
            while (*p && *p != q) p++;
            en = p;
            if (*p) p++;
        } else {
            while (*p && *p != ',') p++;
            en = p;
            while (en > st && isspace((unsigned char)en[-1])) en--;
        }
        char name[FAMILY_MAX];
        lower_copy(name, sizeof(name), st, (size_t)(en - st));
        if (name[0]) slot = resolve_name_locked(name, weight, italic, &pending);
        while (*p && *p != ',') p++;
    }
    if (slot < 0) slot = resolve_name_locked("sans-serif", weight, italic, &pending);
    if (slot < 0) {
        /* Last resort: any font at all. */
        for (int i = 0; i < g_nslots && slot < 0; i++) if (atomic_load(&g_slots[i])) slot = i;
        for (int i = 0; i < g_nsys && slot < 0; i++) if (load_sys_locked(&g_sys[i])) slot = g_sys[i].slot;
    }
    if (!pending) {
        for (int i = 0; i < 8; i++) {
            ResolveEntry *e = &g_cache[(h + i) & (RESOLVE_CACHE - 1)];
            if (!e->key) { e->key = strdup(key); e->slot = slot; break; }
        }
    }
    pthread_mutex_unlock(&g_mu);
    return slot;
}

int font_registry_default_slot(void)
{
    return font_registry_resolve("sans-serif", 400, false);
}

TextShaper *font_registry_slot(int slot)
{
    if (slot < 0 || slot >= FONT_SLOTS_MAX) return NULL;
    return atomic_load(&g_slots[slot]);
}

bool font_registry_set_default_file(const char *path)
{
    TextShaper *s = text_shaper_create(path, FONT_PIXELS);
    if (!s) return false;
    pthread_mutex_lock(&g_mu);
    g_override_slot = add_slot_locked(s);
    pthread_mutex_unlock(&g_mu);
    return g_override_slot >= 0;
}

/* ---- Fallback chain ---- */

int font_registry_fallback_count(void)
{
    return FALLBACK_COUNT < 64 ? FALLBACK_COUNT : 64;
}

TextShaper *font_registry_fallback(int k)
{
    if (k < 0 || k >= font_registry_fallback_count()) return NULL;
    TextShaper *s = atomic_load(&g_fallback[k]);
    if (s || atomic_load(&g_fallback_tried[k])) return s;
    font_registry_init();
    pthread_mutex_lock(&g_mu);
    if (!atomic_load(&g_fallback_tried[k])) {
        int slot = match_system_locked(k_fallbacks[k], 400, false);
        if (slot >= 0) atomic_store(&g_fallback[k], atomic_load(&g_slots[slot]));
        atomic_store(&g_fallback_tried[k], 1);
    }
    s = atomic_load(&g_fallback[k]);
    pthread_mutex_unlock(&g_mu);
    return s;
}

/* ---- @font-face ---- */

static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && (isspace((unsigned char)*p) || *p == ',')) p++;
    return p;
}

/* unicode-range: U+0-FF, U+131, U+4??. */
static void parse_unicode_range(WebFace *w, const char *v, const char *end)
{
    const char *p = v;
    while (p < end) {
        p = skip_ws(p, end);
        if (p + 2 > end || (p[0] != 'U' && p[0] != 'u') || p[1] != '+') break;
        p += 2;
        uint32_t lo = 0, hi = 0;
        int digits = 0, wild = 0;
        while (p < end && (isxdigit((unsigned char)*p) || *p == '?')) {
            if (*p == '?') { lo = lo * 16; hi = hi * 16 + 15; wild++; }
            else {
                int d = isdigit((unsigned char)*p) ? *p - '0' : tolower((unsigned char)*p) - 'a' + 10;
                lo = lo * 16 + (uint32_t)d;
                hi = hi * 16 + (uint32_t)d;
            }
            p++;
            digits++;
        }
        if (!digits) break;
        if (!wild && p < end && *p == '-') {
            p++;
            hi = 0;
            while (p < end && isxdigit((unsigned char)*p)) {
                int d = isdigit((unsigned char)*p) ? *p - '0' : tolower((unsigned char)*p) - 'a' + 10;
                hi = hi * 16 + (uint32_t)d;
                p++;
            }
        }
        uint32_t *nr = (uint32_t *)realloc(w->ranges, (size_t)(w->nranges + 1) * 2 * sizeof(uint32_t));
        if (!nr) break;
        w->ranges = nr;
        w->ranges[2 * w->nranges] = lo;
        w->ranges[2 * w->nranges + 1] = hi;
        w->nranges++;
        while (p < end && *p != ',') p++;
    }
}

static int parse_weight_word(const char *s, const char *end)
{
    while (s < end && isspace((unsigned char)*s)) s++;
    if (end - s >= 4 && strncasecmp(s, "bold", 4) == 0) return 700;
    if (end - s >= 6 && strncasecmp(s, "normal", 6) == 0) return 400;
    int v = atoi(s);
    return v > 0 ? v : 400;
}

/* The first src entry this loader can use: url() with a supported (or no)
 * format(), malloc'd and resolved against base. */
static char *pick_src(const char *v, const char *end, const char *base)
{
    const char *p = v;
    while (p < end) {
        const char *u = NULL;
        for (const char *q = p; q + 4 <= end; q++)
            if (strncasecmp(q, "url(", 4) == 0) { u = q; break; }
        if (!u) return NULL;
        const char *s = u + 4;
        while (s < end && isspace((unsigned char)*s)) s++;
        char quote = (s < end && (*s == '"' || *s == '\'')) ? *s++ : 0;
        const char *e = s;
        while (e < end && (quote ? *e != quote : *e != ')')) e++;
        const char *after = e;
        while (after < end && *after != ')') after++;
        if (after < end) after++;
        const char *comma = after;
        int depth = 0;
        while (comma < end && (depth > 0 || *comma != ',')) {
            if (*comma == '(') depth++;
            else if (*comma == ')') depth--;
            comma++;
        }
        bool usable = true;
        for (const char *q = after; q + 7 <= comma; q++) {
            if (strncasecmp(q, "format(", 7) == 0) {
                usable = false;
                static const char *const ok[] = { "woff2", "woff", "truetype", "opentype", "ttf", "otf" };
                const char *fe = q + 7;
                while (fe < comma && (*fe == '"' || *fe == '\'' || isspace((unsigned char)*fe))) fe++;
                for (size_t k = 0; k < sizeof(ok) / sizeof(ok[0]); k++) {
                    size_t n = strlen(ok[k]);
                    if ((size_t)(comma - fe) >= n && strncasecmp(fe, ok[k], n) == 0 &&
                        !isalnum((unsigned char)fe[n])) usable = true;
                }
                if ((size_t)(comma - fe) >= 9 && strncasecmp(fe, "woff2-var", 9) == 0) usable = false;
                break;
            }
        }
        if (usable && e > s) {
            char *raw = (char *)malloc((size_t)(e - s) + 1);
            if (!raw) return NULL;
            memcpy(raw, s, (size_t)(e - s));
            raw[e - s] = '\0';
            if (strncmp(raw, "data:", 5) == 0 || !base || !base[0]) return raw;
            char *abs = wurl_resolve(base, raw);
            free(raw);
            return abs;
        }
        p = comma < end ? comma + 1 : end;
    }
    return NULL;
}

static void add_face_rule(const char *body, const char *end, const char *base)
{
    WebFace w;
    memset(&w, 0, sizeof(w));
    w.wmin = w.wmax = 400;
    w.slot = -1;

    char *src = NULL;
    const char *p = body;
    while (p < end) {
        while (p < end && (isspace((unsigned char)*p) || *p == ';')) p++;
        const char *ns = p;
        while (p < end && *p != ':' && *p != ';') p++;
        if (p >= end || *p != ':') break;
        const char *ne = p;
        while (ne > ns && isspace((unsigned char)ne[-1])) ne--;
        p++;
        const char *vs = p;
        char quote = 0;
        int depth = 0;
        while (p < end) {
            if (quote) { if (*p == quote) quote = 0; }
            else if (*p == '"' || *p == '\'') quote = *p;
            else if (*p == '(') depth++;
            else if (*p == ')') depth--;
            else if (*p == ';' && depth <= 0) break;
            p++;
        }
        const char *ve = p;
        while (vs < ve && isspace((unsigned char)*vs)) vs++;
        while (ve > vs && isspace((unsigned char)ve[-1])) ve--;
        size_t nl = (size_t)(ne - ns);
        if (nl == 11 && strncasecmp(ns, "font-family", 11) == 0) {
            const char *a = vs, *b = ve;
            if (b > a && (*a == '"' || *a == '\'')) { a++; if (b > a && (b[-1] == '"' || b[-1] == '\'')) b--; }
            lower_copy(w.family, sizeof(w.family), a, (size_t)(b - a));
        } else if (nl == 3 && strncasecmp(ns, "src", 3) == 0) {
            free(src);
            src = pick_src(vs, ve, base);
        } else if (nl == 11 && strncasecmp(ns, "font-weight", 11) == 0) {
            w.wmin = parse_weight_word(vs, ve);
            const char *sp = vs;
            while (sp < ve && !isspace((unsigned char)*sp)) sp++;
            w.wmax = sp < ve ? parse_weight_word(sp, ve) : w.wmin;
            if (w.wmax < w.wmin) { int t = w.wmin; w.wmin = w.wmax; w.wmax = t; }
        } else if (nl == 10 && strncasecmp(ns, "font-style", 10) == 0) {
            w.italic = (ve - vs >= 6 && strncasecmp(vs, "italic", 6) == 0) ||
                       (ve - vs >= 7 && strncasecmp(vs, "oblique", 7) == 0);
        } else if (nl == 13 && strncasecmp(ns, "unicode-range", 13) == 0) {
            parse_unicode_range(&w, vs, ve);
        }
    }
    if (!w.family[0] || !src) { free(src); free(w.ranges); return; }
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_nweb; i++) {
        if (strcmp(g_web[i].url, src) == 0 && strcmp(g_web[i].family, w.family) == 0 &&
            g_web[i].wmin == w.wmin && g_web[i].italic == w.italic) {
            pthread_mutex_unlock(&g_mu);
            free(src);
            free(w.ranges);
            return;
        }
    }
    if (g_nweb == g_capweb) {
        int nc = g_capweb ? g_capweb * 2 : 64;
        WebFace *nw = (WebFace *)realloc(g_web, (size_t)nc * sizeof(*nw));
        if (!nw) { pthread_mutex_unlock(&g_mu); free(src); free(w.ranges); return; }
        g_web = nw;
        g_capweb = nc;
    }
    w.url = src;
    g_web[g_nweb++] = w;
    if (getenv("CYBER_FONT_LOG"))
        fprintf(stderr, "[fonts] @font-face %s %d-%d%s %d ranges: %.100s\n", w.family, w.wmin, w.wmax,
                w.italic ? " italic" : "", w.nranges, src);
    cache_clear_locked();   /* a new face can change earlier answers */
    /* A face for Latin text is all but certain to be used: start its
     * download now (faces of one family often share the file), so the
     * layout that first asks for it does not wait on the network. */
    bool latin = w.nranges == 0;
    for (int i = 0; i < w.nranges && !latin; i++) latin = w.ranges[2 * i] <= 0x41 && w.ranges[2 * i + 1] >= 0x41;
    bool prefetch = latin && strncmp(src, "data:", 5) != 0;
    char *url = prefetch ? strdup(src) : NULL;
    pthread_mutex_unlock(&g_mu);
    if (url) dom_fetch_prefetch_first(url);
    free(url);
}

void font_registry_scan_css(const char *css, size_t len, const char *base_url)
{
    if (!css || len < 10) return;
    /* Layouts hand the same sheets over again and again: a text seen before
     * (same length and sampled bytes, and base) is not rescanned. */
    {
        uint64_t key = (uint64_t)len * 1099511628211ull;
        for (int k = 0; k < 64; k++) {
            key ^= (unsigned char)css[(len - 1) * (size_t)k / 63];
            key *= 1099511628211ull;
        }
        for (const char *b = base_url ? base_url : ""; *b; b++) { key ^= (unsigned char)*b; key *= 1099511628211ull; }
        static uint64_t seen[1024];
        static int nseen;
        pthread_mutex_lock(&g_mu);
        for (int k = 0; k < nseen; k++) {
            if (seen[k] == key) { pthread_mutex_unlock(&g_mu); return; }
        }
        seen[nseen < 1024 ? nseen++ : (int)(key % 1024)] = key;
        pthread_mutex_unlock(&g_mu);
    }
    const char *end = css + len;
    const char *p = css;
    for (;;) {
        const char *at = NULL;
        for (const char *q = p; q + 10 <= end; q++) {
            if (*q == '@' && strncasecmp(q + 1, "font-face", 9) == 0) { at = q; break; }
        }
        if (!at) return;
        const char *open = at + 10;
        while (open < end && *open != '{') open++;
        if (open >= end) return;
        const char *close = open + 1;
        int depth = 1;
        while (close < end && depth > 0) {
            if (*close == '{') depth++;
            else if (*close == '}') depth--;
            if (depth > 0) close++;
        }
        add_face_rule(open + 1, close, base_url);
        p = close < end ? close + 1 : end;
    }
}

/* ---- Fetching ---- */

static int b64v(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

static bool fetch_bytes(const char *url, uint8_t **out, size_t *len)
{
    if (strncmp(url, "data:", 5) == 0) {
        const char *comma = strchr(url, ',');
        if (!comma || !strstr(url, ";base64")) return false;
        size_t n = strlen(comma + 1);
        uint8_t *b = (uint8_t *)malloc(n * 3 / 4 + 3);
        if (!b) return false;
        size_t k = 0;
        uint32_t acc = 0;
        int bits = 0;
        for (const char *p = comma + 1; *p; p++) {
            int v = b64v((unsigned char)*p);
            if (v < 0) continue;
            acc = (acc << 6) | (uint32_t)v;
            bits += 6;
            if (bits >= 8) { bits -= 8; b[k++] = (uint8_t)(acc >> bits); }
        }
        *out = b;
        *len = k;
        return k > 0;
    }
    HttpBuffer buf = { 0 };
    char err[256] = { 0 };
    if (!dom_fetch_script(url, &buf, err, sizeof(err)) || !buf.data || buf.size == 0) {
        fprintf(stderr, "[fonts] fetch failed %.120s: %s\n", url, err);
        http_free_buffer(&buf);
        return false;
    }
    if (buf.headers) { free(buf.headers); buf.headers = NULL; }
    *out = (uint8_t *)buf.data;
    *len = buf.size;
    return true;
}

typedef struct {
    char *url;
    uint8_t *sfnt;
    size_t len;
} FetchJob;

static void *fetch_thread(void *arg)
{
    FetchJob *j = (FetchJob *)arg;
    uint8_t *raw = NULL;
    size_t rlen = 0;
    if (fetch_bytes(j->url, &raw, &rlen)) {
        if (!font_decode_web(raw, rlen, &j->sfnt, &j->len)) {
            fprintf(stderr, "[fonts] could not decode %.120s (%zu bytes)\n", j->url, rlen);
            j->sfnt = NULL;
        }
        free(raw);
    }
    return NULL;
}

int font_registry_load_pending(void)
{
    pthread_mutex_lock(&g_mu);
    int n = 0;
    for (int i = 0; i < g_nweb; i++) if (g_web[i].state == WEB_QUEUED) n++;
    if (!n) { pthread_mutex_unlock(&g_mu); return 0; }
    FetchJob *jobs = (FetchJob *)calloc((size_t)n, sizeof(*jobs));
    int *which = (int *)calloc((size_t)n, sizeof(int));
    if (!jobs || !which) { free(jobs); free(which); pthread_mutex_unlock(&g_mu); return 0; }
    int k = 0;
    for (int i = 0; i < g_nweb; i++) {
        if (g_web[i].state != WEB_QUEUED) continue;
        jobs[k].url = strdup(g_web[i].url);
        which[k++] = i;
    }
    pthread_mutex_unlock(&g_mu);

    /* One fetch per URL (Google Fonts serves every weight of a variable
     * family from the same file), in parallel, at most 8 at a time. */
    int *src = (int *)malloc((size_t)n * sizeof(int));
    int *uniq = (int *)malloc((size_t)n * sizeof(int));
    int nu = 0;
    for (int t = 0; src && uniq && t < n; t++) {
        src[t] = t;
        for (int u = 0; u < t; u++) if (strcmp(jobs[u].url, jobs[t].url) == 0) { src[t] = src[u]; break; }
        if (src[t] == t) uniq[nu++] = t;
    }
    for (int base = 0; src && uniq && base < nu; base += 8) {
        pthread_t th[8];
        int started[8] = { 0 };
        int m = nu - base < 8 ? nu - base : 8;
        for (int t = 0; t < m; t++)
            started[t] = pthread_create(&th[t], NULL, fetch_thread, &jobs[uniq[base + t]]) == 0;
        for (int t = 0; t < m; t++) {
            if (started[t]) pthread_join(th[t], NULL);
            else fetch_thread(&jobs[uniq[base + t]]);
        }
    }

    int ready = 0;
    pthread_mutex_lock(&g_mu);
    for (int t = 0; t < n; t++) {
        WebFace *w = &g_web[which[t]];
        const FetchJob *j = src ? &jobs[src[t]] : &jobs[t];
        w->slot = -1;
        if (j->sfnt && font_is_variable(j->sfnt, j->len)) {
            /* Keep the file; instances are cut per weight on demand. */
            w->var_sfnt = (uint8_t *)malloc(j->len);
            if (w->var_sfnt) {
                memcpy(w->var_sfnt, j->sfnt, j->len);
                w->var_len = j->len;
                w->slot = web_instance_locked(w, w->wmin <= 400 && w->wmax >= 400 ? 400 : w->wmin);
            }
        } else if (j->sfnt) {
            uint8_t *copy = (uint8_t *)malloc(j->len);
            if (copy) memcpy(copy, j->sfnt, j->len);
            TextShaper *s = text_shaper_create_from_memory(copy, j->len, 0, FONT_PIXELS);
            w->slot = s ? add_slot_locked(s) : -1;
        }
        w->state = w->slot >= 0 ? WEB_READY : WEB_FAILED;
        if (w->state == WEB_READY) ready++;
        if (getenv("CYBER_FONT_LOG"))
            fprintf(stderr, "[fonts] %s %s %d-%d%s%s from %.100s\n", w->state == WEB_READY ? "loaded" : "FAILED",
                    w->family, w->wmin, w->wmax, w->italic ? " italic" : "", w->var_sfnt ? " (variable)" : "",
                    w->url);
    }
    for (int t = 0; t < n; t++) { free(jobs[t].url); free(jobs[t].sfnt); }
    free(src);
    free(uniq);
    cache_clear_locked();
    pthread_mutex_unlock(&g_mu);
    free(jobs);
    free(which);
    return ready;
}

bool font_registry_family_file(const char *family, char *path, size_t cap, int *face_index)
{
    font_registry_init();
    char name[FAMILY_MAX];
    lower_copy(name, sizeof(name), family ? family : "", strlen(family ? family : ""));
    const char *const *names = NULL;
    for (size_t g = 0; g < sizeof(k_generics) / sizeof(k_generics[0]); g++)
        if (strcmp(name, k_generics[g].generic) == 0) names = k_generics[g].names;
    const char *one[2] = { name, NULL };
    if (!names) names = one;
    bool found = false;
    pthread_mutex_lock(&g_mu);
    for (int k = 0; names[k] && !found; k++) {
        int best = -1, best_score = 1 << 30;
        for (int i = 0; i < g_nsys; i++) {
            const SysFace *f = &g_sys[i];
            if (strcmp(f->family, names[k]) != 0 && strcmp(f->typo, names[k]) != 0) continue;
            int score = weight_distance(400, f->weight, f->weight) + (f->italic ? 10000 : 0);
            if (score < best_score) { best_score = score; best = i; }
        }
        if (best >= 0) {
            snprintf(path, cap, "%s", g_sys[best].path);
            if (face_index) *face_index = g_sys[best].index;
            found = true;
        }
    }
    pthread_mutex_unlock(&g_mu);
    return found;
}

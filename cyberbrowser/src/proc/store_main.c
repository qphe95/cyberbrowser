/* store_main.c - cb_store.exe: the storage service (SANDBOX_DESIGN.md
 * sections 2.5 and 3.3), the Win32 shim around store_core.c.
 *
 * Started by the coordinator with its control channel:
 *   cb_store.exe --chan S E0 E1 [--root HEX]
 *
 * Owns browser_data/sites/: the folder --root names (UTF-8, as hex), which
 * the coordinator made and gave the sandboxed service access to; without
 * it, next to the executables (or, if that can't be written,
 * %LOCALAPPDATA%\cyberbrowser\browser_data\sites).  The coordinator
 * grants it a channel per tab, badged with the tab's site, and one to the
 * network process.  A client names files by paths relative to its site's
 * folder; store_core.c reads each request, decides what is allowed and makes
 * the file's path, and every file is opened refusing reparse points and hard
 * links and checked to really be inside that folder.  Writes replace a file
 * whole, within the site's quota.
 *
 * It is one thread for every client, so it never waits on one: a reply goes
 * as far as the client's channel has room and the rest later, and that
 * client's next request waits until it has all gone.  A client that leaves a
 * reply unread for REPLY_STALL_MS is dropped (SANDBOX_DESIGN.md 8.7.2).
 *
 * M2 returns a file's content on a read rather than a read-only handle to it
 * (section 3.3): the files written so far are small. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#include "chan.h"
#include "bigmsg.h"
#include "proto.h"
#include "spawn.h"
#include "site.h"
#include "store_core.h"

#define MAX_CLIENTS 62              /* + the control channel: 63 waits */
#define REPLY_STALL_MS 10000        /* a reply no part of which goes for this long: drop the client */
#define RETRY_MAX_MS 250            /* the longest wait before trying a full channel again */

/* A reply on its way: StoreResult + data, sent as far as the client's ring
 * has room, the rest later (send_reply). */
typedef struct Reply {
    bool busy;
    bool started;                   /* its first part (type, total size) is sent */
    StoreResult head;
    uint8_t *data;                  /* owned */
    uint32_t len, sent;             /* of head + data */
    DWORD progress;                 /* when a part last went */
    DWORD retry_at, backoff;        /* when to try again, after the channel was full */
} Reply;

typedef struct Client {
    uint32_t id;
    uint32_t kind;                  /* GRANT_TAB or GRANT_NET */
    char site[SITE_MAX];            /* a tab's badge; "" = opaque: nothing allowed */
    Chan chan;                      /* side 1: the storage service */
    BigRx rx;
    Reply out;                      /* while busy, its next request waits in its channel */
} Client;

/* Bytes each site uses, counted from its folder when first needed. */
typedef struct Usage {
    char site[SITE_MAX];
    uint64_t used;                  /* its quota's: the files outside cache/ */
    uint64_t cache_used;            /* the files under cache/ */
    struct Usage *next;
} Usage;

/* The caches (the network process's HTTP cache under cache/http/, a tab's
 * bytecode cache under cache/code/) live in the site's folder, apart from its
 * quota (store_quota_ok, for what the site stores itself): within
 * CACHE_BUDGET, least recently used going first. */
#define CACHE_BUDGET (256ull << 20)
#define CACHE_ENTRY_MAX (32u << 20)

static Chan g_ctl;
static Client *g_clients[MAX_CLIENTS];
static Usage *g_usage;
static wchar_t g_root[MAX_PATH];            /* ...\browser_data\sites */
static wchar_t g_root_final[MAX_PATH * 2];  /* as GetFinalPathNameByHandle names it */

/* ---- paths ------------------------------------------------------------------- */

static bool writable_dir(const wchar_t *dir) {
    wchar_t probe[MAX_PATH * 2];
    _snwprintf(probe, MAX_PATH * 2, L"%ls\\.probe~", dir);
    HANDLE h = CreateFileW(probe, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

static bool make_root(const wchar_t *base) {
    wchar_t p[MAX_PATH];
    /* Microsoft's _snwprintf leaves no NUL when it cuts short: refuse a base too long. */
    int n = _snwprintf(g_root, MAX_PATH, L"%lsbrowser_data\\sites", base);
    if (n < 0 || n >= MAX_PATH) {
        g_root[0] = 0;
        return false;
    }
    _snwprintf(p, MAX_PATH, L"%lsbrowser_data", base);
    CreateDirectoryW(p, NULL);
    CreateDirectoryW(g_root, NULL);
    return writable_dir(g_root);
}

static bool final_path(HANDLE h, wchar_t *out, DWORD cap) {
    DWORD n = GetFinalPathNameByHandleW(h, out, cap, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    return n > 0 && n < cap;
}

/* The root the coordinator chose (hex of UTF-8), made and granted to us. */
static bool given_root(const char *hex) {
    char utf8[MAX_PATH * 3];
    size_t n = strlen(hex) / 2;
    if (n == 0 || n >= sizeof(utf8)) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(hex + 2 * i, "%2x", &v) != 1) return false;
        utf8[i] = (char)v;
    }
    utf8[n] = 0;
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, g_root, MAX_PATH) > 0 &&
           writable_dir(g_root);
}

static bool init_root(const char *root_hex) {
    if (root_hex) {
        if (!given_root(root_hex)) return false;
    } else if (!make_root(spawn_exe_dir())) {
        wchar_t base[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH - 32);
        if (n == 0 || n >= MAX_PATH - 32) return false;
        wcscat(base, L"\\cyberbrowser\\");
        CreateDirectoryW(base, NULL);       /* ...\cyberbrowser */
        if (!make_root(base)) return false;
    }
    HANDLE h = CreateFileW(g_root, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    bool ok = h != INVALID_HANDLE_VALUE && final_path(h, g_root_final, MAX_PATH * 2);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    fwprintf(stderr, L"[store] root %ls\n", g_root);
    return ok;
}

/* Is the open file inside root\site\ (no link led elsewhere), a plain file
 * with one name? */
static bool contained(HANDLE h, const char *site) {
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(h, &info)) return false;
    if (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) return false;
    if (info.nNumberOfLinks > 1) return false;
    wchar_t fp[MAX_PATH * 2], want[MAX_PATH * 2];
    if (!final_path(h, fp, MAX_PATH * 2)) return false;
    int n = _snwprintf(want, MAX_PATH * 2, L"%ls\\%hs\\", g_root_final, site);
    return n > 0 && _wcsnicmp(fp, want, (size_t)n) == 0;
}

/* Create the folders of `path` under root\site, refusing any that is a link. */
static bool make_parents(const wchar_t *path) {
    wchar_t p[MAX_PATH * 2];
    wcsncpy(p, path, MAX_PATH * 2 - 1);
    p[MAX_PATH * 2 - 1] = 0;
    size_t root_len = wcslen(g_root);
    for (wchar_t *s = p + root_len + 1; *s; s++) {
        if (*s != L'\\') continue;
        *s = 0;
        CreateDirectoryW(p, NULL);
        DWORD a = GetFileAttributesW(p);
        *s = L'\\';
        if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY) || (a & FILE_ATTRIBUTE_REPARSE_POINT))
            return false;
    }
    return true;
}

static uint64_t folder_bytes(const wchar_t *dir, int depth) {
    if (depth > 9) return 0;
    wchar_t pat[MAX_PATH * 2];
    _snwprintf(pat, MAX_PATH * 2, L"%ls\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return 0;
    uint64_t total = 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            wchar_t sub[MAX_PATH * 2];
            _snwprintf(sub, MAX_PATH * 2, L"%ls\\%ls", dir, fd.cFileName);
            total += folder_bytes(sub, depth + 1);
        } else {
            total += ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        }
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return total;
}

static Usage *usage_of(const char *site) {
    for (Usage *u = g_usage; u; u = u->next)
        if (!strcmp(u->site, site)) return u;
    Usage *u = (Usage *)calloc(1, sizeof(Usage));
    if (!u) return NULL;
    snprintf(u->site, sizeof(u->site), "%s", site);
    wchar_t dir[MAX_PATH * 2];
    _snwprintf(dir, MAX_PATH * 2, L"%ls\\%hs", g_root, site);
    u->used = folder_bytes(dir, 0);
    _snwprintf(dir, MAX_PATH * 2, L"%ls\\%hs\\cache", g_root, site);
    u->cache_used = folder_bytes(dir, 0);
    u->used -= u->cache_used > u->used ? u->used : u->cache_used;     /* (the quota is for the rest) */
    u->next = g_usage;
    g_usage = u;
    return u;
}

static bool is_cache(const char *rel) {
    return !strncmp(rel, "cache/", 6);
}

typedef struct CacheFile {
    wchar_t path[MAX_PATH * 2];
    uint64_t size;
    uint64_t time;                  /* last written or read (cache_touch) */
} CacheFile;

static void cache_files(const wchar_t *dir, int depth, CacheFile **files, size_t *n, size_t *cap) {
    if (depth > 3) return;
    wchar_t pat[MAX_PATH * 2];
    _snwprintf(pat, MAX_PATH * 2, L"%ls\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        wchar_t sub[MAX_PATH * 2];
        _snwprintf(sub, MAX_PATH * 2, L"%ls\\%ls", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            cache_files(sub, depth + 1, files, n, cap);
            continue;
        }
        if (*n == *cap) {
            size_t c2 = *cap ? *cap * 2 : 64;
            CacheFile *nf = (CacheFile *)realloc(*files, c2 * sizeof(CacheFile));
            if (!nf) break;
            *files = nf;
            *cap = c2;
        }
        CacheFile *cf = &(*files)[(*n)++];
        wcscpy(cf->path, sub);
        cf->size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        cf->time = ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

static int cache_older(const void *a, const void *b) {
    uint64_t x = ((const CacheFile *)a)->time, y = ((const CacheFile *)b)->time;
    return x < y ? -1 : x > y;
}

/* Delete site's least recently used cache files (not keep) until the cache
 * has room for need more bytes. */
static void cache_evict(Usage *u, const char *site, uint64_t need, const wchar_t *keep) {
    if (u->cache_used + need <= CACHE_BUDGET) return;
    wchar_t dir[MAX_PATH * 2];
    _snwprintf(dir, MAX_PATH * 2, L"%ls\\%hs\\cache", g_root, site);
    CacheFile *files = NULL;
    size_t n = 0, cap = 0;
    cache_files(dir, 0, &files, &n, &cap);
    qsort(files, n, sizeof(CacheFile), cache_older);
    for (size_t i = 0; i < n; i++) {
        if (u->cache_used + need <= CACHE_BUDGET) break;
        if (keep && !wcscmp(files[i].path, keep)) continue;
        if (!DeleteFileW(files[i].path)) continue;
        u->cache_used -= files[i].size > u->cache_used ? u->cache_used : files[i].size;
    }
    free(files);
}

/* A cache entry read: the most recently used now. */
static void cache_touch(const wchar_t *path) {
    HANDLE h = CreateFileW(path, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    SetFileTime(h, NULL, NULL, &now);
    CloseHandle(h);
}

/* ---- requests -------------------------------------------------------------------- */

/* Send what is left of c's reply while its ring has room: big_send's framing
 * (the first part has the type and the total size, the rest BIG_MORE), but
 * resumable, so the single-threaded service never waits on one client.  A
 * full ring costs one try of at most a timer tick (chan_send's shortest
 * wait); the next try comes after a growing pause.  True once it's all gone. */
static bool send_reply(Client *c) {
    static uint8_t part[CHAN_MSG_MAX];
    Reply *o = &c->out;
    while (o->busy) {
        uint32_t used = 0;
        if (!o->started) {
            memcpy(part, &o->len, 4);
            used = 4;
        }
        uint32_t n = o->len - o->sent;
        if (n > CHAN_MSG_MAX - used) n = CHAN_MSG_MAX - used;
        for (uint32_t k = 0; k < n;) {      /* bytes sent .. sent + n of head + data */
            uint32_t at = o->sent + k, piece;
            if (at < sizeof(o->head)) {
                piece = (uint32_t)sizeof(o->head) - at < n - k ? (uint32_t)sizeof(o->head) - at : n - k;
                memcpy(part + used + k, (const uint8_t *)&o->head + at, piece);
            } else {
                piece = n - k;
                memcpy(part + used + k, o->data + (at - sizeof(o->head)), piece);
            }
            k += piece;
        }
        DWORD now = GetTickCount();
        if (!chan_send(&c->chan, o->started ? BIG_MORE : ST_RESULT, part, used + n, 0)) {
            o->backoff = o->backoff ? (o->backoff * 2 < RETRY_MAX_MS ? o->backoff * 2 : RETRY_MAX_MS) : 1;
            o->retry_at = now + o->backoff;
            return false;
        }
        o->started = true;
        o->sent += n;
        o->progress = now;
        o->backoff = 0;
        if (o->sent == o->len) {
            free(o->data);
            memset(o, 0, sizeof(*o));
        }
    }
    return true;
}

/* Answer c: data (owned, NULL if none) goes with the reply and is freed
 * once sent. */
static void reply(Client *c, uint32_t id, bool ok, uint8_t *data, uint32_t len) {
    Reply *o = &c->out;
    memset(o, 0, sizeof(*o));
    o->head.id = id;
    o->head.ok = ok;
    o->head.data_len = ok && data ? len : 0;
    o->data = ok ? data : NULL;
    if (!ok) free(data);
    o->len = (uint32_t)sizeof(o->head) + o->head.data_len;
    o->busy = true;
    o->progress = GetTickCount();
    send_reply(c);
}

static bool do_read(const wchar_t *path, const char *site, uint8_t **data, uint32_t *len) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = false;
    LARGE_INTEGER size;
    if (contained(h, site) && GetFileSizeEx(h, &size) && size.QuadPart <= (LONGLONG)STORE_QUOTA_BYTES) {
        *data = (uint8_t *)malloc((size_t)size.QuadPart + 1);
        DWORD got = 0;
        ok = *data && ReadFile(h, *data, (DWORD)size.QuadPart, &got, NULL) && got == (DWORD)size.QuadPart;
        *len = got;
    }
    CloseHandle(h);
    return ok;
}

static uint64_t file_size(const wchar_t *path) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &a)) return 0;
    return ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
}

static bool do_write(const wchar_t *path, const char *site, const char *rel, const uint8_t *data, uint32_t len) {
    Usage *u = usage_of(site);
    if (!u) return false;               /* out of memory */
    uint64_t old = file_size(path);
    bool cache = is_cache(rel);
    if (cache) {
        /* Room in the cache's budget, the oldest entries going. */
        if (len > CACHE_ENTRY_MAX) return false;
        uint64_t was = old > u->cache_used ? u->cache_used : old;
        cache_evict(u, site, len > was ? len - was : 0, path);
        if (u->cache_used - was + len > CACHE_BUDGET) return false;
    } else if (!store_quota_ok(u->used, old > u->used ? u->used : old, len)) {
        return false;
    }
    if (!make_parents(path)) return false;
    /* Write beside it, check where it really is, then replace it.  A client
     * can't name the temporary file: '~' is outside its grammar. */
    wchar_t tmp[MAX_PATH * 2];
    _snwprintf(tmp, MAX_PATH * 2, L"%ls.~tmp", path);
    HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = contained(h, site) && WriteFile(h, data, len, &put, NULL) && put == len;
    CloseHandle(h);
    /* (A cache entry is replaced without write-through: losing one is fine.) */
    if (ok) ok = MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | (cache ? 0 : MOVEFILE_WRITE_THROUGH)) != 0;
    if (!ok) DeleteFileW(tmp);
    else {
        if (cache) u->cache_used = u->cache_used - (old > u->cache_used ? u->cache_used : old) + len;
        else u->used = u->used - (old > u->used ? u->used : old) + len;
    }
    return ok;
}

static bool do_delete(const wchar_t *path, const char *site, const char *rel) {
    HANDLE h = CreateFileW(path, DELETE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool inside = contained(h, site);
    CloseHandle(h);
    if (!inside) return false;
    uint64_t old = file_size(path);
    if (!DeleteFileW(path)) return false;
    Usage *u = usage_of(site);
    if (u && is_cache(rel)) u->cache_used -= old > u->cache_used ? u->cache_used : old;
    else if (u) u->used -= old > u->used ? u->used : old;
    return true;
}

/* A request: store_decode (proved) reads it, decides it and makes the path;
 * this opens only that path. */
static void request(Client *c, uint16_t type, const uint8_t *m, uint32_t len) {
    static StoreReq q;
    wchar_t path[MAX_PATH * 2];
    int v = store_decode(m, len, c->kind == GRANT_NET ? NULL : c->site, g_root, (uint32_t)wcslen(g_root), &q, path,
                         MAX_PATH * 2);
    if (v == STORE_REQ_SHORT) return;
    if (v != STORE_REQ_OK) {
        if (v == STORE_REQ_REFUSED) fprintf(stderr, "[store] client %u: refused %s/%s\n", c->id, q.target, q.rel);
        reply(c, q.id, false, NULL, 0);
        return;
    }
    if (type == TS_READ) {
        uint8_t *out = NULL;
        uint32_t olen = 0;
        bool ok = do_read(path, q.target, &out, &olen);
        if (ok && is_cache(q.rel)) cache_touch(path);
        reply(c, q.id, ok, out, olen);
    } else if (type == TS_WRITE) {
        reply(c, q.id, do_write(path, q.target, q.rel, m + q.data_off, q.data_len), NULL, 0);
    } else if (type == TS_DELETE) {
        reply(c, q.id, do_delete(path, q.target, q.rel), NULL, 0);
    }
}

/* ---- grants ---------------------------------------------------------------------- */

static void grant(const CsGrant *g) {
    HANDLE s = (HANDLE)(uintptr_t)g->chan_section, e0 = (HANDLE)(uintptr_t)g->chan_ev0,
           e1 = (HANDLE)(uintptr_t)g->chan_ev1;
    int slot = -1;
    for (int i = 0; i < MAX_CLIENTS && slot < 0; i++)
        if (!g_clients[i]) slot = i;
    Client *c = (Client *)calloc(1, sizeof(Client));
    bool site_ok = g->kind == GRANT_NET || (g->kind == GRANT_TAB && !g->site[SITE_MAX - 1] &&
                                            (!g->site[0] || site_label_ok(g->site)));
    if (slot < 0 || !c || !site_ok || !chan_attach(&c->chan, s, e0, e1, 1)) {
        free(c);
        CloseHandle(s); CloseHandle(e0); CloseHandle(e1);
        return;
    }
    c->id = g->client;
    c->kind = g->kind;
    c->rx.limit = (uint32_t)(sizeof(StoreRequest) + SITE_MAX + STORE_PATH_MAX + STORE_QUOTA_BYTES);  /* request()'s largest; data over the quota fails */
    if (g->kind == GRANT_TAB) memcpy(c->site, g->site, SITE_MAX);
    g_clients[slot] = c;
    fprintf(stderr, "[store] serving %s %u (%s)\n", c->kind == GRANT_NET ? "the network process" : "tab",
            c->id, c->site[0] ? c->site : "-");
}

static void drop(int i) {
    Client *c = g_clients[i];
    if (!c) return;
    chan_close(&c->chan);
    big_free(&c->rx);
    free(c->out.data);
    free(c);
    g_clients[i] = NULL;
}

/* The coordinator's revocation names a tab.  (A client that breaks its
 * channel, a tab or the network process, is dropped by its slot.) */
static void revoke(uint32_t id) {
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (g_clients[i] && g_clients[i]->kind == GRANT_TAB && g_clients[i]->id == id) drop(i);
}

int main(int argc, char **argv) {
    if (argc < 5 || strcmp(argv[1], "--chan") ||
        !chan_attach(&g_ctl, spawn_handle_from_arg(argv[2]), spawn_handle_from_arg(argv[3]),
                     spawn_handle_from_arg(argv[4]), 1)) {
        fprintf(stderr, "cb_store: bad arguments\n");
        return 1;
    }
    const char *root_hex = argc >= 7 && !strcmp(argv[5], "--root") ? argv[6] : NULL;
    if (!init_root(root_hex)) { fprintf(stderr, "cb_store: no writable browser_data\n"); return 1; }
    static ChanMsg m;
    for (;;) {
        /* Everyone's messages, then sleep until one more arrives. */
        int r;
        while ((r = chan_recv(&g_ctl, &m)) > 0) {
            if (m.type == CS_GRANT && m.len == sizeof(CsGrant)) {
                CsGrant g;
                memcpy(&g, m.data, sizeof(g));
                grant(&g);
            } else if (m.type == CS_REVOKE && m.len == sizeof(CsRevoke)) {
                CsRevoke v;
                memcpy(&v, m.data, sizeof(v));
                revoke(v.client);
            } else if (m.type == CS_QUIT) {
                return 0;
            }
        }
        if (r < 0) return 1;
        /* Each client's requests, one at a time: the next is read only once
         * the last one's reply is sent.  A client that leaves a reply unread
         * holds up only itself, and is dropped after REPLY_STALL_MS. */
        DWORD wait_ms = INFINITE;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            Client *c = g_clients[i];
            if (!c) continue;
            DWORD now = GetTickCount();
            if (c->out.busy && (int32_t)(now - c->out.retry_at) >= 0 && !send_reply(c) &&
                now - c->out.progress > REPLY_STALL_MS) {
                fprintf(stderr, "[store] client %u left its reply unread for %u ms: dropped\n", c->id,
                        (unsigned)(now - c->out.progress));
                drop(i);
                continue;
            }
            int got = 0;
            while (!c->out.busy && (got = big_recv(&c->chan, &c->rx)) > 0) request(c, c->rx.type, c->rx.buf, c->rx.total);
            if (got < 0) {
                fprintf(stderr, "[store] client %u broke its channel\n", c->id);
                drop(i);
                continue;
            }
            if (c->out.busy) {
                int32_t left = (int32_t)(c->out.retry_at - GetTickCount());
                DWORD ms = left < 1 ? 1 : (DWORD)left;
                if (ms < wait_ms) wait_ms = ms;
            }
        }
        fflush(stderr);
        HANDLE waits[MAX_CLIENTS + 1];
        int n = 0;
        bool ready = !chan_arm(&g_ctl);
        waits[n++] = chan_event(&g_ctl);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!g_clients[i]) continue;
            if (!chan_arm(&g_clients[i]->chan) && !g_clients[i]->out.busy) ready = true;
            waits[n++] = chan_event(&g_clients[i]->chan);
        }
        if (!ready) WaitForMultipleObjects((DWORD)n, waits, FALSE, wait_ms);
        chan_disarm(&g_ctl);
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (g_clients[i]) chan_disarm(&g_clients[i]->chan);
    }
}

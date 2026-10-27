/* store_main.c - cb_store.exe: the storage service (SANDBOX_DESIGN.md
 * sections 2.5 and 3.3), the Win32 shim around store_core.c.
 *
 * Started by the coordinator with its control channel:
 *   cb_store.exe --chan S E0 E1
 *
 * Owns browser_data/sites/ next to the executables (or, if that can't be
 * written, %LOCALAPPDATA%\cyberbrowser\browser_data\sites).  The coordinator
 * grants it a channel per tab, badged with the tab's site, and one to the
 * network process.  A client names files by paths relative to its site's
 * folder; store_core.c decides what is allowed, and every file is opened
 * refusing reparse points and hard links and checked to really be inside that
 * folder.  Writes replace a file whole, within the site's quota.
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

typedef struct Client {
    uint32_t id;
    uint32_t kind;                  /* GRANT_TAB or GRANT_NET */
    char site[SITE_MAX];            /* a tab's badge; "" = opaque: nothing allowed */
    Chan chan;                      /* side 1: the storage service */
    BigRx rx;
} Client;

/* Bytes each site uses, counted from its folder when first needed. */
typedef struct Usage {
    char site[SITE_MAX];
    uint64_t used;
    struct Usage *next;
} Usage;

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
    _snwprintf(p, MAX_PATH, L"%lsbrowser_data", base);
    CreateDirectoryW(p, NULL);
    _snwprintf(g_root, MAX_PATH, L"%lsbrowser_data\\sites", base);
    CreateDirectoryW(g_root, NULL);
    return writable_dir(g_root);
}

static bool final_path(HANDLE h, wchar_t *out, DWORD cap) {
    DWORD n = GetFinalPathNameByHandleW(h, out, cap, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    return n > 0 && n < cap;
}

static bool init_root(void) {
    if (!make_root(spawn_exe_dir())) {
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

/* "site/a/b" (checked by store_join) as root\site\a\b. */
static void to_path(const char *joined, wchar_t *out, size_t cap) {
    size_t o = (size_t)_snwprintf(out, cap, L"%ls\\", g_root);
    for (const char *p = joined; *p && o + 1 < cap; p++) out[o++] = *p == '/' ? L'\\' : (wchar_t)*p;
    out[o] = 0;
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
    snprintf(u->site, sizeof(u->site), "%s", site);
    wchar_t dir[MAX_PATH * 2];
    _snwprintf(dir, MAX_PATH * 2, L"%ls\\%hs", g_root, site);
    u->used = folder_bytes(dir, 0);
    u->next = g_usage;
    g_usage = u;
    return u;
}

/* ---- requests -------------------------------------------------------------------- */

static void reply(Client *c, uint32_t id, bool ok, const void *data, uint32_t len) {
    StoreResult r = { id, ok, ok ? len : 0 };
    big_send(&c->chan, ST_RESULT, &r, sizeof(r), ok ? data : NULL, ok ? len : 0, NULL, 0, 10000);
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

static bool do_write(const wchar_t *path, const char *site, const uint8_t *data, uint32_t len) {
    Usage *u = usage_of(site);
    uint64_t old = file_size(path);
    if (!store_quota_ok(u->used, old > u->used ? u->used : old, len)) return false;
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
    if (ok) ok = MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) DeleteFileW(tmp);
    else u->used = u->used - (old > u->used ? u->used : old) + len;
    return ok;
}

static bool do_delete(const wchar_t *path, const char *site) {
    HANDLE h = CreateFileW(path, DELETE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool inside = contained(h, site);
    CloseHandle(h);
    if (!inside) return false;
    uint64_t old = file_size(path);
    if (!DeleteFileW(path)) return false;
    Usage *u = usage_of(site);
    u->used -= old > u->used ? u->used : old;
    return true;
}

static void request(Client *c, uint16_t type, const uint8_t *m, uint32_t len) {
    StoreRequest q;
    if (len < sizeof(q)) return;
    memcpy(&q, m, sizeof(q));
    if ((uint64_t)sizeof(q) + q.site_len + q.path_len + q.data_len != len || q.site_len >= SITE_MAX ||
        q.path_len > STORE_PATH_MAX) {
        reply(c, q.id, false, NULL, 0);
        return;
    }
    char target[SITE_MAX], rel[STORE_PATH_MAX + 1];
    const uint8_t *p = m + sizeof(q);
    if (q.site_len) {
        memcpy(target, p, q.site_len);
        target[q.site_len] = 0;
    } else {
        snprintf(target, sizeof(target), "%s", c->site);
    }
    memcpy(rel, p + q.site_len, q.path_len);
    rel[q.path_len] = 0;
    const uint8_t *data = p + q.site_len + q.path_len;
    const char *client_site = c->kind == GRANT_NET ? NULL : c->site;
    char joined[SITE_MAX + STORE_PATH_MAX + 2];
    if ((client_site && !client_site[0]) || !store_allowed(client_site, target, rel, q.path_len) ||
        !store_join(target, rel, q.path_len, joined, sizeof(joined))) {
        fprintf(stderr, "[store] client %u: refused %s/%s\n", c->id, target, rel);
        reply(c, q.id, false, NULL, 0);
        return;
    }
    wchar_t path[MAX_PATH * 2];
    to_path(joined, path, MAX_PATH * 2);
    if (type == TS_READ) {
        uint8_t *out = NULL;
        uint32_t olen = 0;
        bool ok = do_read(path, target, &out, &olen);
        reply(c, q.id, ok, out, olen);
        free(out);
    } else if (type == TS_WRITE) {
        reply(c, q.id, do_write(path, target, data, q.data_len), NULL, 0);
    } else if (type == TS_DELETE) {
        reply(c, q.id, do_delete(path, target), NULL, 0);
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
    if (g->kind == GRANT_TAB) memcpy(c->site, g->site, SITE_MAX);
    g_clients[slot] = c;
    fprintf(stderr, "[store] serving %s %u (%s)\n", c->kind == GRANT_NET ? "the network process" : "tab",
            c->id, c->site[0] ? c->site : "-");
}

static void revoke(uint32_t id) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        Client *c = g_clients[i];
        if (!c || c->kind != GRANT_TAB || c->id != id) continue;
        chan_close(&c->chan);
        big_free(&c->rx);
        free(c);
        g_clients[i] = NULL;
    }
}

int main(int argc, char **argv) {
    if (argc < 5 || strcmp(argv[1], "--chan") ||
        !chan_attach(&g_ctl, spawn_handle_from_arg(argv[2]), spawn_handle_from_arg(argv[3]),
                     spawn_handle_from_arg(argv[4]), 1)) {
        fprintf(stderr, "cb_store: bad arguments\n");
        return 1;
    }
    if (!init_root()) { fprintf(stderr, "cb_store: no writable browser_data\n"); return 1; }
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
        for (int i = 0; i < MAX_CLIENTS; i++) {
            Client *c = g_clients[i];
            int got;
            while (c && (got = big_recv(&c->chan, &c->rx)) > 0) request(c, c->rx.type, c->rx.buf, c->rx.total);
            if (c && got < 0) {
                fprintf(stderr, "[store] client %u broke its channel\n", c->id);
                revoke(c->id);
            }
        }
        fflush(stderr);
        HANDLE waits[MAX_CLIENTS + 1];
        int n = 0;
        bool ready = !chan_arm(&g_ctl);
        waits[n++] = chan_event(&g_ctl);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!g_clients[i]) continue;
            if (!chan_arm(&g_clients[i]->chan)) ready = true;
            waits[n++] = chan_event(&g_clients[i]->chan);
        }
        if (!ready) WaitForMultipleObjects((DWORD)n, waits, FALSE, INFINITE);
        chan_disarm(&g_ctl);
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (g_clients[i]) chan_disarm(&g_clients[i]->chan);
    }
}

/* escape_main.c - cb_escape.exe: a hostile tab for the escape tests
 * (SANDBOX_DESIGN.md section 11.1).
 *
 * The coordinator starts it in place of cb_tab.exe, with a tab's arguments and
 * sandbox, for the first tab opened on the URL in CYBER_TAB_EXE_URL when
 * CYBER_TAB_EXE=cb_escape.exe: both set in the coordinator's environment
 * (tests/sandbox/escape_test.sh).  It plays a tab whose renderer an attacker
 * controls, and tries to get out: to files, the registry, sockets, other
 * processes and threads, new processes, windows and the GPU, executable
 * memory, more memory than its job allows, other sites' storage and cookies,
 * other sites' pages as its own document (P10), its site's web-GPU process
 * (records that lie, objects it never made); it leaves the network
 * process's replies unread;
 * and it sends malformed messages to every server and scribbles on the
 * channels' shared memory.  Each attempt prints a line:
 *
 *   [escape] blocked <what>          the sandbox held
 *   [escape] ESCAPED <what>: <how>   it didn't
 *   [escape] ok <what>               a check that must work did
 *   [escape] FAILED <what>           it didn't (the test is broken)
 *   [escape] note <what>             allowed, and harmless if the test
 *                                    script's checks afterwards pass
 *
 * and the last line is "[escape] done: N attempts, E escaped, F failed".
 * The exit code is E + F.  Whether the servers survived is checked by the
 * test script, which opens a normal tab afterwards.
 *
 * Its environment (set by the test script):
 *   CYBER_ESCAPE_READ   files it must not open, separated by ';'
 *   CYBER_ESCAPE_WRITE  folders it must not create a file in, by ';'
 *   CYBER_ESCAPE_PORT   the test server's port on 127.0.0.1
 *   CYBER_ESCAPE_OTHER  another site's label: its folder holds escape_canary,
 *                       and this tab's folder holds jlink (a junction to
 *                       it) and hlink (a hard link to its canary) */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <tlhelp32.h>
#include <sddl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chan.h"
#include "bigmsg.h"
#include "http_download.h"     /* HTTP_MODE_* */
#include "proto.h"
#include "spawn.h"
#include "site.h"
#include "store_core.h"       /* STORE_QUOTA_BYTES */
#include "gpu_wire.h"

static int g_attempts, g_escaped, g_failed;
static Chan g_ui, g_net, g_store, g_gpu, g_ctl, g_wgpu;
static bool g_have_store, g_have_gpu, g_have_ctl, g_have_wgpu;
static uint32_t g_tab;
static char g_url[PROTO_URL_MAX + 1];
static char g_site[SITE_MAX];

static void blocked_or(const char *what, bool escaped, const char *how) {
    g_attempts++;
    if (escaped) {
        g_escaped++;
        printf("[escape] ESCAPED %s: %s\n", what, how ? how : "");
    } else {
        printf("[escape] blocked %s\n", what);
    }
    fflush(stdout);
}

static void note(const char *what, const char *detail) {
    printf("[escape] note %s: %s\n", what, detail);
    fflush(stdout);
}

/* Its AppContainer SID as a string (LocalFree it), or NULL. */
static char *own_sid(void) {
    HANDLE tok;
    char *sid = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        union { TOKEN_APPCONTAINER_INFORMATION i; char b[256]; } ac;
        DWORD n;
        if (GetTokenInformation(tok, TokenAppContainerSid, &ac, sizeof(ac), &n) && ac.i.TokenAppContainer)
            ConvertSidToStringSidA(ac.i.TokenAppContainer, &sid);
        CloseHandle(tok);
    }
    return sid;
}

static void must(const char *what, bool ok) {
    if (!ok) g_failed++;
    printf("[escape] %s %s\n", ok ? "ok" : "FAILED", what);
    fflush(stdout);
}

/* ---- files and the registry ------------------------------------------------- */

static void try_read(const char *path) {
    char what[MAX_PATH + 16];
    snprintf(what, sizeof(what), "read %s", path);
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    DWORD err = GetLastError();
    /* A sharing violation means the access check passed. */
    blocked_or(what, h != INVALID_HANDLE_VALUE || err == ERROR_SHARING_VIOLATION, "opened");
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

static void try_list(const char *dir) {
    char what[MAX_PATH + 16], pattern[MAX_PATH + 4];
    snprintf(what, sizeof(what), "list %s", dir);
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    blocked_or(what, h != INVALID_HANDLE_VALUE, fd.cFileName);
    if (h != INVALID_HANDLE_VALUE) FindClose(h);
}

static void try_create_in(const char *dir) {
    char what[MAX_PATH + 16], path[MAX_PATH + 32];
    snprintf(what, sizeof(what), "create a file in %s", dir);
    snprintf(path, sizeof(path), "%s\\cb_escape_%lu.txt", dir, GetCurrentProcessId());
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_FLAG_DELETE_ON_CLOSE, NULL);
    blocked_or(what, h != INVALID_HANDLE_VALUE, path);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

static void for_each(const char *list, void (*fn)(const char *)) {
    if (!list) return;
    char buf[8192];
    snprintf(buf, sizeof(buf), "%s", list);
    for (char *s = strtok(buf, ";"); s; s = strtok(NULL, ";"))
        if (*s) fn(s);
}

static void files(void) {
    for_each(getenv("CYBER_ESCAPE_READ"), try_read);
    for_each(getenv("CYBER_ESCAPE_WRITE"), try_create_in);
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    try_read(exe);                                       /* its own folder */
    const char *profile = getenv("USERPROFILE");
    if (profile) {
        try_list(profile);
        char p[MAX_PATH];
        snprintf(p, sizeof(p), "%s\\Documents", profile);
        try_list(p);
        snprintf(p, sizeof(p), "%s\\AppData\\Local\\Microsoft\\Windows\\Fonts", profile);
        try_list(p);
    }
    try_create_in("C:\\Users\\Public");
    try_create_in("C:\\ProgramData");
    /* Its AppContainer profile's folder: its own, made for this process and
     * deleted after it (the test script checks), so whatever it leaves there
     * reaches no later tab. */
    char *sid = own_sid();
    wchar_t *folder = NULL;
    /* userenv loaded here: linking it (or ole32) would load user32, which a
     * process without win32k can't initialize. */
    typedef HRESULT (WINAPI *FolderPath)(PCWSTR, PWSTR *);
    HMODULE userenv = LoadLibraryA("userenv.dll");
    FolderPath folder_path = userenv ? (FolderPath)(void *)GetProcAddress(userenv, "GetAppContainerFolderPath") : NULL;
    if (sid && folder_path) {
        wchar_t wsid[128];
        MultiByteToWideChar(CP_ACP, 0, sid, -1, wsid, 128);
        if (SUCCEEDED(folder_path(wsid, &folder))) {
            char path[MAX_PATH + 64];
            snprintf(path, sizeof(path), "%ls\\Temp\\cb_escape_left_behind.txt", folder);
            HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                note("left a file in its profile's folder", path);
                CloseHandle(h);
            }
        }
    }
    if (sid) LocalFree(sid);
}

static void registry(void) {
    HKEY k;
    LONG r = RegOpenKeyExA(HKEY_CURRENT_USER, "Software", 0, KEY_READ, &k);
    blocked_or("read HKCU\\Software", r == ERROR_SUCCESS, "opened");
    if (r == ERROR_SUCCESS) RegCloseKey(k);
    r = RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SAM", 0, KEY_READ, &k);
    blocked_or("read HKLM\\SAM", r == ERROR_SUCCESS, "opened");
    if (r == ERROR_SUCCESS) RegCloseKey(k);
    r = RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\cb_escape", 0, NULL, REG_OPTION_VOLATILE, KEY_WRITE, NULL, &k,
                        NULL);
    blocked_or("create HKCU\\Software\\cb_escape", r == ERROR_SUCCESS, "created");
    if (r == ERROR_SUCCESS) {
        RegCloseKey(k);
        RegDeleteKeyA(HKEY_CURRENT_USER, "Software\\cb_escape");
    }
    /* Its profile's registry storage: deleted with the profile. */
    char *sid = own_sid();
    if (sid) {
        char path[512];
        snprintf(path, sizeof(path),
                 "Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppContainer\\"
                 "Storage\\%s\\cb_escape",
                 sid);
        r = RegCreateKeyExA(HKEY_CURRENT_USER, path, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL);
        if (r == ERROR_SUCCESS) {
            note("left a key in its profile's registry storage", path);
            RegCloseKey(k);
        }
        LocalFree(sid);
    }
}

/* ---- sockets, processes, threads, the token ------------------------------------ */

static void sockets(void) {
    WSADATA w;
    int r = WSAStartup(MAKEWORD(2, 2), &w);
    if (r != 0) {
        blocked_or("start Winsock", false, NULL);
        return;
    }
    const char *port = getenv("CYBER_ESCAPE_PORT");
    struct { const char *host, *port; } to[] = { { "127.0.0.1", port ? port : "80" }, { "1.1.1.1", "53" } };
    for (int i = 0; i < 2; i++) {
        char what[96];
        snprintf(what, sizeof(what), "connect to %s:%s", to[i].host, to[i].port);
        SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons((u_short)atoi(to[i].port));
        inet_pton(AF_INET, to[i].host, &a.sin_addr);
        bool ok = s != INVALID_SOCKET && connect(s, (struct sockaddr *)&a, sizeof(a)) == 0;
        blocked_or(what, ok, "connected");
        if (s != INVALID_SOCKET) closesocket(s);
    }
}

static void processes(void) {
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    char cmd[] = "cmd.exe /c exit 0";
    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    blocked_or("start cmd.exe", ok, "started");
    if (ok) {
        TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    ok = CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB, NULL, NULL, &si,
                        &pi);
    blocked_or("start cmd.exe outside its job", ok, "started");
    if (ok) {
        TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }

    /* Every other process, with each dangerous right: the coordinator, the
     * servers, the UI, other tabs, everything else. */
    static const struct { DWORD access; const char *name; } rights[] = {
        { PROCESS_VM_READ, "read memory" },
        { PROCESS_VM_WRITE | PROCESS_VM_OPERATION, "write memory" },
        { PROCESS_CREATE_THREAD, "create threads" },
        { PROCESS_DUP_HANDLE, "take handles" },
        { PROCESS_TERMINATE, "terminate" },
        { PROCESS_SUSPEND_RESUME, "suspend" },
    };
    DWORD self = GetCurrentProcessId();
    int opened = 0;
    char how[256] = "";
    for (DWORD pid = 4; pid < 400000; pid += 4) {
        if (pid == self) continue;
        for (size_t k = 0; k < sizeof(rights) / sizeof(rights[0]); k++) {
            HANDLE p = OpenProcess(rights[k].access, FALSE, pid);
            if (p) {
                if (!opened++) {
                    char exe[MAX_PATH] = "?";
                    DWORD n = MAX_PATH;
                    HANDLE q = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                    if (q) {
                        QueryFullProcessImageNameA(q, 0, exe, &n);
                        CloseHandle(q);
                    }
                    snprintf(how, sizeof(how), "pid %lu (%s): %s", pid, exe, rights[k].name);
                }
                CloseHandle(p);
            }
        }
    }
    blocked_or("open another process (every pid, every dangerous right)", opened > 0, how);
    /* Debug the coordinator (its parent): if this got through, nothing but
     * the browser would suffer. */
    typedef LONG (WINAPI *QueryInfo)(HANDLE, int, void *, ULONG, ULONG *);
    QueryInfo query = (QueryInfo)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationProcess");
    struct { void *r1; void *peb; void *r2[2]; ULONG_PTR pid, parent; } basic;
    if (query && query(GetCurrentProcess(), 0 /* ProcessBasicInformation */, &basic, sizeof(basic), NULL) == 0) {
        BOOL dbg = DebugActiveProcess((DWORD)basic.parent);
        if (dbg) DebugActiveProcessStop((DWORD)basic.parent);
        blocked_or("debug the coordinator", dbg, "attached");
    }

    /* Threads of other processes, if it can list them. */
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        blocked_or("list the threads of other processes", false, NULL);
    } else {
        THREADENTRY32 te;
        te.dwSize = sizeof(te);
        int seen = 0, got = 0;
        for (BOOL ok2 = Thread32First(snap, &te); ok2; ok2 = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID == self) continue;
            seen++;
            HANDLE t = OpenThread(THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
            if (t) {
                if (!got++) snprintf(how, sizeof(how), "thread %lu of pid %lu", te.th32ThreadID, te.th32OwnerProcessID);
                CloseHandle(t);
            }
        }
        CloseHandle(snap);
        char what[96];
        snprintf(what, sizeof(what), "take over a thread of another process (%d listed)", seen);
        blocked_or(what, got > 0, how);
    }

    /* Privileges it doesn't have. */
    HANDLE tok;
    bool raised = false;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
        static const char *const privs[] = { SE_DEBUG_NAME, SE_TCB_NAME, SE_IMPERSONATE_NAME, SE_BACKUP_NAME };
        for (int i = 0; i < 4; i++) {
            TOKEN_PRIVILEGES tp;
            tp.PrivilegeCount = 1;
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            if (LookupPrivilegeValueA(NULL, privs[i], &tp.Privileges[0].Luid) &&
                AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), NULL, NULL) && GetLastError() == ERROR_SUCCESS) {
                raised = true;
                snprintf(how, sizeof(how), "%s", privs[i]);
            }
        }
        CloseHandle(tok);
    }
    blocked_or("enable a privilege (debug, tcb, impersonate, backup)", raised, how);
}

/* ---- memory ---------------------------------------------------------------------- */

static void memory(void) {
    void *p = VirtualAlloc(NULL, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    blocked_or("allocate executable memory", p != NULL, "allocated");
    if (p) VirtualFree(p, 0, MEM_RELEASE);
    p = VirtualAlloc(NULL, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    DWORD old;
    bool ok = p && VirtualProtect(p, 65536, PAGE_EXECUTE_READ, &old);
    blocked_or("make written memory executable", ok, "protected");
    if (p) VirtualFree(p, 0, MEM_RELEASE);
    HANDLE s = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_EXECUTE_READWRITE, 0, 65536, NULL);
    void *v = s ? MapViewOfFile(s, FILE_MAP_WRITE | FILE_MAP_EXECUTE, 0, 0, 65536) : NULL;
    blocked_or("map a section writable and executable", v != NULL, "mapped");
    if (v) UnmapViewOfFile(v);
    if (s) CloseHandle(s);

    /* More than the job allows (4 GB). */
    enum { CHUNK = 256 << 20, MAXN = 24 };
    void *chunks[MAXN];
    int n = 0;
    while (n < MAXN && (chunks[n] = VirtualAlloc(NULL, CHUNK, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) != NULL) n++;
    char how[64];
    snprintf(how, sizeof(how), "committed %d MB", n * 256);
    blocked_or("commit more than 4.5 GB", n * 256 > 4608, how);
    for (int i = 0; i < n; i++) VirtualFree(chunks[i], 0, MEM_RELEASE);
}

/* ---- windows and the GPU (win32k) ---------------------------------------------- */

static void windows(void) {
    printf("[escape] trying user32 (win32k is disabled: this may end the process)\n");
    fflush(stdout);
    HMODULE user32 = LoadLibraryA("user32.dll");
    if (!user32) {
        blocked_or("load user32.dll", false, NULL);
    } else {
        typedef HWND (WINAPI *CreateWindowExA_t)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU,
                                                HINSTANCE, LPVOID);
        typedef BOOL (WINAPI *OpenClipboard_t)(HWND);
        CreateWindowExA_t cw = (CreateWindowExA_t)(void *)GetProcAddress(user32, "CreateWindowExA");
        OpenClipboard_t oc = (OpenClipboard_t)(void *)GetProcAddress(user32, "OpenClipboard");
        HWND w = cw ? cw(0, "STATIC", "escape", 0, 0, 0, 10, 10, NULL, NULL, NULL, NULL) : NULL;
        blocked_or("create a window", w != NULL, "created");
        blocked_or("open the clipboard", oc && oc(NULL), "opened");
    }
    HMODULE d3d = LoadLibraryA("d3d11.dll");
    typedef HRESULT (WINAPI *Create_t)(void *, int, HMODULE, UINT, const void *, UINT, UINT, void **, void *, void **);
    Create_t create = d3d ? (Create_t)(void *)GetProcAddress(d3d, "D3D11CreateDevice") : NULL;
    void *dev = NULL;
    HRESULT hr = create ? create(NULL, 1 /* D3D_DRIVER_TYPE_HARDWARE */, NULL, 0, NULL, 0, 7 /* SDK */, &dev, NULL,
                                 NULL)
                        : E_FAIL;
    blocked_or("create a GPU device", SUCCEEDED(hr) && dev, "created");
}

/* ---- the servers ------------------------------------------------------------------ */

/* Send and wait for the reply with this id (the first 4 bytes of the reply). */
static uint8_t *call(Chan *c, uint16_t type, const void *a, uint32_t alen, const void *b, uint32_t blen,
                     const void *d, uint32_t dlen, uint32_t id, uint32_t *len) {
    if (!big_send(c, type, a, alen, b, blen, d, dlen, 2000)) return NULL;
    static BigRx rx;
    for (DWORD start = GetTickCount(); GetTickCount() - start < 10000;) {
        int r = big_recv(c, &rx);
        if (r < 0) return NULL;
        if (r == 0) {
            chan_wait(c, 100);
            continue;
        }
        uint32_t got;
        if (c == &g_net && rx.type == NT_PART) continue;     /* (a document's body as it came) */
        if (rx.total >= 4) {
            memcpy(&got, rx.buf, 4);
            if (got == id) {
                *len = rx.total;
                return rx.buf;
            }
        }
    }
    return NULL;
}

static uint32_t g_id = 1000;

/* A storage request: did the service say yes (and what did it return)? */
static bool store(uint16_t type, const char *site, const char *path, const void *data, uint32_t dlen, char *out,
                  uint32_t cap) {
    StoreRequest q = { ++g_id, site ? (uint32_t)strlen(site) : 0, (uint32_t)strlen(path), dlen };
    uint32_t sl = q.site_len, pl = q.path_len;
    char sp[SITE_MAX + 1024];
    if (sl + pl > sizeof(sp)) return false;
    memcpy(sp, site ? site : "", sl);
    memcpy(sp + sl, path, pl);
    uint32_t len;
    uint8_t *m = call(&g_store, type, &q, sizeof(q), sp, sl + pl, data, dlen, q.id, &len);
    StoreResult r;
    if (!m || len < sizeof(r)) return false;
    memcpy(&r, m, sizeof(r));
    if (out && cap) {
        uint32_t n = r.data_len < cap - 1 && sizeof(r) + r.data_len <= len ? r.data_len : 0;
        memcpy(out, m + sizeof(r), n);
        out[n] = 0;
    }
    return r.ok != 0;
}

static void storage(void) {
    if (!g_have_store) {
        must("a storage channel (the escape URL's site isn't opaque)", false);
        return;
    }
    char got[256];
    must("write and read its own file", store(TS_WRITE, NULL, "escape/own", "mine", 4, NULL, 0) &&
                                            store(TS_READ, NULL, "escape/own", NULL, 0, got, sizeof(got)) &&
                                            !strcmp(got, "mine"));
    const char *other = getenv("CYBER_ESCAPE_OTHER");
    if (!other) other = "http+127.0.0.1";
    /* Another site, named outright or through paths. */
    blocked_or("read another site's file (naming the site)", store(TS_READ, other, "escape_canary", NULL, 0, got,
                                                                   sizeof(got)), got);
    blocked_or("write another site's file (naming the site)", store(TS_WRITE, other, "escape_planted", "x", 1, NULL, 0),
               "written");
    blocked_or("read another site's cookies", store(TS_READ, other, "cookies", NULL, 0, got, sizeof(got)), got);
    static const char *const paths[] = {
        "../%s/escape_canary", "..\\%s\\escape_canary", "a/../../%s/escape_canary", "./../%s/escape_canary",
        "local_storage/../../%s/escape_canary", "%%2e%%2e/%s/escape_canary", "..%%2f%s%%2fescape_canary",
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        char p[256], what[320];
        snprintf(p, sizeof(p), paths[i], other);
        snprintf(what, sizeof(what), "read \"%s\"", p);
        blocked_or(what, store(TS_READ, NULL, p, NULL, 0, got, sizeof(got)), got);
    }
    static const char *const odd[] = {
        "C:/Windows/win.ini", "C:\\Windows\\win.ini", "\\\\?\\C:\\Windows\\win.ini", "\\\\localhost\\C$\\Windows\\win.ini",
        "/Windows/win.ini", "cookies:secret", "escape/own::$DATA", "escape/own:stream", "CON", "nul", "aux.txt",
        "COM1", "escape/own.", "escape/own ", "HTTP+1~1/escape_canary", "jlink/escape_canary", "hlink", "jlink",
        ".", "..", "", "/", "a/b/c/d/e/f/g/h/i",
    };
    for (size_t i = 0; i < sizeof(odd) / sizeof(odd[0]); i++) {
        char what[320];
        snprintf(what, sizeof(what), "read \"%s\"", odd[i]);
        blocked_or(what, store(TS_READ, NULL, odd[i], NULL, 0, got, sizeof(got)), got);
    }
    blocked_or("write through a junction", store(TS_WRITE, NULL, "jlink/escape_planted", "x", 1, NULL, 0), "written");
    blocked_or("delete through a hard link", store(TS_DELETE, NULL, "hlink", NULL, 0, NULL, 0), "deleted");
    blocked_or("write an alternate data stream", store(TS_WRITE, NULL, "escape/own:ads", "x", 1, NULL, 0), "written");
    /* Its cookies file is the network process's alone. */
    blocked_or("read its own site's cookies file", store(TS_READ, NULL, "cookies", NULL, 0, got, sizeof(got)), got);
    blocked_or("write its own site's cookies file", store(TS_WRITE, NULL, "cookies", "x", 1, NULL, 0), "written");
    /* Its site's HTTP cache is the network process's to fill: an entry
     * planted here would be served as the site's response. */
    blocked_or("plant an entry in its own site's HTTP cache",
               store(TS_WRITE, NULL, "cache/http/0123456789abcdef0123456789abcdef", "x", 1, NULL, 0), "written");
    /* Over its quota (64 MB), by a byte: still within the channel's limit, so
     * the storage service itself refuses it.  (More than that is past the
     * limit, and the service drops the channel.) */
    uint32_t big = (uint32_t)STORE_QUOTA_BYTES + 1;
    char *blob = (char *)calloc(1, big);
    if (blob) {
        blocked_or("write 64 MB and a byte (the quota is 64 MB)",
                   store(TS_WRITE, NULL, "escape/big", blob, big, NULL, 0), "written");
        free(blob);
    }
    store(TS_DELETE, NULL, "escape/own", NULL, 0, NULL, 0);
}

/* A request through the network process, saying it is for mode (an
 * HTTP_MODE_*) from initiator; the body into out, the reply's NET_* into
 * *ok and its URL (where it ended, or where to go) into where.  False if
 * there was no reply. */
static bool fetch_reply(const char *url, const char *method, uint32_t mode, const char *initiator, char *out,
                        uint32_t cap, uint32_t *ok, char *where, uint32_t where_cap) {
    NetRequest q;
    memset(&q, 0, sizeof(q));
    q.id = ++g_id;
    q.method_len = (uint32_t)strlen(method) + 1;
    q.url_len = (uint32_t)strlen(url) + 1;
    q.headers_len = 0;
    q.mode = mode;
    q.initiator_len = (uint32_t)strlen(initiator) + 1;
    char buf[2 * PROTO_URL_MAX + 32];
    memcpy(buf, method, q.method_len);
    memcpy(buf + q.method_len, url, q.url_len);
    memcpy(buf + q.method_len + q.url_len, initiator, q.initiator_len);
    uint32_t len;
    uint8_t *m = call(&g_net, TN_REQUEST, &q, sizeof(q), buf, q.method_len + q.url_len + q.initiator_len, NULL, 0,
                      q.id, &len);
    NetResponse r;
    *ok = NET_FAILED;
    if (out && cap) out[0] = 0;
    if (where && where_cap) where[0] = 0;
    if (!m || len < sizeof(r)) return false;
    memcpy(&r, m, sizeof(r));
    uint64_t body_at = sizeof(r) + (uint64_t)r.url_len + r.headers_len + r.error_len;
    if (out && cap) {
        uint32_t n = body_at + r.body_len <= len && r.body_len < cap ? r.body_len : 0;
        memcpy(out, m + body_at, n);
        out[n] = 0;
    }
    if (where && where_cap) {
        uint32_t n = sizeof(r) + (uint64_t)r.url_len <= len && r.url_len < where_cap ? r.url_len : 0;
        memcpy(where, m + sizeof(r), n);
        where[n] = 0;
    }
    *ok = r.ok;
    return true;
}

static bool fetch_as(const char *url, const char *method, uint32_t mode, const char *initiator, char *out,
                     uint32_t cap) {
    uint32_t ok;
    return fetch_reply(url, method, mode, initiator, out, cap, &ok, NULL, 0) && ok == NET_OK;
}

static bool fetch(const char *url, const char *method, char *out, uint32_t cap) {
    return fetch_as(url, method, HTTP_MODE_NO_CORS, "", out, cap);
}

static void network(void) {
    const char *port = getenv("CYBER_ESCAPE_PORT");
    if (!port) port = "38517";
    char url[PROTO_URL_MAX + 1], got[4096];
    /* document.cookie of its own site: never an HttpOnly cookie. */
    NetCookieGet g = { ++g_id };
    snprintf(url, sizeof(url), "%s", g_url);
    char *hash = strchr(url, '#');
    if (hash) *hash = 0;
    uint32_t len;
    uint8_t *m = call(&g_net, TN_COOKIE_GET, &g, sizeof(g), url, (uint32_t)strlen(url) + 1, NULL, 0, g.id, &len);
    NetResponse r;
    const char *cookies = "";
    if (m && len >= sizeof(r)) {
        memcpy(&r, m, sizeof(r));
        uint64_t at = sizeof(r) + (uint64_t)r.url_len + r.headers_len + r.error_len;
        if (at + r.body_len <= len) {
            m[at + r.body_len] = 0;
            cookies = (const char *)m + at;
        }
    }
    snprintf(got, sizeof(got), "%s", cookies);
    must("document.cookie through the network process", m != NULL);
    blocked_or("read an HttpOnly cookie through document.cookie", strstr(got, "secret=") != NULL, got);
    /* Another site's cookies, by asking for its document.cookie... */
    snprintf(url, sizeof(url), "http://127.0.0.1:%s/", port);
    g.id = ++g_id;
    m = call(&g_net, TN_COOKIE_GET, &g, sizeof(g), url, (uint32_t)strlen(url) + 1, NULL, 0, g.id, &len);
    bool leaked = false;
    if (m && len >= sizeof(r)) {
        memcpy(&r, m, sizeof(r));
        leaked = r.body_len > 0;
        uint64_t at = sizeof(r) + (uint64_t)r.url_len + r.headers_len + r.error_len;
        if (at + r.body_len <= len) snprintf(got, sizeof(got), "%.*s", (int)r.body_len, m + at);
    }
    blocked_or("read another site's document.cookie", leaked, got);
    /* ...or by sending it a request (the test server echoes the Cookie header). */
    snprintf(url, sizeof(url), "http://127.0.0.1:%s/echo", port);
    bool ok = fetch(url, "GET", got, sizeof(got));
    must("a request through the network process", ok);
    blocked_or("send another site's cookies with a request", ok && got[0] != 0, got);
    /* Setting a cookie for another site (or a public suffix). */
    char line[] = "planted=1; domain=127.0.0.1; path=/";
    char set[512];
    snprintf(set, sizeof(set), "http://127.0.0.1:%s/", port);
    size_t ul = strlen(set);
    memcpy(set + ul + 1, line, sizeof(line));
    big_send(&g_net, TN_COOKIE_SET, set, (uint32_t)(ul + 1 + sizeof(line)), NULL, 0, NULL, 0, 1000);
    note("set a cookie for another site in its own jar", "the test script checks that site's own tabs never see it");
    /* Another site's page, read as a no-cors subresource (ORB), in its own
     * name (the initiator it claims isn't one it loaded), or as cors. */
    char other[64];
    snprintf(other, sizeof(other), "http://127.0.0.1:%s", port);
    snprintf(url, sizeof(url), "%s/page?from-escape", other);    /* (a page that sets no cookies) */
    blocked_or("read another site's page as a subresource",
               fetch_as(url, "GET", HTTP_MODE_NO_CORS, "", got, sizeof(got)) && strstr(got, "<html") != NULL, got);
    blocked_or("read another site's page claiming to be it",
               fetch_as(url, "GET", HTTP_MODE_NO_CORS, other, got, sizeof(got)) && strstr(got, "<html") != NULL, got);
    blocked_or("read another site's page with cors",
               fetch_as(url, "GET", HTTP_MODE_CORS, other, got, sizeof(got)) && strstr(got, "<html") != NULL, got);
    note("read another site's page as a frame document",
         fetch_as(url, "GET", HTTP_MODE_FRAME, "", got, sizeof(got)) && strstr(got, "<html")
             ? "it can, as frames share their embedder's process (section 9); without that site's own cookies"
             : "refused");
    /* Local files through the network process. */
    blocked_or("fetch file:///C:/Windows/win.ini", fetch("file:///C:/Windows/win.ini", "GET", got, sizeof(got)) &&
                                                       strstr(got, "fonts") != NULL,
               got);
}

/* P10: its own document (a navigation) is of its own site.  Another site's
 * page, through a redirect from its own site or asked for outright, isn't
 * given to it: the reply says where it went, for the UI to open in a tab of
 * that site.  A redirect within its site is followed, relative Location
 * included, and the reply says where it ended. */
static void documents(void) {
    const char *port = getenv("CYBER_ESCAPE_PORT");
    if (!port) port = "38517";
    char url[PROTO_URL_MAX + 1], target[256], expect[256], got[4096], where[2048];
    uint32_t ok;
    snprintf(target, sizeof(target), "http://127.0.0.1:%s/page?from-escape-redirect", port);
    snprintf(url, sizeof(url), "http://localhost:%s/redirect?to=%s", port, target);
    bool any = fetch_reply(url, "GET", HTTP_MODE_NAVIGATE, "", got, sizeof(got), &ok, where, sizeof(where));
    blocked_or("be given another site's document through a redirect from its own",
               ok == NET_OK && strstr(got, "<html") != NULL, got);
    must("that redirect is answered with where it went", any && ok == NET_ELSEWHERE && !strcmp(where, target));
    any = fetch_reply(target, "GET", HTTP_MODE_NAVIGATE, "", got, sizeof(got), &ok, where, sizeof(where));
    blocked_or("be given another site's document by asking for it", ok == NET_OK && strstr(got, "<html") != NULL,
               got);
    must("that request is answered with where to go", any && ok == NET_ELSEWHERE && !strcmp(where, target));
    snprintf(url, sizeof(url), "http://localhost:%s/dir/sub/redirect?to=../rel", port);
    snprintf(expect, sizeof(expect), "http://localhost:%s/dir/rel", port);
    any = fetch_reply(url, "GET", HTTP_MODE_NAVIGATE, "", got, sizeof(got), &ok, where, sizeof(where));
    must("a relative redirect within its site is followed, and the reply says where it ended",
         any && ok == NET_OK && !strcmp(got, "in-dir") && !strcmp(where, expect));
}

/* Replies left unread (SANDBOX_DESIGN.md 8.7.2): many requests for 1 MB
 * each, never read.  The network process must go on serving the other tabs
 * at once (it used to hold a worker for 30 s per reply) and stop serving
 * this one; the test script checks both.  Last on this channel. */
static void unread_replies(void) {
    const char *port = getenv("CYBER_ESCAPE_PORT");
    if (!port) port = "38517";
    char url[256], buf[512];
    snprintf(url, sizeof(url), "http://localhost:%s/big", port);
    NetRequest q;
    memset(&q, 0, sizeof(q));
    q.method_len = 4;
    q.url_len = (uint32_t)strlen(url) + 1;
    q.mode = HTTP_MODE_NO_CORS;
    memcpy(buf, "GET", 4);
    memcpy(buf + 4, url, q.url_len);
    int sent = 0, stalled = 0;
    for (int i = 0; i < 600 && stalled < 5; i++) {      /* (stalled: it stopped reading us) */
        q.id = ++g_id;
        if (big_send(&g_net, TN_REQUEST, &q, sizeof(q), buf, q.method_len + q.url_len, NULL, 0, 200)) {
            sent++;
            stalled = 0;
        } else {
            stalled++;
        }
    }
    note("left its replies unread", "the test script checks the network process stopped serving it and served the "
         "next tabs");
    printf("[escape] sent %d requests for 1 MB, reading none of the replies\n", sent);
    fflush(stdout);
    Sleep(5000);
}

/* ---- malformed messages and shared memory ------------------------------------------ */

static uint32_t g_rand = 12345;
static uint32_t rnd(void) {
    g_rand ^= g_rand << 13;
    g_rand ^= g_rand >> 17;
    g_rand ^= g_rand << 5;
    return g_rand;
}

/* Raw parts: random types and lengths, lying big-message headers, parts
 * out of order, messages that never finish. */
static void fuzz(Chan *c, const char *name, int n) {
    static uint8_t buf[CHAN_MSG_MAX];
    int sent = 0, stalled = 0;
    for (int i = 0; i < n; i++) {
        uint32_t len = rnd() % (CHAN_MSG_MAX + 1);
        for (uint32_t k = 0; k < len; k++) buf[k] = (uint8_t)rnd();
        uint16_t type;
        switch (rnd() % 6) {
        case 0: type = (uint16_t)rnd(); break;
        case 1: type = BIG_MORE; break;                               /* a part of nothing */
        case 2:                                                       /* claims 256 MB, never sends it */
            type = (uint16_t)(1 + rnd() % 4);
            if (len < 4) len = 4;
            { uint32_t t = BIG_MAX; memcpy(buf, &t, 4); }
            break;
        case 3:                                                       /* a claim smaller than the part */
            type = (uint16_t)(1 + rnd() % 4);
            if (len < 8) len = 8;
            { uint32_t t = 2; memcpy(buf, &t, 4); }
            break;
        case 4:                                                       /* a header whose lengths overflow */
            type = (uint16_t)(1 + rnd() % 4);
            len = 4 + 24;
            { uint32_t t = 24, h[6] = { rnd(), 0xffffffffu, 0xfffffff0u, 0x20u, rnd(), 0xffffffffu };
              memcpy(buf, &t, 4); memcpy(buf + 4, h, 24); }
            break;
        default: type = (uint16_t)(1 + rnd() % 4); break;
        }
        if (chan_send(c, type, buf, len, 50)) {
            sent++;
            stalled = 0;
        } else if (++stalled == 20) {
            break;                      /* the peer stopped reading: it dropped us */
        }
    }
    printf("[escape] sent %d malformed parts to the %s%s\n", sent, name,
           stalled >= 20 ? ", which then stopped reading the channel" : "");
    fflush(stdout);
}

/* Well-formed messages with hostile contents: every request type, with
 * header lengths that lie (zero, huge, off by one, summing past the end),
 * strings without their NULs, garbage URLs and paths.  These reach the
 * servers' request handlers; afterwards the servers must still answer. */
static void fuzz_requests(Chan *c, const char *name, int n, uint16_t first, uint16_t last) {
    static uint8_t buf[4096];
    static const uint32_t edges[] = { 0, 1, 2, 7, 8, 127, 128, 600, 601, 4096, 65535, 0x7fffffffu, 0x80000000u,
                                      0xfffffff0u, 0xffffffffu };
    static const char *const strs[] = { "GET", "http://", "http://[::1", "file:///C:/Windows/win.ini",
                                        "javascript:1", "../../x", "cookies", "https+a.com", "", "\xff\xfe" };
    int sent = 0;
    for (int i = 0; i < n; i++) {
        uint32_t len = 24 + rnd() % 400;
        for (uint32_t k = 0; k < len; k++) buf[k] = (uint8_t)rnd();
        /* The header: ids and lengths, mostly edge values. */
        for (uint32_t k = 0; k < 6; k++) {
            uint32_t v = rnd() % 3 ? edges[rnd() % (sizeof(edges) / sizeof(edges[0]))] : rnd() % 64;
            memcpy(buf + 4 * k, &v, 4);
        }
        /* Some real strings after it. */
        if (rnd() % 2) {
            const char *s = strs[rnd() % (sizeof(strs) / sizeof(strs[0]))];
            size_t sl = strlen(s);
            if (24 + sl < len) memcpy(buf + 24, s, sl + (rnd() % 2));
        }
        uint16_t type = (uint16_t)(first + rnd() % (last - first + 1));
        if (big_send(c, type, buf, len, NULL, 0, NULL, 0, 200)) sent++;
    }
    printf("[escape] sent %d malformed requests to the %s\n", sent, name);
    fflush(stdout);
}

/* Malformed UI messages: impossible frames, a huge title, navigations to
 * file: and javascript: URLs (the coordinator only starts http(s) tabs). */
static void fuzz_ui(void) {
    TuFrame f = { 1, 7, SURFACE_MAX_W + 1, SURFACE_MAX_H * 4, 0xffffffffu, 0xffffffffu };
    chan_send(&g_ui, TU_FRAME, &f, sizeof(f), 100);
    f.unused = 1; f.width = 0xffffffffu; f.height = 1;
    chan_send(&g_ui, TU_FRAME, &f, sizeof(f), 100);
    chan_send(&g_ui, TU_FRAME, &f, 3, 100);
    static char title[CHAN_MSG_MAX];
    memset(title, 'A', sizeof(title));
    chan_send(&g_ui, TU_TITLE, title, sizeof(title), 100);
    static const char *const urls[] = { "file:///C:/Windows/win.ini", "javascript:alert(1)", "about:config",
                                        "chrome://settings", "\\\\?\\C:\\Windows\\win.ini" };
    for (size_t i = 0; i < sizeof(urls) / sizeof(urls[0]); i++) {
        uint8_t m[256];
        TuNavigate nv = { (uint32_t)(i & 1) };
        memcpy(m, &nv, sizeof(nv));
        size_t n = strlen(urls[i]);
        memcpy(m + sizeof(nv), urls[i], n);
        chan_send(&g_ui, TU_NAVIGATE, m, (uint32_t)(sizeof(nv) + n), 100);
    }
    fuzz(&g_ui, "UI", 300);
}

/* Rewrite the rings' indices in shared memory while the peer reads them. */
static void scribble(HANDLE section, const char *name) {
    uint8_t *p = (uint8_t *)MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, 4096);
    if (!p) return;
    for (int round = 0; round < 20000; round++) {
        uint32_t at = 64 + (rnd() % (2 * 192)) / 8 * 8;              /* a ring's tail, head or waiting */
        uint64_t v = rnd() % 4 == 0 ? ((uint64_t)rnd() << 32 | rnd()) : rnd() % 70000;
        memcpy(p + at, &v, 8);
        if (round % 1000 == 0) Sleep(1);
    }
    UnmapViewOfFile(p);
    printf("[escape] scribbled on the %s channel's indices\n", name);
    fflush(stdout);
}

/* ---- the GPU process ------------------------------------------------------------------- */

/* Where the frames say their layer lies in the page (they may lie too). */
static GpuFrame g_place = { 0, 0, 0, 0, 0, 0, 600, 0, 0, 600 };

/* A TG_FRAME of n records like c (and nm masks like m), its header's counts
 * as given (they may lie). */
static bool gpu_frame(uint32_t frame, uint32_t ncmds, uint32_t nmasks, const WireCmd *c, uint32_t n,
                      const WireMask *m, uint32_t nm) {
    size_t bytes = sizeof(GpuFrame) + (size_t)n * sizeof(WireCmd) + (size_t)nm * sizeof(WireMask);
    uint8_t *buf = (uint8_t *)malloc(bytes);
    if (!buf) return false;
    GpuFrame f = g_place;
    f.frame = frame;
    f.width = 800;
    f.height = 600;
    f.ncmds = ncmds;
    f.nmasks = nmasks;
    memcpy(buf, &f, sizeof(f));
    for (uint32_t i = 0; i < n; i++) memcpy(buf + sizeof(f) + i * sizeof(WireCmd), c, sizeof(WireCmd));
    for (uint32_t i = 0; i < nm; i++)
        memcpy(buf + sizeof(f) + n * sizeof(WireCmd) + i * sizeof(WireMask), m, sizeof(WireMask));
    bool ok = big_send(&g_gpu, TG_FRAME, buf, (uint32_t)bytes, NULL, 0, NULL, 0, 2000);
    free(buf);
    return ok;
}

/* Wait for GT_FRAME_DONE of `frame`; false if anything else comes back, or
 * nothing within ms. */
static bool gpu_done(uint32_t frame, DWORD ms, bool *other) {
    static BigRx rx;
    DWORD start = GetTickCount();
    while (GetTickCount() - start < ms) {
        int r = big_recv(&g_gpu, &rx);
        if (r < 0) return false;
        if (r == 0) {
            chan_wait(&g_gpu, 20);
            continue;
        }
        if (rx.type != GT_FRAME_DONE || rx.total != sizeof(GtFrameDone)) {
            *other = true;
            continue;
        }
        GtFrameDone d;
        memcpy(&d, rx.buf, sizeof(d));
        if (d.frame == frame) return true;
    }
    return false;
}

/* The GPU process draws every tab's content; a tab may name only what it
 * uploaded itself, and nothing comes back but "drawn". */
static void gpu(void) {
    if (!g_have_gpu) {
        printf("[escape] FAILED: no GPU channel\n");
        g_failed++;
        return;
    }
    bool other = false;
    WireCmd c;
    memset(&c, 0, sizeof(c));
    c.w = c.h = 100 * WIRE_UNIT;
    c.a = 65535;
    WireMask m;
    memset(&m, 0, sizeof(m));
    uint32_t frame = 1000;
    /* Other tabs' images and font slots: never uploaded here. */
    c.type = WIRE_IMAGE;
    int drawn = 0;
    for (uint32_t id = 0; id < 64; id++) {
        c.ref = id;
        if (gpu_frame(++frame, 1, 0, &c, 1, NULL, 0) && gpu_done(frame, 2000, &other)) drawn++;
    }
    c.type = WIRE_GLYPH;
    for (uint32_t slot = 0; slot < 300; slot += 7) {
        c.ref = slot;
        if (gpu_frame(++frame, 1, 0, &c, 1, NULL, 0) && gpu_done(frame, 2000, &other)) drawn++;
    }
    printf("[escape] note sent %d frames naming other tabs' image ids and font slots: the GPU process looks them up in this tab's namespace only (the proofs, P5)\n", drawn);
    /* Lying counts, extreme numbers, brackets that don't balance, masks of
     * no kind, stops past the end. */
    c.type = WIRE_RECT;
    c.x = c.y = (int32_t)0x80000000;
    c.w = c.h = 0x7fffffff;
    c.u0 = c.v1 = 0xffffffffu;
    gpu_frame(++frame, 1000000, 0, &c, 1, NULL, 0);          /* says a million, sends one */
    gpu_frame(++frame, 0xffffffffu, 0xffffffffu, &c, 1, NULL, 0);
    gpu_frame(++frame, 1, 0, &c, 1, NULL, 0);
    gpu_done(frame, 2000, &other);
    c.type = WIRE_MASK_BEGIN;
    c.ref = 0;
    m.kind = 77;
    m.nstops = 0xffffffffu;
    m.image = 5;
    gpu_frame(++frame, 2000, 1, &c, 2000, &m, 1);            /* 2000 opens, a mask of no kind */
    gpu_done(frame, 5000, &other);
    m.kind = 2;
    gpu_frame(++frame, 2000, 1, &c, 2000, &m, 1);            /* 2000 opens of a good mask */
    gpu_done(frame, 5000, &other);
    c.type = WIRE_MASK_END;
    gpu_frame(++frame, 500, 0, &c, 500, NULL, 0);            /* closes of nothing */
    gpu_done(frame, 2000, &other);
    c.type = WIRE_SHADOW;
    c.ref = 0xfffffff0u;
    gpu_frame(++frame, 10, 1, &c, 10, &m, 1);
    gpu_done(frame, 2000, &other);
    /* Uploads that lie: a slot past the end, rows past the atlas, a size
     * that doesn't match the bytes, and more images than a tab may keep. */
    static uint8_t junk[1 << 16];
    for (size_t i = 0; i < sizeof(junk); i++) junk[i] = (uint8_t)rnd();
    TgAtlas a = { 300, 1024, 1024, 0, 1 };
    big_send(&g_gpu, TG_ATLAS, &a, sizeof(a), junk, 1024, NULL, 0, 2000);
    a.slot = 3;
    a.y0 = 1023;
    a.rows = 50;
    big_send(&g_gpu, TG_ATLAS, &a, sizeof(a), junk, 50 * 1024, NULL, 0, 2000);
    TgImage t = { 99999, 16384, 16384 };
    big_send(&g_gpu, TG_IMAGE, &t, sizeof(t), junk, sizeof(junk), NULL, 0, 2000);
    t.id = 7;
    t.width = t.height = 64;
    big_send(&g_gpu, TG_IMAGE, &t, sizeof(t), junk, 100, NULL, 0, 2000);
    uint8_t *big = (uint8_t *)malloc(64u << 20);
    int over = 0;
    if (big) {
        memset(big, 0x5a, 64u << 20);
        TgImage b = { 0, 4096, 4096 };
        for (uint32_t id = 100; id < 100 + 24; id++) {          /* 1.5 GB asked for, 1 GB allowed */
            b.id = id;
            if (big_send(&g_gpu, TG_IMAGE, &b, sizeof(b), big, 64u << 20, NULL, 0, 10000)) over++;
        }
        free(big);
    }
    printf("[escape] note uploaded %d images of 64 MB: past a tab's 1 GB the GPU process keeps no more\n", over);
    /* Shown in the UI's window (a frame it believes), frames that say their
     * layer lies anywhere at all: the GPU process clamps where it shows it. */
    TuFrame shown = { frame, 0, 800, 600, 0, 600 };
    chan_send(&g_ui, TU_FRAME, &shown, sizeof(shown), 100);
    static const uint32_t places[][5] = {
        { 0xffffffffu, 0, 0xffffffffu, 0xffffffffu, 0xffffffffu }, { 0, 0xffffffffu, 0x7fffffffu, 0, 0x80000000u },
        { 0x80000000u, 1, 0, 0x80000000u, 0xffffffffu }, { 5000, 600, 100, 3, 0 },
    };
    c.type = WIRE_RECT;
    c.x = c.y = 0;
    c.w = c.h = 100 * WIRE_UNIT;
    c.u0 = c.v1 = 0;
    int placed = 0;
    for (size_t i = 0; i < sizeof(places) / sizeof(places[0]); i++) {
        g_place.origin_y = places[i][0];
        g_place.view_h = places[i][1];
        g_place.scroll_y = places[i][2];
        g_place.scroll_seq = places[i][3];
        g_place.content_h = places[i][4];
        if (gpu_frame(++frame, 1, 0, &c, 1, NULL, 0) && gpu_done(frame, 2000, &other)) placed++;
        Sleep(100);                     /* presented in between */
    }
    g_place.origin_y = g_place.scroll_y = g_place.scroll_seq = 0;
    g_place.view_h = g_place.content_h = 600;
    printf("[escape] note sent %d frames placing their layer anywhere at all\n", placed);
    /* It still draws for us, and never sent anything but "drawn". */
    c.type = WIRE_RECT;
    c.x = c.y = 0;
    c.w = c.h = 10 * WIRE_UNIT;
    c.ref = 0;
    c.u0 = c.v1 = 0;
    bool answers = gpu_frame(++frame, 1, 0, &c, 1, NULL, 0) && gpu_done(frame, 20000, &other);
    must("the GPU process draws after them", answers);
    must("the GPU process sends a tab nothing but \"drawn\" (no pixels back)", !other);
    fflush(stdout);
}

/* ---- the site's web-GPU process -------------------------------------------------------- */

/* Asked for as a tab asks (through the UI), granted on the control channel. */
static bool wgpu_connect(void) {
    if (!g_have_ctl || !chan_send(&g_ui, TU_WANT_WEBGPU, NULL, 0, 1000)) return false;
    static ChanMsg m;
    for (int i = 0; i < 150; i++) {
        int r = chan_recv(&g_ctl, &m);
        if (r < 0) return false;
        if (r == 0) { chan_wait(&g_ctl, 100); continue; }
        if (m.type == CT_WEBGPU_FAILED) return false;
        if (m.type != CT_WEBGPU || m.len != sizeof(CtGpu)) continue;
        CtGpu g;
        memcpy(&g, m.data, sizeof(g));
        return chan_attach(&g_wgpu, (HANDLE)(uintptr_t)g.chan_section, (HANDLE)(uintptr_t)g.chan_ev0,
                           (HANDLE)(uintptr_t)g.chan_ev1, 0);
    }
    return false;
}

/* The next message of type t (its body in rx), or false after ms. */
static bool wgpu_reply(BigRx *rx, uint16_t t, DWORD ms) {
    unsigned long long until = GetTickCount64() + ms;
    for (;;) {
        int r = big_recv(&g_wgpu, rx);
        if (r < 0) return false;
        if (r > 0 && rx->type == t) return true;
        if (r == 0) {
            if (GetTickCount64() >= until) return false;
            chan_wait(&g_wgpu, 50);
        }
    }
}

/* A WebGL context 1 cleared green and read back: true if it reads green. */
static bool wgpu_gl_works(BigRx *rx) {
    static const uint32_t recs[] = {
        4, 8, 0x8D40, 0,                    /* bindFramebuffer(FRAMEBUFFER, null) */
        13, 16, 0, 0x3f800000u, 0, 0x3f800000u,    /* clearColor(0, 1, 0, 1) */
        12, 4, 0x4000,                      /* clear(COLOR_BUFFER_BIT) */
        313, 28, 0, 0, 1, 1, 0x1908, 0x1401, 4,    /* readPixels: 4 bytes */
    };
    TwGlRun q = { 1, 77, 1 };
    if (!big_send(&g_wgpu, TW_GL_RUN, &q, sizeof(q), recs, sizeof(recs), NULL, 0, 2000)) return false;
    if (!wgpu_reply(rx, WT_GL_RESULT, 10000) || rx->total < sizeof(WtResult) + 8) return false;
    const uint8_t *px = rx->buf + sizeof(WtResult) + 4;
    return px[0] == 0 && px[1] == 255 && px[2] == 0;
}

static void webgpu(void) {
    g_have_wgpu = wgpu_connect();
    must("the site's web-GPU process is granted, as to any tab", g_have_wgpu);
    if (!g_have_wgpu) return;
    static BigRx rx;
    TwGlCreate cr = { 1, 2, 64, 64, 0 };
    big_send(&g_wgpu, TW_GL_CREATE, &cr, sizeof(cr), NULL, 0, NULL, 0, 2000);
    bool made = wgpu_reply(&rx, WT_GL_CREATED, 10000) && rx.total >= sizeof(WtCreated) && ((WtCreated *)rx.buf)->ok;
    must("a WebGL context", made && wgpu_gl_works(&rx));
    /* Contexts it may not have: huge, of no version, past the limit. */
    static const uint32_t bad[][5] = { { 2, 2, 100000, 100000, 0 }, { 3, 7, 1, 1, 0xffffffffu },
                                       { 0, 1, 0, 0, 0 }, { 0xffffffffu, 2, 0xffffffffu, 1, 31 } };
    int refused = 0;
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TwGlCreate b = { bad[i][0], bad[i][1], bad[i][2], bad[i][3], bad[i][4] };
        big_send(&g_wgpu, TW_GL_CREATE, &b, sizeof(b), NULL, 0, NULL, 0, 2000);
        if (wgpu_reply(&rx, WT_GL_CREATED, 10000) && rx.total >= sizeof(WtCreated) && !((WtCreated *)rx.buf)->ok) refused++;
    }
    printf("[escape] note asked for %d impossible WebGL contexts: %d refused (huge ones are clamped by ANGLE)\n",
           (int)(sizeof(bad) / sizeof(bad[0])), refused);
    /* Other tabs' contexts and canvases: ids are this tab's own, so these
     * name nothing (or this tab's), never another tab's pixels. */
    int frames = 0;
    for (uint32_t id = 1; id < 300; id += 13) {
        TwPresent pr = { id, 1000 + id };
        if (!big_send(&g_wgpu, TW_PRESENT, &pr, sizeof(pr), NULL, 0, NULL, 0, 2000)) break;
        if (wgpu_reply(&rx, WT_FRAME, 5000) && rx.total > sizeof(WtFrame) && ((WtFrame *)rx.buf)->ctx != 1) frames++;
    }
    blocked_or("present another tab's canvas (its pixels sent here)", frames > 0, "pixels for a context it never made");
    /* WebGL records that lie: random ops and lengths, ids never made. */
    static uint8_t recs[8192];
    for (int i = 0; i < 2000; i++) {
        uint32_t len = 0;
        while (len + 16 < sizeof(recs) && rnd() % 8) {
            uint32_t op = rnd() % 3 ? 1 + rnd() % 101 : rnd() % 2 ? 256 + rnd() % 21 : 300 + rnd() % 41;
            uint32_t n = rnd() % 4 ? 4 * (rnd() % 8) : rnd() % 3 ? rnd() % 64 : rnd();
            memcpy(recs + len, &op, 4);
            memcpy(recs + len + 4, &n, 4);
            len += 8;
            for (uint32_t k = 0; k < n && len < sizeof(recs); k++) recs[len++] = (uint8_t)(rnd() % 3 ? rnd() % 16 : rnd());
        }
        TwGlRun q = { 1 + rnd() % 3, (uint32_t)i, rnd() % 4 == 0 };
        big_send(&g_wgpu, TW_GL_RUN, &q, sizeof(q), recs, len, NULL, 0, 2000);
        if (q.reply) wgpu_reply(&rx, WT_GL_RESULT, 10000);
        if (i % 100 == 0) {
            TwGlResize rs = { 1, rnd() % 5000, rnd() % 5000 };
            big_send(&g_wgpu, TW_GL_RESIZE, &rs, sizeof(rs), NULL, 0, NULL, 0, 2000);
        }
    }
    TwGlResize back = { 1, 64, 64 };
    big_send(&g_wgpu, TW_GL_RESIZE, &back, sizeof(back), NULL, 0, NULL, 0, 2000);
    printf("[escape] note sent 2000 batches of lying WebGL records\n");
    /* WebGPU records that lie: trees nested past the limit, counts that
     * claim the world, ids never made, an adapter asked for and misused. */
    static uint8_t gpu[70000];
    uint32_t len = 0, deep = 3000;
    uint32_t op = 1, n = 4 + deep + 1, id = 9;
    memcpy(gpu, &op, 4); memcpy(gpu + 4, &n, 4); memcpy(gpu + 8, &id, 4);
    len = 12;
    for (uint32_t k = 0; k < deep; k++) gpu[len++] = 5;             /* [[[[... arrays without counts */
    gpu[len++] = 0;
    TwGpuRun gr = { 1, 1 };
    big_send(&g_wgpu, TW_GPU_RUN, &gr, sizeof(gr), gpu, len, NULL, 0, 2000);
    bool answered = wgpu_reply(&rx, WT_GPU_RESULT, 10000);
    for (int i = 0; i < 1500; i++) {
        len = 0;
        while (len + 64 < sizeof(gpu) && rnd() % 6) {
            uint32_t o = rnd() % 4 ? 1 + rnd() % 33 : rnd(), at = len;
            memcpy(gpu + len, &o, 4);
            len += 8;
            for (int k = 0; k < 3; k++) { uint32_t v = rnd() % 3 ? rnd() % 40 : rnd(); memcpy(gpu + len, &v, 4); len += 4; }
            uint32_t junk = rnd() % 4 ? rnd() % 40 : rnd() % 6000;
            for (uint32_t k = 0; k < junk && len < sizeof(gpu); k++) gpu[len++] = (uint8_t)(rnd() % 2 ? rnd() % 9 : rnd());
            uint32_t nn = rnd() % 10 ? len - at - 8 : rnd();
            memcpy(gpu + at + 4, &nn, 4);
        }
        TwGpuRun q = { 2 + (uint32_t)i, rnd() % 4 == 0 };
        big_send(&g_wgpu, TW_GPU_RUN, &q, sizeof(q), gpu, len, NULL, 0, 2000);
        if (q.reply) wgpu_reply(&rx, WT_GPU_RESULT, 15000);
    }
    printf("[escape] note sent a tree nested %u deep (%s) and 1500 batches of lying WebGPU records\n", deep,
           answered ? "refused" : "no answer");
    must("the web-GPU process still draws for us after them", wgpu_gl_works(&rx));
    fflush(stdout);
}

/* ---- main ---------------------------------------------------------------------------- */

static void unhex(const char *h) {
    size_t n = strlen(h) / 2;
    if (n > PROTO_URL_MAX) n = PROTO_URL_MAX;
    for (size_t i = 0; i < n; i++) {
        unsigned v = 0;
        sscanf(h + 2 * i, "%2x", &v);
        g_url[i] = (char)v;
    }
    g_url[n] = 0;
}

int main(int argc, char **argv) {
    HANDLE ui[3] = { 0 }, net[3] = { 0 }, st[3] = { 0 }, gp[3] = { 0 }, ct[3] = { 0 };
    for (int i = 1; i < argc; i++) {
        HANDLE *h = !strcmp(argv[i], "--chan") ? ui : !strcmp(argv[i], "--net") ? net
                  : !strcmp(argv[i], "--store") ? st : !strcmp(argv[i], "--gpu") ? gp
                  : !strcmp(argv[i], "--ctl") ? ct : NULL;
        if (h && i + 3 < argc) {
            for (int k = 0; k < 3; k++) h[k] = spawn_handle_from_arg(argv[++i]);
        } else if (!strcmp(argv[i], "--tab") && i + 1 < argc) {
            g_tab = (uint32_t)atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--url") && i + 1 < argc) {
            unhex(argv[++i]);
        }
    }
    if (!chan_attach(&g_ui, ui[0], ui[1], ui[2], 1) || !chan_attach(&g_net, net[0], net[1], net[2], 0)) {
        printf("[escape] FAILED: no channels\n");
        return 100;
    }
    g_have_store = st[0] && chan_attach(&g_store, st[0], st[1], st[2], 0);
    g_have_gpu = gp[0] && chan_attach(&g_gpu, gp[0], gp[1], gp[2], 0);
    g_have_ctl = ct[0] && chan_attach(&g_ctl, ct[0], ct[1], ct[2], 1);
    char scheme[8], host[SITE_HOST_MAX + 1];
    if (site_url_host((const uint8_t *)g_url, (uint32_t)strlen(g_url), scheme, sizeof(scheme), host, sizeof(host)))
        site_of(scheme, host, g_site);
    printf("[escape] tab %u, %s, site %s\n", g_tab, g_url, g_site);
    fflush(stdout);
    TuState s = { TAB_LOADED };
    chan_send(&g_ui, TU_STATE, &s, sizeof(s), 100);

    files();
    registry();
    sockets();
    processes();
    memory();
    storage();
    network();
    documents();
    gpu();
    webgpu();

    /* Malformed requests: answered or ignored, never fatal. */
    char got[64];
    fuzz_requests(&g_store, "storage service", 3000, TS_READ, TS_DELETE);
    must("the storage service answers after them",
         store(TS_WRITE, NULL, "escape/after", "y", 1, NULL, 0) && store(TS_READ, NULL, "escape/after", NULL, 0, got,
                                                                        sizeof(got)) && !strcmp(got, "y"));
    store(TS_DELETE, NULL, "escape/after", NULL, 0, NULL, 0);
    fuzz_requests(&g_net, "network process", 3000, TN_REQUEST, TN_COOKIE_SET);
    char url[64];
    snprintf(url, sizeof(url), "http://localhost:%s/ping", getenv("CYBER_ESCAPE_PORT") ? getenv("CYBER_ESCAPE_PORT")
                                                                                       : "38517");
    must("the network process answers after them", fetch(url, "GET", got, sizeof(got)) && !strcmp(got, "pong"));
    unread_replies();

    /* Broken framing and rings: the servers drop the channel; the test
     * script checks they live on and serve the next tabs. */
    fuzz(&g_store, "storage service", 3000);
    fuzz(&g_net, "network process", 3000);
    if (g_have_gpu) fuzz(&g_gpu, "GPU process", 3000);
    if (g_have_wgpu) fuzz(&g_wgpu, "web-GPU process", 3000);
    scribble(st[0], "storage");
    scribble(net[0], "network");
    if (g_have_gpu) scribble(gp[0], "GPU");
    windows();
    printf("[escape] done: %d attempts, %d escaped, %d failed\n", g_attempts, g_escaped, g_failed);
    fflush(stdout);

    /* Last, the UI: it ends a tab that navigates (every navigation gets a
     * new process), so this may be the end of us. */
    scribble(ui[0], "UI");
    fuzz_ui();
    /* Stay up a while, so the servers deal with the wreckage while we live. */
    Sleep(3000);
    return g_escaped + g_failed;
}

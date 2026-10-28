/* coordinator.c - cb_coord.exe: the coordinator's Win32 shim
 * (SANDBOX_DESIGN.md section 2.1).
 *
 *   cb_coord.exe [url ...]      open a window with a tab for each url
 *
 * It starts the network process (cb_net.exe), the storage service
 * (cb_store.exe), the loopback relay (cb_loop.exe) and the UI (cb_ui.exe), and
 * connects the network process to the storage service and to the relay.  It turns the UI's messages and its children's exits
 * into events for the core (coord_core.c), and performs the core's actions:
 * starting a tab process with its channels (to the UI, to the network process
 * and, unless its site is opaque, to the storage service, both badged with
 * the site the core gave it) and its surface; revoking those channels when the
 * tab ends; messaging the UI.  Every child runs in a job that ends with the
 * coordinator, and in the sandbox for its kind (sandbox.c).  Each child's
 * output goes to logs\<name>.log next to the executables.
 *
 *   cb_coord.exe --remove-profiles   delete the sandbox's AppContainer profiles
 *
 * CYBER_NO_SANDBOX=1 starts the children unconfined; CYBER_UI_SCRIPT (tests
 * drive the UI and save screenshots) leaves the UI unconfined.
 * CYBER_TAB_EXE=cb_escape.exe starts the escape tests in place of the tabs
 * of URLs that end in "#escape". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "chan.h"
#include "spawn.h"
#include "proto.h"
#include "coord_core.h"
#include "sandbox.h"

_Static_assert(PROTO_SITE_MAX == SITE_MAX, "a badge is a site label");

/* A child with a control channel from the coordinator. */
typedef struct Child {
    HANDLE proc;
    Chan ctl;                           /* side 0: the coordinator */
} Child;

static HANDLE g_job;
static Child g_ui, g_net, g_store, g_loop;
static HANDLE g_tab_proc[COORD_MAX_TABS];
static CoordState g_state;
static CoordActions g_actions;          /* large: kept off the stack */
static wchar_t g_root[MAX_PATH];        /* browser_data\sites */

static HANDLE open_log(const char *name) {
    wchar_t path[MAX_PATH * 2];
    _snwprintf(path, MAX_PATH * 2, L"%lslogs", spawn_exe_dir());
    CreateDirectoryW(path, NULL);
    _snwprintf(path, MAX_PATH * 2, L"%lslogs\\%hs.log", spawn_exe_dir(), name);
    path[MAX_PATH * 2 - 1] = 0;
    return CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
}

/* Bytes as hex, for a command line. */
static void hex(char *out, const uint8_t *p, uint32_t n) {
    static const char d[] = "0123456789abcdef";
    for (uint32_t i = 0; i < n; i++) {
        out[2 * i] = d[p[i] >> 4];
        out[2 * i + 1] = d[p[i] & 15];
    }
    out[2 * n] = 0;
}

static HANDLE dup_to(HANDLE proc, HANDLE h, DWORD access, bool same) {
    HANDLE out = NULL;
    if (!DuplicateHandle(GetCurrentProcess(), h, proc, &out, access, FALSE,
                         same ? DUPLICATE_SAME_ACCESS : 0))
        return NULL;
    return out;
}

/* Undo dup_to for handles the other process never got told about. */
static void close_in(HANDLE proc, HANDLE h) {
    if (h) DuplicateHandle(proc, h, NULL, NULL, 0, FALSE, DUPLICATE_CLOSE_SOURCE);
}

/* Start <exe> with a control channel (and extra arguments). */
static bool start_child(Child *c, const wchar_t *exe, const char *log_name, const char *extra, DWORD flags,
                        int kind) {
    if (!chan_create(&c->ctl, 1u << 18)) return false;
    static char args[16384];
    snprintf(args, sizeof(args), "--chan %llu %llu %llu%s%s", spawn_handle_arg(c->ctl.section),
             spawn_handle_arg(c->ctl.ev[0]), spawn_handle_arg(c->ctl.ev[1]), extra ? " " : "", extra ? extra : "");
    HANDLE log = open_log(log_name);
    HANDLE inh[3] = { c->ctl.section, c->ctl.ev[0], c->ctl.ev[1] };
    SpawnSandbox sb;
    bool confined = kind < 0 || sandbox_for((SandboxKind)kind, 0, &sb);
    c->proc = confined ? spawn_child(exe, args, inh, 3, log != INVALID_HANDLE_VALUE ? log : NULL, g_job, flags,
                                     kind < 0 ? NULL : &sb)
                       : NULL;
    if (confined && kind >= 0) sandbox_release(&sb);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    return c->proc != NULL;
}

/* Hand a server the server end of channel `ch` for a client, with its badge. */
static bool grant(Child *server, const Chan *ch, uint32_t client, uint32_t kind, const char *site) {
    HANDLE s = dup_to(server->proc, ch->section, 0, true), e0 = dup_to(server->proc, ch->ev[0], 0, true),
           e1 = dup_to(server->proc, ch->ev[1], 0, true);
    CsGrant g;
    memset(&g, 0, sizeof(g));
    g.client = client;
    g.kind = kind;
    g.chan_section = spawn_handle_arg(s);
    g.chan_ev0 = spawn_handle_arg(e0);
    g.chan_ev1 = spawn_handle_arg(e1);
    snprintf(g.site, sizeof(g.site), "%s", site ? site : "");
    if (s && e0 && e1 && chan_send(&server->ctl, CS_GRANT, &g, sizeof(g), 1000)) return true;
    close_in(server->proc, s);
    close_in(server->proc, e0);
    close_in(server->proc, e1);
    return false;
}

static void revoke(Child *server, uint32_t client) {
    CsRevoke v = { client };
    chan_send(&server->ctl, CS_REVOKE, &v, sizeof(v), 1000);
}

static bool start_tab(const CoordAction *a) {
    Chan ui, net, store;
    bool have_store = a->grant_store;       /* the core: not for an opaque site */
    memset(&store, 0, sizeof(store));
    if (!chan_create(&ui, 1u << 18)) return false;
    if (!chan_create(&net, 1u << 20)) { chan_close(&ui); return false; }
    if (have_store && !chan_create(&store, 1u << 18)) { chan_close(&ui); chan_close(&net); return false; }
    HANDLE surface = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                        (DWORD)(SURFACE_BYTES >> 32), (DWORD)SURFACE_BYTES, NULL);
    char name[32];
    snprintf(name, sizeof(name), "tab-%u", a->tab);
    HANDLE log = open_log(name);
    bool ok = false;
    if (surface) {
        static char args[PROTO_URL_MAX * 2 + 512];
        char url_hex[PROTO_URL_MAX * 2 + 1], store_arg[80] = "";
        hex(url_hex, a->data, a->len);
        if (have_store)
            snprintf(store_arg, sizeof(store_arg), " --store %llu %llu %llu", spawn_handle_arg(store.section),
                     spawn_handle_arg(store.ev[0]), spawn_handle_arg(store.ev[1]));
        snprintf(args, sizeof(args),
                 "--chan %llu %llu %llu --net %llu %llu %llu%s --surface %llu --size %u %u --tab %u --url %s",
                 spawn_handle_arg(ui.section), spawn_handle_arg(ui.ev[0]), spawn_handle_arg(ui.ev[1]),
                 spawn_handle_arg(net.section), spawn_handle_arg(net.ev[0]), spawn_handle_arg(net.ev[1]),
                 store_arg, spawn_handle_arg(surface), a->width, a->height, a->tab, url_hex);
        HANDLE inh[10] = { ui.section, ui.ev[0], ui.ev[1], net.section, net.ev[0], net.ev[1], surface };
        int ninh = 7;
        if (have_store) {
            inh[ninh++] = store.section;
            inh[ninh++] = store.ev[0];
            inh[ninh++] = store.ev[1];
        }
        static wchar_t exe[64];
        const char *alt = getenv("CYBER_TAB_EXE");
        bool escape = a->len >= 7 && !memcmp(a->data + a->len - 7, "#escape", 7);
        _snwprintf(exe, 64, L"%hs", alt && alt[0] && escape ? alt : "cb_tab.exe");
        SpawnSandbox sb;
        HANDLE proc = NULL;
        if (sandbox_for(SB_TAB, a->slot, &sb)) {
            /* Detached: a console would come with a console host process,
             * which the tab could open. */
            proc = spawn_child(exe, args, inh, ninh, log != INVALID_HANDLE_VALUE ? log : NULL, g_job,
                               DETACHED_PROCESS, &sb);
            sandbox_release(&sb);
        } else {
            fprintf(stderr, "cb_coord: no sandbox for tab %u\n", a->tab);
        }
        if (!proc) sandbox_tab_ended(a->slot);
        if (proc) {
            /* The servers first: the tab may already be asking. */
            bool granted = a->grant_net && grant(&g_net, &net, a->tab, GRANT_TAB, a->site) &&
                           (!have_store || grant(&g_store, &store, a->tab, GRANT_TAB, a->site));
            CuTabStarted m;
            memset(&m, 0, sizeof(m));
            m.tab = a->tab;
            HANDLE hs = dup_to(g_ui.proc, ui.section, 0, true), h0 = dup_to(g_ui.proc, ui.ev[0], 0, true),
                   h1 = dup_to(g_ui.proc, ui.ev[1], 0, true),
                   hsurf = dup_to(g_ui.proc, surface, FILE_MAP_READ, false);
            m.chan_section = spawn_handle_arg(hs);
            m.chan_ev0 = spawn_handle_arg(h0);
            m.chan_ev1 = spawn_handle_arg(h1);
            m.surface = spawn_handle_arg(hsurf);
            m.surface_bytes = SURFACE_BYTES;
            if (granted && hs && h0 && h1 && hsurf && chan_send(&g_ui.ctl, CU_TAB_STARTED, &m, sizeof(m), 1000)) {
                g_tab_proc[a->slot] = proc;
                ok = true;
            } else {
                close_in(g_ui.proc, hs);
                close_in(g_ui.proc, h0);
                close_in(g_ui.proc, h1);
                close_in(g_ui.proc, hsurf);
                revoke(&g_net, a->tab);
                revoke(&g_store, a->tab);
                TerminateProcess(proc, 1);
                WaitForSingleObject(proc, 1000);
                CloseHandle(proc);
                sandbox_tab_ended(a->slot);
            }
        }
    }
    /* The children have their own handles now. */
    chan_close(&ui);
    chan_close(&net);
    if (have_store) chan_close(&store);
    if (surface) CloseHandle(surface);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    return ok;
}

/* Ask a server to save what is pending and exit; wait a while for it. */
static void quit_server(Child *c, DWORD ms) {
    if (!c->proc) return;
    chan_send(&c->ctl, CS_QUIT, NULL, 0, 100);
    WaitForSingleObject(c->proc, ms);
}

/* Results of actions, stepped after the actions that produced them. */
static CoordEvent g_results[COORD_MAX_ACTIONS];
static int g_nresults;

static void perform(const CoordAction *a) {
    switch (a->kind) {
    case CA_START_TAB:
        if (g_nresults < COORD_MAX_ACTIONS) {
            CoordEvent *r = &g_results[g_nresults++];
            memset(r, 0, sizeof(*r));
            r->kind = start_tab(a) ? CE_TAB_STARTED : CE_TAB_START_FAILED;
            r->slot = a->slot;
        }
        break;
    case CA_KILL_TAB:
        if (a->slot >= 0 && a->slot < COORD_MAX_TABS && g_tab_proc[a->slot])
            TerminateProcess(g_tab_proc[a->slot], 0);
        break;
    case CA_TAB_GONE:
        revoke(&g_net, a->tab);
        revoke(&g_store, a->tab);
        break;
    case CA_SEND_UI:
        chan_send(&g_ui.ctl, a->type, a->data, a->len, 1000);
        break;
    case CA_EXIT:
        /* The network process saves its cookie jars to the storage service,
         * then the storage service finishes. */
        quit_server(&g_net, 3000);
        quit_server(&g_store, 2000);
        quit_server(&g_loop, 500);
        /* Their profiles go once the tabs have. */
        for (int i = 0; i < COORD_MAX_TABS; i++)
            if (g_tab_proc[i]) TerminateProcess(g_tab_proc[i], 0);
        for (int i = 0; i < COORD_MAX_TABS; i++)
            if (g_tab_proc[i]) WaitForSingleObject(g_tab_proc[i], 1000);
        sandbox_end();
        CloseHandle(g_job);             /* KILL_ON_JOB_CLOSE ends the rest */
        ExitProcess(0);
    }
}

/* Step the core and perform what it decided, then step the results. */
static void step(const CoordEvent *e) {
    static CoordEvent queue[COORD_MAX_ACTIONS + 1];
    int head = 0, n = 0;
    queue[n++] = *e;
    while (head < n) {
        coord_step(&g_state, &queue[head++], &g_actions);
        g_nresults = 0;
        for (int i = 0; i < g_actions.n; i++) perform(&g_actions.a[i]);
        for (int i = 0; i < g_nresults && n <= COORD_MAX_ACTIONS; i++) queue[n++] = g_results[i];
    }
}

/* The network process reads the replays (CYBER_REPLAY) and writes
 * recordings (CYBER_RECORD) in folders it is given access to: in its
 * AppContainer it has none to the user's files. */
static void grant_replays(void) {
    static const wchar_t *vars[2] = { L"CYBER_REPLAY", L"CYBER_RECORD" };
    for (int i = 0; i < 2; i++) {
        wchar_t dir[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(vars[i], dir, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) continue;
        if (i == 1) CreateDirectoryW(dir, NULL);
        if (!sandbox_grant_folder(dir, SB_NET, i == 1))
            fprintf(stderr, "cb_coord: could not give the network process access to %ls\n", dir);
    }
}

/* browser_data\sites next to the executables, else in %LOCALAPPDATA%, made
 * if need be; the storage service gets it, and access to it alone. */
static bool make_root(void) {
    wchar_t base[MAX_PATH];
    for (int attempt = 0; attempt < 2; attempt++) {
        if (attempt == 0) {
            _snwprintf(base, MAX_PATH, L"%ls", spawn_exe_dir());
        } else {
            DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH - 64);
            if (n == 0 || n >= MAX_PATH - 64) return false;
            wcscat(base, L"\\cyberbrowser\\");
            CreateDirectoryW(base, NULL);
        }
        wchar_t data[MAX_PATH];
        _snwprintf(data, MAX_PATH, L"%lsbrowser_data", base);
        CreateDirectoryW(data, NULL);
        _snwprintf(g_root, MAX_PATH, L"%ls\\sites", data);
        CreateDirectoryW(g_root, NULL);
        DWORD attr = GetFileAttributesW(g_root);
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) &&
            !(attr & FILE_ATTRIBUTE_REPARSE_POINT) && sandbox_grant_folder(g_root, SB_STORE, true))
            return true;
    }
    return false;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--remove-profiles")) {
        printf("removed %d sandbox profiles\n", sandbox_remove_profiles());
        return 0;
    }
    if (!sandbox_init()) fprintf(stderr, "cb_coord: running without the sandbox\n");
    if (!make_root()) {
        fprintf(stderr, "cb_coord: no browser_data folder\n");
        return 1;
    }
    g_job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim;
    memset(&lim, 0, sizeof(lim));
    lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &lim, sizeof(lim));

    /* The servers, connected to each other: the network process keeps its
     * cookie jars in each site's folder. */
    char root_arg[MAX_PATH * 6 + 16], root_utf8[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, g_root, -1, root_utf8, sizeof(root_utf8), NULL, NULL);
    strcpy(root_arg, "--root ");
    hex(root_arg + 7, (const uint8_t *)root_utf8, (uint32_t)strlen(root_utf8));
    grant_replays();
    if (!start_child(&g_store, L"cb_store.exe", "store", root_arg, DETACHED_PROCESS, SB_STORE) ||
        !start_child(&g_net, L"cb_net.exe", "net", NULL, DETACHED_PROCESS, SB_NET) ||
        !start_child(&g_loop, L"cb_loop.exe", "loop", NULL, DETACHED_PROCESS, SB_LOOP)) {
        fprintf(stderr, "cb_coord: could not start cb_store.exe, cb_net.exe or cb_loop.exe\n");
        return 1;
    }
    Chan net_store;
    if (!chan_create(&net_store, 1u << 20) || !grant(&g_store, &net_store, 0, GRANT_NET, NULL) ||
        !grant(&g_net, &net_store, 0, GRANT_STORE, NULL)) {
        DWORD err = GetLastError(), sc = 0, nc = 0;
        GetExitCodeProcess(g_store.proc, &sc);
        GetExitCodeProcess(g_net.proc, &nc);
        fprintf(stderr, "cb_coord: could not connect cb_net.exe to cb_store.exe (%lu; exit codes %lx %lx)\n", err,
                sc, nc);
        return 1;
    }
    chan_close(&net_store);
    /* The relay, for loopback hosts: the network process can't reach them. */
    Chan net_loop;
    if (!chan_create(&net_loop, 1u << 20) || !grant(&g_loop, &net_loop, 0, GRANT_NET, NULL) ||
        !grant(&g_net, &net_loop, 0, GRANT_LOOP, NULL)) {
        fprintf(stderr, "cb_coord: could not connect cb_net.exe to cb_loop.exe\n");
        return 1;
    }
    chan_close(&net_loop);

    /* The UI, with the start URLs (hex). */
    static char urls[16000];
    size_t used = 0;
    for (int i = 1; i < argc; i++) {
        size_t n = strlen(argv[i]);
        if (n > PROTO_URL_MAX || used + 2 * n + 2 >= sizeof(urls)) break;
        if (used) urls[used++] = ' ';
        hex(urls + used, (const uint8_t *)argv[i], (uint32_t)n);
        used += 2 * n;
    }
    const char *script = getenv("CYBER_UI_SCRIPT");
    bool test_ui = script && script[0];
    if (test_ui && sandbox_enabled()) fprintf(stderr, "cb_coord: CYBER_UI_SCRIPT: the UI runs without its sandbox\n");
    if (!start_child(&g_ui, L"cb_ui.exe", "ui", used ? urls : NULL, 0, test_ui ? -1 : SB_UI)) {
        fprintf(stderr, "cb_coord: could not start cb_ui.exe\n");
        return 1;
    }
    coord_init(&g_state);

    static ChanMsg m;
    for (;;) {
        /* Wait for the UI's channel, or a child's exit. */
        HANDLE waits[5 + COORD_MAX_TABS];
        int slot_of[5 + COORD_MAX_TABS];
        int n = 0;
        bool armed = chan_arm(&g_ui.ctl);
        waits[n] = chan_event(&g_ui.ctl); slot_of[n++] = -1;
        waits[n] = g_ui.proc; slot_of[n++] = -2;
        waits[n] = g_net.proc; slot_of[n++] = -3;
        waits[n] = g_store.proc; slot_of[n++] = -3;
        waits[n] = g_loop.proc; slot_of[n++] = -3;
        for (int i = 0; i < COORD_MAX_TABS; i++)
            if (g_tab_proc[i]) { waits[n] = g_tab_proc[i]; slot_of[n++] = i; }
        DWORD w = armed ? WaitForMultipleObjects((DWORD)n, waits, FALSE, INFINITE) : WAIT_OBJECT_0;
        chan_disarm(&g_ui.ctl);
        if (w >= WAIT_OBJECT_0 + (DWORD)n) continue;
        int which = slot_of[w - WAIT_OBJECT_0];
        CoordEvent e;
        memset(&e, 0, sizeof(e));
        if (which == -1) {
            int r;
            while ((r = chan_recv(&g_ui.ctl, &m)) > 0) {
                e.kind = CE_UI_MESSAGE;
                e.type = m.type;
                e.data = m.data;
                e.len = m.len;
                step(&e);
            }
            if (r < 0) { e.kind = CE_UI_EXITED; step(&e); }
        } else if (which == -2) {
            e.kind = CE_UI_EXITED;
            step(&e);
        } else if (which == -3) {
            DWORD nc = 0, sc = 0;
            GetExitCodeProcess(g_net.proc, &nc);
            GetExitCodeProcess(g_store.proc, &sc);
            fprintf(stderr, "cb_coord: the network process or the storage service ended (%lx, %lx)\n", nc, sc);
            e.kind = CE_SERVICE_EXITED;
            step(&e);
        } else {
            DWORD code = 0;
            GetExitCodeProcess(g_tab_proc[which], &code);
            CloseHandle(g_tab_proc[which]);
            g_tab_proc[which] = NULL;
            sandbox_tab_ended(which);
            e.kind = CE_TAB_EXITED;
            e.slot = which;
            e.exit_code = code;
            step(&e);
        }
    }
}

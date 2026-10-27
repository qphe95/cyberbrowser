/* coordinator.c - cb_coord.exe: the coordinator's Win32 shim
 * (SANDBOX_DESIGN.md section 2.1).
 *
 *   cb_coord.exe [url ...]      open a window with a tab for each url
 *
 * It starts the UI process, turns the UI's messages and its children's exits
 * into events for the core (coord_core.c), and performs the core's actions:
 * starting a tab process with its channel to the UI and its surface, ending
 * one, messaging the UI.  Every child runs in a job that ends with the
 * coordinator.  Each child's output goes to logs\<name>.log next to the
 * executables. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "chan.h"
#include "spawn.h"
#include "proto.h"
#include "coord_core.h"

static HANDLE g_job;
static HANDLE g_ui_proc;
static Chan g_ui;                       /* side 0: the coordinator */
static HANDLE g_tab_proc[COORD_MAX_TABS];
static CoordState g_state;
static CoordActions g_actions;          /* large: kept off the stack */

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

static HANDLE dup_to_ui(HANDLE h, DWORD access, bool same) {
    HANDLE out = NULL;
    if (!DuplicateHandle(GetCurrentProcess(), h, g_ui_proc, &out, access, FALSE,
                         same ? DUPLICATE_SAME_ACCESS : 0))
        return NULL;
    return out;
}

/* Undo dup_to_ui for handles the UI never got told about. */
static void close_in_ui(HANDLE h) {
    if (h) DuplicateHandle(g_ui_proc, h, NULL, NULL, 0, FALSE, DUPLICATE_CLOSE_SOURCE);
}

static bool start_tab(const CoordAction *a) {
    Chan c;
    if (!chan_create(&c, 1u << 18)) return false;
    HANDLE surface = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                        (DWORD)(SURFACE_BYTES >> 32), (DWORD)SURFACE_BYTES, NULL);
    char name[32];
    snprintf(name, sizeof(name), "tab-%u", a->tab);
    HANDLE log = open_log(name);
    bool ok = false;
    if (surface) {
        static char args[PROTO_URL_MAX * 2 + 256];
        char url_hex[PROTO_URL_MAX * 2 + 1];
        hex(url_hex, a->data, a->len);
        snprintf(args, sizeof(args), "--chan %llu %llu %llu --surface %llu --size %u %u --tab %u --url %s",
                 spawn_handle_arg(c.section), spawn_handle_arg(c.ev[0]), spawn_handle_arg(c.ev[1]),
                 spawn_handle_arg(surface), a->width, a->height, a->tab, url_hex);
        HANDLE inh[4] = { c.section, c.ev[0], c.ev[1], surface };
        HANDLE proc = spawn_child(L"cb_tab.exe", args, inh, 4,
                                  log != INVALID_HANDLE_VALUE ? log : NULL, g_job, CREATE_NO_WINDOW);
        if (proc) {
            CuTabStarted m;
            memset(&m, 0, sizeof(m));
            m.tab = a->tab;
            HANDLE hs = dup_to_ui(c.section, 0, true), h0 = dup_to_ui(c.ev[0], 0, true),
                   h1 = dup_to_ui(c.ev[1], 0, true), hsurf = dup_to_ui(surface, FILE_MAP_READ, false);
            m.chan_section = spawn_handle_arg(hs);
            m.chan_ev0 = spawn_handle_arg(h0);
            m.chan_ev1 = spawn_handle_arg(h1);
            m.surface = spawn_handle_arg(hsurf);
            m.surface_bytes = SURFACE_BYTES;
            if (hs && h0 && h1 && hsurf && chan_send(&g_ui, CU_TAB_STARTED, &m, sizeof(m), 1000)) {
                g_tab_proc[a->slot] = proc;
                ok = true;
            } else {
                close_in_ui(hs);
                close_in_ui(h0);
                close_in_ui(h1);
                close_in_ui(hsurf);
                TerminateProcess(proc, 1);
                CloseHandle(proc);
            }
        }
    }
    /* The child and the UI have their own handles now. */
    chan_close(&c);
    if (surface) CloseHandle(surface);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    return ok;
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
    case CA_SEND_UI:
        chan_send(&g_ui, a->type, a->data, a->len, 1000);
        break;
    case CA_EXIT:
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

int main(int argc, char **argv) {
    g_job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim;
    memset(&lim, 0, sizeof(lim));
    lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &lim, sizeof(lim));

    if (!chan_create(&g_ui, 1u << 18)) { fprintf(stderr, "cb_coord: no channel\n"); return 1; }
    /* The UI's start URLs, hex-encoded. */
    static char args[16384];
    int used = snprintf(args, sizeof(args), "--chan %llu %llu %llu", spawn_handle_arg(g_ui.section),
                        spawn_handle_arg(g_ui.ev[0]), spawn_handle_arg(g_ui.ev[1]));
    for (int i = 1; i < argc; i++) {
        size_t n = strlen(argv[i]);
        if (n > PROTO_URL_MAX || used + 2 * n + 2 >= sizeof(args)) break;
        args[used++] = ' ';
        hex(args + used, (const uint8_t *)argv[i], (uint32_t)n);
        used += (int)(2 * n);
    }
    HANDLE log = open_log("ui");
    HANDLE inh[3] = { g_ui.section, g_ui.ev[0], g_ui.ev[1] };
    g_ui_proc = spawn_child(L"cb_ui.exe", args, inh, 3, log != INVALID_HANDLE_VALUE ? log : NULL, g_job, 0);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (!g_ui_proc) { fprintf(stderr, "cb_coord: could not start cb_ui.exe\n"); return 1; }
    coord_init(&g_state);

    static ChanMsg m;
    for (;;) {
        /* Wait for the UI's channel, the UI's exit, or a tab's exit. */
        HANDLE waits[2 + COORD_MAX_TABS];
        int slot_of[2 + COORD_MAX_TABS];
        int n = 0;
        bool armed = chan_arm(&g_ui);
        waits[n] = chan_event(&g_ui); slot_of[n++] = -1;
        waits[n] = g_ui_proc; slot_of[n++] = -2;
        for (int i = 0; i < COORD_MAX_TABS; i++)
            if (g_tab_proc[i]) { waits[n] = g_tab_proc[i]; slot_of[n++] = i; }
        DWORD w = armed ? WaitForMultipleObjects((DWORD)n, waits, FALSE, INFINITE) : WAIT_OBJECT_0;
        chan_disarm(&g_ui);
        if (w >= WAIT_OBJECT_0 + (DWORD)n) continue;
        int which = slot_of[w - WAIT_OBJECT_0];
        CoordEvent e;
        memset(&e, 0, sizeof(e));
        if (which == -1) {
            int r;
            while ((r = chan_recv(&g_ui, &m)) > 0) {
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
        } else {
            DWORD code = 0;
            GetExitCodeProcess(g_tab_proc[which], &code);
            CloseHandle(g_tab_proc[which]);
            g_tab_proc[which] = NULL;
            e.kind = CE_TAB_EXITED;
            e.slot = which;
            e.exit_code = code;
            step(&e);
        }
    }
}

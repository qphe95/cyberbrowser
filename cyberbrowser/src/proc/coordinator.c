/* coordinator.c - cb_coord.exe: the coordinator's Win32 shim
 * (SANDBOX_DESIGN.md section 2.1).
 *
 *   cb_coord.exe [url ...]      open a window with a tab for each url
 *
 * It starts the network process (cb_net.exe), the storage service
 * (cb_store.exe), the loopback relay (cb_loop.exe), the GPU process
 * (cb_gpu.exe) and the UI (cb_ui.exe), and connects the network process to
 * the storage service and to the relay, and the UI to the GPU process.  It
 * turns the UI's messages and its children's exits into events for the core
 * (coord_core.c), and performs the core's actions: starting a tab process with
 * its channels (to the UI, to the network process, to the GPU process and,
 * unless its site is opaque, to the storage service, badged with the site the
 * core gave it); revoking them when the tab ends; messaging the UI; starting
 * a site's web-GPU process (cb_webgpu.exe, section 5.7) and giving its tabs
 * channels to it, badged with the site.  Every child runs in a job that ends with the
 * coordinator, and in the sandbox for its kind (sandbox.c).  Each child's
 * output goes to logs\<name>.log next to the executables.
 *
 *   cb_coord.exe --remove-profiles   delete the sandbox's AppContainer profiles
 *
 * CYBER_NO_SANDBOX=1 starts the children unconfined.  CYBER_UI_SCRIPT (tests
 * drive the UI and save screenshots) gives the UI's AppContainer access to the
 * script's folder, where the screenshots go too.
 * CYBER_TAB_EXE=cb_escape.exe with CYBER_TAB_EXE_URL=<url> starts the escape
 * tests in place of the first tab opened on exactly that URL (a test hook:
 * only the coordinator's environment chooses it, never what a tab sends).
 *
 * What it does with handles is decided by coord_plan.c (proved): for each of
 * the core's actions, a list of operations naming each handle, the process
 * it goes into and its badge.  This file performs them (coord_do) with thin
 * wrappers around DuplicateHandle, CloseHandle, the spawn and
 * TerminateProcess, each with a contract the proofs assume. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "chan.h"
#include "spawn.h"
#include "proto.h"
#include "coord_core.h"
#include "coord_plan.h"
#include "sandbox.h"

_Static_assert(PROTO_SITE_MAX == SITE_MAX, "a badge is a site label");

/* A child with a control channel from the coordinator. */
typedef struct Child {
    HANDLE proc;
    Chan ctl;                           /* side 0: the coordinator */
} Child;

static HANDLE g_job;
static Child g_ui, g_net, g_store, g_loop, g_gpu;
static Child g_webgpu[COORD_MAX_WEBGPU];
static HANDLE g_tab_proc[COORD_MAX_TABS];
/* Tabs and web-GPU processes are too many to wait on at once (64 handles):
 * a thread-pool wait on each sets g_exited, and the loop looks which ended. */
static HANDLE g_exited;
static HANDLE g_tab_wait[COORD_MAX_TABS], g_webgpu_wait[COORD_MAX_WEBGPU];
static Chan g_tab_ctl[COORD_MAX_TABS];  /* to each tab, one way (side 0: the coordinator) */
static DWORD g_gpu_starts[3];           /* when the last GPU processes started */
static CoordState g_state;
static CoordActions g_actions;          /* large: kept off the stack */
/* The proved mirror of the tables above (coord_plan.h), changed only by
 * coord_run and the exit scans, as the wrappers change these. */
static CoordTables g_tables;
static CoordPlan g_plan;
static wchar_t g_root[MAX_PATH];        /* browser_data\sites */
/* The escape tests' hostile tab (CYBER_TAB_EXE, CYBER_TAB_EXE_URL), read
 * at the start; cleared once used. */
static char g_tab_exe[64], g_tab_exe_url[PROTO_URL_MAX + 1];

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

/* ---- The Windows wrappers ------------------------------------------------------
 * Trusted, not proved.  Each says, as an ACSL contract over the objects
 * Windows keeps (written informally: Frama-C doesn't read this file), what
 * the proofs of coord_plan.c take it to do.  The tables they name:
 * g_tab_proc / g_tab_ctl (tab slot i), g_webgpu (web-GPU process w), and the
 * servers g_ui, g_net, g_store, g_gpu. */

/*@ // DuplicateHandle
    requires proc is a process handle of those tables, h one of the coordinator's own;
    assigns proc's handle table;
    ensures \result != NULL ==> proc, and no other process, gains a handle to h's object,
        with h's access, whose value is \result;
    ensures \result == NULL ==> no handle table changed;
 */
static HANDLE dup_to(HANDLE proc, HANDLE h, DWORD access, bool same) {
    HANDLE out = NULL;
    if (!DuplicateHandle(GetCurrentProcess(), h, proc, &out, access, FALSE,
                         same ? DUPLICATE_SAME_ACCESS : 0))
        return NULL;
    return out;
}

/* Undo dup_to for handles the other process never got told about. */
/*@ // DuplicateHandle(DUPLICATE_CLOSE_SOURCE)
    requires h == NULL || h is a value dup_to returned for proc;
    assigns proc's handle table;
    ensures proc no longer holds h;
 */
static void close_in(HANDLE proc, HANDLE h) {
    if (h) DuplicateHandle(proc, h, NULL, NULL, 0, FALSE, DUPLICATE_CLOSE_SOURCE);
}

static VOID CALLBACK on_exit(PVOID ctx, BOOLEAN timed_out) {
    (void)ctx;
    (void)timed_out;
    SetEvent(g_exited);
}

static HANDLE watch(HANDLE proc) {
    HANDLE w = NULL;
    if (!RegisterWaitForSingleObject(&w, proc, on_exit, NULL, INFINITE, WT_EXECUTEONLYONCE)) return NULL;
    return w;
}

static void unwatch(HANDLE *w) {
    if (*w) UnregisterWaitEx(*w, INVALID_HANDLE_VALUE);
    *w = NULL;
}

/* Start <exe> with a control channel (and extra arguments). */
static bool start_child_in(Child *c, const wchar_t *exe, const char *log_name, const char *extra, DWORD flags,
                           int kind, int slot);
static bool start_child(Child *c, const wchar_t *exe, const char *log_name, const char *extra, DWORD flags,
                        int kind) {
    return start_child_in(c, exe, log_name, extra, flags, kind, 0);
}

static bool start_child_in(Child *c, const wchar_t *exe, const char *log_name, const char *extra, DWORD flags,
                           int kind, int slot) {
    if (!chan_create(&c->ctl, 1u << 18)) return false;
    static char args[16384];
    snprintf(args, sizeof(args), "--chan %llu %llu %llu%s%s", spawn_handle_arg(c->ctl.section),
             spawn_handle_arg(c->ctl.ev[0]), spawn_handle_arg(c->ctl.ev[1]), extra ? " " : "", extra ? extra : "");
    HANDLE log = open_log(log_name);
    HANDLE inh[3] = { c->ctl.section, c->ctl.ev[0], c->ctl.ev[1] };
    SpawnSandbox sb;
    bool confined = kind < 0 || sandbox_for((SandboxKind)kind, slot, &sb);
    c->proc = confined ? spawn_child(exe, args, inh, 3, log != INVALID_HANDLE_VALUE ? log : NULL, g_job, flags,
                                     kind < 0 ? NULL : &sb)
                       : NULL;
    if (confined && kind >= 0) sandbox_release(&sb);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    return c->proc != NULL;
}

/* Hand a server the server end of channel `ch` for a client, with its badge. */
/*@ // DuplicateHandle, and a CS_GRANT on the server's control channel
    requires server is g_net, g_store, g_gpu, g_loop or one of g_webgpu; ch is open;
    assigns server's handle table, server->ctl;
    ensures \result ==> server holds ch's section and events, and has been sent
        CS_GRANT { client, kind, site } naming them;
    ensures !\result ==> server holds none of them;
 */
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

/*@ // a CS_REVOKE on the server's control channel
    assigns server->ctl;
    ensures server has been sent CS_REVOKE { client }, if its channel had room within 1 s;
 */
static void revoke(Child *server, uint32_t client) {
    CsRevoke v = { client };
    chan_send(&server->ctl, CS_REVOKE, &v, sizeof(v), 1000);
}

/* What a plan makes as it goes (OP_CHAN, OP_SPAWN_TAB), until it ends
 * (plan_end): its channels, by CoordChan, and a tab process not yet a
 * slot's, with its log. */
static Chan g_ch[CH_COUNT];
static bool g_ch_open[CH_COUNT];
static HANDLE g_new_proc, g_new_log = INVALID_HANDLE_VALUE;

/* The children have their own handles now. */
/*@ // CloseHandle, on the coordinator's own handles only
    assigns g_ch, g_ch_open, g_new_log;
    ensures every channel of the plan is closed in the coordinator (not in the children it
        was given to), and the log too;
 */
static void plan_end(void) {
    for (int c = 0; c < CH_COUNT; c++)
        if (g_ch_open[c]) chan_close(&g_ch[c]);
    memset(g_ch_open, 0, sizeof(g_ch_open));
    if (g_new_log != INVALID_HANDLE_VALUE) CloseHandle(g_new_log);
    g_new_log = INVALID_HANDLE_VALUE;
}

/* OP_SPAWN_TAB: tab slot's process, in a tab's sandbox, given exactly the
 * plan's channels (inherited at its start: the handle list). */
/*@ // CreateProcess (spawn_child), in sandbox_for(SB_TAB, slot)
    requires the plan made CH_CTL, CH_UI, CH_NET, CH_DRAW (and, for a site, CH_STORE);
    assigns g_new_proc, g_new_log;
    ensures \result ==> g_new_proc is a new process in slot's sandbox, in g_job, holding
        exactly those channels' handles and no others of the coordinator's;
    ensures !\result ==> no process was started, and slot's sandbox is released;
 */
static bool spawn_tab(const CoordAction *a, int slot) {
    if (!g_ch_open[CH_CTL] || !g_ch_open[CH_UI] || !g_ch_open[CH_NET] || !g_ch_open[CH_DRAW]) return false;
    Chan *ui = &g_ch[CH_UI], *net = &g_ch[CH_NET], *store = &g_ch[CH_STORE], *draw = &g_ch[CH_DRAW],
         *ctl = &g_ch[CH_CTL];
    bool have_store = g_ch_open[CH_STORE];
    char name[32];
    snprintf(name, sizeof(name), "tab-%u", a->tab);
    g_new_log = open_log(name);
    static char args[PROTO_URL_MAX * 2 + 512];
    char url_hex[PROTO_URL_MAX * 2 + 1], store_arg[80] = "";
    hex(url_hex, a->data, a->len);
    if (have_store)
        snprintf(store_arg, sizeof(store_arg), " --store %llu %llu %llu", spawn_handle_arg(store->section),
                 spawn_handle_arg(store->ev[0]), spawn_handle_arg(store->ev[1]));
    snprintf(args, sizeof(args),
             "--chan %llu %llu %llu --ctl %llu %llu %llu --net %llu %llu %llu%s --gpu %llu %llu %llu "
             "--size %u %u --tab %u --url %s",
             spawn_handle_arg(ui->section), spawn_handle_arg(ui->ev[0]), spawn_handle_arg(ui->ev[1]),
             spawn_handle_arg(ctl->section), spawn_handle_arg(ctl->ev[0]), spawn_handle_arg(ctl->ev[1]),
             spawn_handle_arg(net->section), spawn_handle_arg(net->ev[0]), spawn_handle_arg(net->ev[1]),
             store_arg, spawn_handle_arg(draw->section), spawn_handle_arg(draw->ev[0]),
             spawn_handle_arg(draw->ev[1]), a->width, a->height, a->tab, url_hex);
    HANDLE inh[15] = { ui->section, ui->ev[0], ui->ev[1], net->section, net->ev[0], net->ev[1],
                       draw->section, draw->ev[0], draw->ev[1], ctl->section, ctl->ev[0], ctl->ev[1] };
    int ninh = 12;
    if (have_store) {
        inh[ninh++] = store->section;
        inh[ninh++] = store->ev[0];
        inh[ninh++] = store->ev[1];
    }
    /* The escape tests' hostile tab, for the one tab opened on the URL the
     * coordinator was configured with. */
    static wchar_t exe[64];
    bool hostile = g_tab_exe[0] && strlen(g_tab_exe_url) == a->len && !memcmp(g_tab_exe_url, a->data, a->len);
    _snwprintf(exe, 64, L"%hs", hostile ? g_tab_exe : "cb_tab.exe");
    if (hostile) g_tab_exe[0] = 0;
    SpawnSandbox sb;
    HANDLE proc = NULL;
    if (sandbox_for(SB_TAB, slot, &sb)) {
        /* Detached: a console would come with a console host process,
         * which the tab could open. */
        proc = spawn_child(exe, args, inh, ninh, g_new_log != INVALID_HANDLE_VALUE ? g_new_log : NULL, g_job,
                           DETACHED_PROCESS, &sb);
        sandbox_release(&sb);
    } else {
        fprintf(stderr, "cb_coord: no sandbox for tab %u\n", a->tab);
    }
    if (!proc) sandbox_tab_ended(slot);
    g_new_proc = proc;
    return proc != NULL;
}

/* OP_GIVE_UI: the tab's channel to the UI. */
/*@ // DuplicateHandle into g_ui.proc, and CU_TAB_STARTED on its control channel
    requires ch is open;
    assigns g_ui.proc's handle table, g_ui.ctl;
    ensures \result ==> the UI holds ch's handles, and has been told they are tab's;
    ensures !\result ==> the UI holds none of them;
 */
static bool give_ui(uint32_t tab, const Chan *ch) {
    CuTabStarted m;
    memset(&m, 0, sizeof(m));
    m.tab = tab;
    HANDLE hs = dup_to(g_ui.proc, ch->section, 0, true), h0 = dup_to(g_ui.proc, ch->ev[0], 0, true),
           h1 = dup_to(g_ui.proc, ch->ev[1], 0, true);
    m.chan_section = spawn_handle_arg(hs);
    m.chan_ev0 = spawn_handle_arg(h0);
    m.chan_ev1 = spawn_handle_arg(h1);
    if (hs && h0 && h1 && chan_send(&g_ui.ctl, CU_TAB_STARTED, &m, sizeof(m), 1000)) return true;
    close_in(g_ui.proc, hs);
    close_in(g_ui.proc, h0);
    close_in(g_ui.proc, h1);
    return false;
}

/* OP_COMMIT_TAB: the new process and its control channel are slot's. */
/*@ // RegisterWaitForSingleObject; CloseHandle of slot's old control channel
    requires g_new_proc != NULL;
    assigns g_tab_proc[slot], g_tab_wait[slot], g_tab_ctl[slot], g_new_proc, g_ch[CH_CTL], g_ch_open[CH_CTL];
    ensures g_tab_proc[slot] == \old(g_new_proc) && g_tab_ctl[slot] == \old(g_ch[CH_CTL]);
 */
static void commit_tab(int slot) {
    g_tab_proc[slot] = g_new_proc;
    g_new_proc = NULL;
    g_tab_wait[slot] = watch(g_tab_proc[slot]);
    if (g_tab_ctl[slot].sh) chan_close(&g_tab_ctl[slot]);
    g_tab_ctl[slot] = g_ch[CH_CTL];
    memset(&g_ch[CH_CTL], 0, sizeof(g_ch[CH_CTL]));
    g_ch_open[CH_CTL] = false;
}

/* OP_DISCARD_TAB: the new process ends, never having been slot's. */
/*@ // TerminateProcess, WaitForSingleObject, CloseHandle
    assigns g_new_proc;
    ensures \old(g_new_proc) has ended (or 1 s passed), and slot's sandbox is released;
 */
static void discard_tab(int slot) {
    if (!g_new_proc) return;
    TerminateProcess(g_new_proc, 1);
    WaitForSingleObject(g_new_proc, 1000);
    CloseHandle(g_new_proc);
    g_new_proc = NULL;
    sandbox_tab_ended(slot);
}

/* OP_GIVE_TAB: channel ch's handles into slot's process, told with msg
 * (CT_GPU, CT_WEBGPU) on its control channel. */
/*@ // DuplicateHandle into g_tab_proc[slot], and msg on g_tab_ctl[slot]
    requires g_tab_proc[slot] != NULL; ch is open;
    assigns g_tab_proc[slot]'s handle table, g_tab_ctl[slot];
    ensures \result ==> that process, and no other, holds ch's handles, and was told;
    ensures !\result ==> it holds none of them;
 */
static bool give_tab(int slot, const Chan *ch, uint16_t msg) {
    HANDLE p = g_tab_proc[slot];
    if (!p) return false;
    HANDLE s = dup_to(p, ch->section, 0, true), e0 = dup_to(p, ch->ev[0], 0, true), e1 = dup_to(p, ch->ev[1], 0, true);
    CtGpu m = { spawn_handle_arg(s), spawn_handle_arg(e0), spawn_handle_arg(e1) };
    if (s && e0 && e1 && chan_send(&g_tab_ctl[slot], msg, &m, sizeof(m), 1000)) return true;
    close_in(p, s);
    close_in(p, e0);
    close_in(p, e1);
    return false;
}

/* Give the UI a channel to the GPU process. */
static bool connect_ui_gpu(void) {
    Chan ch;
    if (!chan_create(&ch, 1u << 22)) return false;
    bool ok = false;
    if (grant(&g_gpu, &ch, 0, GRANT_UI, NULL)) {
        HANDLE s = dup_to(g_ui.proc, ch.section, 0, true), e0 = dup_to(g_ui.proc, ch.ev[0], 0, true),
               e1 = dup_to(g_ui.proc, ch.ev[1], 0, true);
        CuGpu m = { spawn_handle_arg(s), spawn_handle_arg(e0), spawn_handle_arg(e1) };
        ok = s && e0 && e1 && chan_send(&g_ui.ctl, CU_GPU, &m, sizeof(m), 1000);
        if (!ok) {
            close_in(g_ui.proc, s);
            close_in(g_ui.proc, e0);
            close_in(g_ui.proc, e1);
        }
    }
    chan_close(&ch);
    return ok;
}

/* A GPU process again (CA_RESTART_GPU).  After three starts within a
 * minute the next uses the CPU rasterizer: a driver that keeps crashing it
 * shouldn't take the pages with it. */
static void restart_gpu(void) {
    if (g_gpu.proc) CloseHandle(g_gpu.proc);
    chan_close(&g_gpu.ctl);
    memset(&g_gpu, 0, sizeof(g_gpu));
    DWORD now = GetTickCount();
    static bool on_cpu;
    if (!on_cpu && g_gpu_starts[0] && now - g_gpu_starts[0] < 60000) {
        on_cpu = true;
        fprintf(stderr, "cb_coord: the GPU process keeps ending: drawing on the CPU from now on\n");
        SetEnvironmentVariableW(L"CYBER_GPU", L"cpu");
    }
    memmove(g_gpu_starts, g_gpu_starts + 1, sizeof(g_gpu_starts) - sizeof(DWORD));
    g_gpu_starts[2] = now;
    if (!start_child(&g_gpu, L"cb_gpu.exe", "gpu", NULL, DETACHED_PROCESS, SB_GPU) || !connect_ui_gpu())
        fprintf(stderr, "cb_coord: could not start the GPU process again\n");
}

/* OP_SPAWN_WEBGPU: web-GPU process w, labeled site, with its control channel. */
/*@ // CreateProcess (start_child_in), in sandbox_for(SB_WEBGPU, w); RegisterWaitForSingleObject
    requires 0 <= w < COORD_MAX_WEBGPU;
    assigns g_webgpu[w], g_webgpu_wait[w];
    ensures \result ==> g_webgpu[w].proc is a new process in w's sandbox, told its site is site,
        holding only its control channel's handles;
    ensures !\result ==> g_webgpu[w] is empty, and w's sandbox released;
 */
static bool start_webgpu(int w, const char *site) {
    if (g_webgpu[w].proc) {
        unwatch(&g_webgpu_wait[w]);
        CloseHandle(g_webgpu[w].proc);
        chan_close(&g_webgpu[w].ctl);
    }
    memset(&g_webgpu[w], 0, sizeof(g_webgpu[w]));
    char name[32], extra[32 + 2 * SITE_MAX];
    snprintf(name, sizeof(name), "webgpu-%d", w);
    strcpy(extra, "--site ");
    hex(extra + 7, (const uint8_t *)site, (uint32_t)strnlen(site, SITE_MAX - 1));
    if (!start_child_in(&g_webgpu[w], L"cb_webgpu.exe", name, extra, DETACHED_PROCESS, SB_WEBGPU, w)) {
        chan_close(&g_webgpu[w].ctl);
        memset(&g_webgpu[w], 0, sizeof(g_webgpu[w]));
        sandbox_webgpu_ended(w);
        return false;
    }
    g_webgpu_wait[w] = watch(g_webgpu[w].proc);
    return true;
}

/* Ask a server to save what is pending and exit; wait a while for it. */
static void quit_server(Child *c, DWORD ms) {
    if (!c->proc) return;
    chan_send(&c->ctl, CS_QUIT, NULL, 0, 100);
    WaitForSingleObject(c->proc, ms);
}

/* OP_EXIT. */
/*@ // TerminateProcess, ExitProcess
    ensures \false;     // it doesn't return: every child ends with the job
 */
/* CYBER_TAB_CPU: what a server process cost so far. */
static void log_cpu(const char *name, HANDLE p) {
    FILETIME cr, ex, k, u;
    if (!p || !getenv("CYBER_TAB_CPU") || !GetProcessTimes(p, &cr, &ex, &k, &u)) return;
    ULARGE_INTEGER kk = { .LowPart = k.dwLowDateTime, .HighPart = k.dwHighDateTime },
                   uu = { .LowPart = u.dwLowDateTime, .HighPart = u.dwHighDateTime };
    fprintf(stderr, "[coord] %s: user %.1f ms, kernel %.1f ms\n", name, uu.QuadPart / 1e4, kk.QuadPart / 1e4);
}

static void exit_all(void) {
    log_cpu("ui", g_ui.proc);
    log_cpu("gpu", g_gpu.proc);
    log_cpu("net", g_net.proc);
    log_cpu("store", g_store.proc);
    log_cpu("loop", g_loop.proc);
    /* The network process saves its cookie jars to the storage service,
     * then the storage service finishes. */
    quit_server(&g_net, 3000);
    quit_server(&g_store, 2000);
    quit_server(&g_loop, 500);
    quit_server(&g_gpu, 500);
    /* Their profiles go once the tabs have. */
    for (int i = 0; i < COORD_MAX_TABS; i++)
        if (g_tab_proc[i]) TerminateProcess(g_tab_proc[i], 0);
    for (int w = 0; w < COORD_MAX_WEBGPU; w++)
        if (g_webgpu[w].proc) TerminateProcess(g_webgpu[w].proc, 0);
    for (int w = 0; w < COORD_MAX_WEBGPU; w++)
        if (g_webgpu[w].proc) WaitForSingleObject(g_webgpu[w].proc, 1000);
    for (int i = 0; i < COORD_MAX_TABS; i++)
        if (g_tab_proc[i]) WaitForSingleObject(g_tab_proc[i], 1000);
    sandbox_end();
    if (getenv("CYBER_TAB_CPU")) {
        /* (what all the processes cost, ended ones too) */
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION ai;
        if (QueryInformationJobObject(g_job, JobObjectBasicAccountingInformation, &ai, sizeof(ai), NULL))
            fprintf(stderr, "[coord] all processes: %lu, user %.1f ms, kernel %.1f ms\n", ai.TotalProcesses,
                    ai.TotalUserTime.QuadPart / 1e4, ai.TotalKernelTime.QuadPart / 1e4);
        FILETIME cr, ex, k, u;
        GetProcessTimes(GetCurrentProcess(), &cr, &ex, &k, &u);
        ULARGE_INTEGER kk = { .LowPart = k.dwLowDateTime, .HighPart = k.dwHighDateTime },
                       uu = { .LowPart = u.dwLowDateTime, .HighPart = u.dwHighDateTime };
        fprintf(stderr, "[coord] the coordinator: user %.1f ms, kernel %.1f ms\n", uu.QuadPart / 1e4, kk.QuadPart / 1e4);
        fflush(stderr);
    }
    CloseHandle(g_job);             /* KILL_ON_JOB_CLOSE ends the rest */
    ExitProcess(0);
}

/* The server an operation names. */
static Child *server(int srv, int w) {
    switch (srv) {
    case SRV_NET: return &g_net;
    case SRV_STORE: return &g_store;
    case SRV_GPU: return &g_gpu;
    case SRV_WEBGPU: return w >= 0 && w < COORD_MAX_WEBGPU ? &g_webgpu[w] : NULL;
    default: return NULL;
    }
}

/* One operation of a plan (coord_plan.h): the contract there, and each
 * wrapper's above.  The checks here only guard the shim's arrays: the plan
 * was proved to name entries in range, and in use. */
bool coord_do(const CoordAction *a, const CoordOp *o) {
    int i = o->slot, w = o->w;
    bool tab = i >= 0 && i < COORD_MAX_TABS, web = w >= 0 && w < COORD_MAX_WEBGPU;
    bool chan = o->ch >= 0 && o->ch < CH_COUNT;
    Child *srv = server(o->srv, w);
    switch (o->kind) {
    case OP_CHAN:
        if (!chan) return false;
        if (g_ch_open[o->ch]) chan_close(&g_ch[o->ch]);
        g_ch_open[o->ch] = chan_create(&g_ch[o->ch], o->size);
        return g_ch_open[o->ch];
    case OP_SPAWN_TAB:
        return tab && spawn_tab(a, i);
    case OP_GRANT:
        return chan && g_ch_open[o->ch] && srv && srv->proc && grant(srv, &g_ch[o->ch], a->tab, GRANT_TAB, a->site);
    case OP_GIVE_UI:
        return chan && g_ch_open[o->ch] && give_ui(a->tab, &g_ch[o->ch]);
    case OP_GIVE_TAB:
        return tab && chan && g_ch_open[o->ch] && give_tab(i, &g_ch[o->ch], o->msg);
    case OP_COMMIT_TAB:
        if (!tab || !g_new_proc) return false;
        commit_tab(i);
        return true;
    case OP_DISCARD_TAB:
        if (tab) discard_tab(i);
        return true;
    case OP_REVOKE:
        if (srv && (o->srv != SRV_WEBGPU || srv->proc)) revoke(srv, a->tab);
        return true;
    case OP_KILL_TAB:
        if (tab && g_tab_proc[i]) TerminateProcess(g_tab_proc[i], 0);
        return true;
    case OP_CLOSE_CTL:
        if (tab && g_tab_ctl[i].sh) chan_close(&g_tab_ctl[i]);
        return true;
    case OP_FORGET_WEBGPU:
        return true;
    case OP_SEND_TAB:
        return tab && g_tab_ctl[i].sh && chan_send(&g_tab_ctl[i], o->msg, NULL, 0, o->quick ? 100 : 1000);
    case OP_SPAWN_WEBGPU:
        return web && start_webgpu(w, a->site);
    case OP_KILL_WEBGPU:
        if (web && g_webgpu[w].proc) TerminateProcess(g_webgpu[w].proc, 0);
        return true;
    case OP_SEND_UI:
        return chan_send(&g_ui.ctl, a->type, a->data, a->len, 1000);
    case OP_RESTART_GPU:
        restart_gpu();
        return g_gpu.proc != NULL;
    case OP_EXIT:
        exit_all();
        return true;
    }
    return false;
}

/* Has tab slot i's process ended?  If so its exit code, and the shim lets
 * go of it. */
/*@ // WaitForSingleObject, GetExitCodeProcess, UnregisterWaitEx, CloseHandle
    assigns *code, g_tab_proc[i], g_tab_wait[i];
    ensures \result <==> \old(g_tab_proc[i]) had ended;
    ensures \result ==> g_tab_proc[i] == NULL and slot i's sandbox is released;
 */
bool coord_tab_signaled(int i, uint32_t *code) {
    if (!g_tab_proc[i] || WaitForSingleObject(g_tab_proc[i], 0) != WAIT_OBJECT_0) return false;
    DWORD c = 0;
    GetExitCodeProcess(g_tab_proc[i], &c);
    *code = c;
    if (getenv("CYBER_TAB_CPU")) {
        /* (what a tab process cost: its life and CPU, user and kernel) */
        FILETIME cr, ex, k, u;
        if (GetProcessTimes(g_tab_proc[i], &cr, &ex, &k, &u)) {
            ULARGE_INTEGER a = { .LowPart = cr.dwLowDateTime, .HighPart = cr.dwHighDateTime },
                           b = { .LowPart = ex.dwLowDateTime, .HighPart = ex.dwHighDateTime },
                           kk = { .LowPart = k.dwLowDateTime, .HighPart = k.dwHighDateTime },
                           uu = { .LowPart = u.dwLowDateTime, .HighPart = u.dwHighDateTime };
            fprintf(stderr, "[coord] tab ended: lived %.1f ms, user %.1f ms, kernel %.1f ms\n",
                    (b.QuadPart - a.QuadPart) / 1e4, uu.QuadPart / 1e4, kk.QuadPart / 1e4);
        }
    }
    unwatch(&g_tab_wait[i]);
    CloseHandle(g_tab_proc[i]);
    g_tab_proc[i] = NULL;
    sandbox_tab_ended(i);
    return true;
}

/* The same for web-GPU process w. */
/*@ // WaitForSingleObject, GetExitCodeProcess, UnregisterWaitEx, CloseHandle
    assigns *code, g_webgpu[w], g_webgpu_wait[w];
    ensures \result <==> \old(g_webgpu[w].proc) had ended;
    ensures \result ==> g_webgpu[w] is empty and w's sandbox is released;
 */
bool coord_webgpu_signaled(int w, uint32_t *code) {
    if (!g_webgpu[w].proc || WaitForSingleObject(g_webgpu[w].proc, 0) != WAIT_OBJECT_0) return false;
    DWORD c = 0;
    GetExitCodeProcess(g_webgpu[w].proc, &c);
    *code = c;
    fprintf(stderr, "cb_coord: web-GPU process %d ended (%lx)\n", w, c);
    unwatch(&g_webgpu_wait[w]);
    CloseHandle(g_webgpu[w].proc);
    chan_close(&g_webgpu[w].ctl);
    memset(&g_webgpu[w], 0, sizeof(g_webgpu[w]));
    sandbox_webgpu_ended(w);
    return true;
}

/* Results of actions, stepped after the actions that produced them. */
static CoordEvent g_results[COORD_MAX_ACTIONS];
static int g_nresults;

/* An action: planned (coord_plan), performed (coord_run, through coord_do). */
static void perform(const CoordAction *a) {
    coord_plan(&g_state, &g_tables, a, &g_plan);
    CoordEvent r;
    bool result = coord_run(&g_tables, a, &g_plan, &r);
    plan_end();
    if (result && g_nresults < COORD_MAX_ACTIONS) g_results[g_nresults++] = r;
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

/* A test script (CYBER_UI_SCRIPT) and its screenshots live in one folder,
 * which the UI's AppContainer is given. */
static void script_folder(void) {
    wchar_t dir[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"CYBER_UI_SCRIPT", dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    for (wchar_t *p = dir; *p; p++)
        if (*p == L'/') *p = L'\\';
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (!slash) return;
    *slash = 0;
    if (!sandbox_grant_folder(dir, SB_UI, true))
        fprintf(stderr, "cb_coord: could not give the UI access to %ls\n", dir);
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

/* The web-GPU process's libraries (third_party/prebuilt, copied by the
 * build into webgpu\ next to the executables): readable by AppContainers.
 * It loads them by full path from there. */
static void webgpu_folder(void) {
    wchar_t dir[MAX_PATH];
    _snwprintf(dir, MAX_PATH, L"%lswebgpu", spawn_exe_dir());
    if (GetFileAttributesW(dir) == INVALID_FILE_ATTRIBUTES) return;
    if (!sandbox_grant_packages_read(dir)) fprintf(stderr, "cb_coord: could not open %ls to the web-GPU process\n", dir);
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
        /* Microsoft's _snwprintf leaves no NUL when it cuts short: a base too
         * long for the root is passed over. */
        wchar_t data[MAX_PATH];
        int n = _snwprintf(g_root, MAX_PATH, L"%lsbrowser_data\\sites", base);
        if (n < 0 || n >= MAX_PATH) {
            g_root[0] = 0;
            continue;
        }
        _snwprintf(data, MAX_PATH, L"%lsbrowser_data", base);
        CreateDirectoryW(data, NULL);
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
    const char *alt = getenv("CYBER_TAB_EXE"), *alt_url = getenv("CYBER_TAB_EXE_URL");
    if (alt && alt[0] && alt_url && alt_url[0] && strlen(alt) < sizeof(g_tab_exe) &&
        strlen(alt_url) < sizeof(g_tab_exe_url)) {
        strcpy(g_tab_exe, alt);
        strcpy(g_tab_exe_url, alt_url);
    }
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
    /* The GPU process, then the UI it presents into. */
    if (!start_child(&g_gpu, L"cb_gpu.exe", "gpu", NULL, DETACHED_PROCESS, SB_GPU)) {
        fprintf(stderr, "cb_coord: could not start cb_gpu.exe\n");
        return 1;
    }
    script_folder();
    webgpu_folder();
    g_exited = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!start_child(&g_ui, L"cb_ui.exe", "ui", used ? urls : NULL, 0, SB_UI) || !connect_ui_gpu()) {
        fprintf(stderr, "cb_coord: could not start cb_ui.exe\n");
        return 1;
    }
    coord_init(&g_state);
    coord_tables_init(&g_tables, g_gpu.proc != NULL);

    static ChanMsg m;
    for (;;) {
        /* Wait for the UI's channel, or a child's exit. */
        HANDLE waits[8];
        int slot_of[8];
        int n = 0;
        bool armed = chan_arm(&g_ui.ctl);
        waits[n] = chan_event(&g_ui.ctl); slot_of[n++] = -1;
        waits[n] = g_ui.proc; slot_of[n++] = -2;
        waits[n] = g_net.proc; slot_of[n++] = -3;
        waits[n] = g_store.proc; slot_of[n++] = -3;
        waits[n] = g_loop.proc; slot_of[n++] = -3;
        if (g_gpu.proc) { waits[n] = g_gpu.proc; slot_of[n++] = -4; }
        waits[n] = g_exited; slot_of[n++] = -5;
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
        } else if (which == -4) {
            DWORD gc = 0;
            GetExitCodeProcess(g_gpu.proc, &gc);
            fprintf(stderr, "cb_coord: the GPU process ended (%lx): starting another\n", gc);
            e.kind = CE_GPU_EXITED;
            step(&e);
        } else if (which == -3) {
            DWORD nc = 0, sc = 0;
            GetExitCodeProcess(g_net.proc, &nc);
            GetExitCodeProcess(g_store.proc, &sc);
            fprintf(stderr, "cb_coord: the network process or the storage service ended (%lx, %lx)\n", nc, sc);
            e.kind = CE_SERVICE_EXITED;
            step(&e);
        } else {
            /* Which tabs and web-GPU processes ended: each found in its
             * table entry, and its event names that entry (coord_plan.c).
             * A web-GPU process's tabs hear at once that their canvases
             * are lost, rather than waiting on a channel nobody answers. */
            for (int i = 0; (i = coord_next_tab_exit(&g_tables, i, &e)) >= 0; i++) step(&e);
            static CoordAction none;            /* (the notices name no action) */
            for (int w = 0; (w = coord_next_webgpu_exit(&g_state, &g_tables, w, &g_plan, &e)) >= 0; w++) {
                CoordEvent unused;
                coord_run(&g_tables, &none, &g_plan, &unused);
                step(&e);
            }
        }
    }
}

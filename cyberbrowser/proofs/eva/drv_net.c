/* drv_net.c - Eva's driver for cb_net.exe (net_main.c; loop_client.c is
 * drv_net_loop.c's).  The HTTP, TLS, cookie and URL code are lib_net.c's
 * models.
 *
 * Eva follows one thread.  The process has a main thread, a reader thread
 * per tab, 16 workers and the relay client's reader; CreateThread runs
 * nothing (include/windows.h).  So the driver runs, each in turn:
 *   0. the main thread: main with any command line;
 *   1. a tab's threads: what main does before its loop, any grants (as the
 *      main thread makes them), then the reader of one of the clients they
 *      made, until it ends, then a worker, which takes the requests that
 *      reader queued;
 *   2. the relay client (drv_net_loop.c).
 * What this leaves out: interleavings (a revoke on the main thread while a
 * reader or worker uses the client, which the reference count is for), and
 * a worker running before its reader has ended.  See M9_NOTES_eva.md. */
#define main net_main
#include "net_main.c"
#undef main
#include "drv.h"

void fc_loop_client(void);

int main(void) {
    static char *argv[FC_MAX_ARGS];
    static const char *const fixed[] = { "--chan", NULL, NULL, NULL };
    int argc = fc_command_line(argv, fixed, 4, 1, 32);
    switch (Frama_C_interval(0, 2)) {
    case 0:
        return net_main(argc, argv);
    case 1: {
        tls_set_space_policy(space_of);
        InitializeCriticalSection(&g_jobs_lock);
        InitializeConditionVariable(&g_jobs_cv);
        while (Frama_C_nondet(0, 1)) {
            static CsGrant g;
            Frama_C_make_unknown((char *)&g, sizeof(g));
            grant(&g);
        }
        Client *c = g_clients[Frama_C_interval(0, MAX_CLIENTS - 1)];
        if (!c) return 0;
        reader(c);
        worker(NULL);
        return 0;
    }
    default:
        fc_loop_client();
        return 0;
    }
}

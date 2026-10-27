/* chan_bench.c - libchan's costs between two processes, and a check that
 * every message arrives intact and in order.
 *
 *   chan_bench.exe            run all measurements (starts its own child)
 *
 * Measured: a blocking round trip (each side sleeps on its event between
 * messages), a spinning round trip (both poll, no system calls), a stream
 * of small messages, and a stream of 16 KB messages. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "../ipc/chan.h"
#include "spawn.h"

enum { M_PING = 1, M_PONG, M_SMALL, M_BIG, M_DONE, M_SPIN_PING, M_SPIN_PONG, M_QUIT };

static double now_us(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1e6 / (double)f.QuadPart;
}

/* Wait for the next message (sleeping on the event when idle). */
static int next(Chan *c, ChanMsg *m) {
    for (;;) {
        int r = chan_recv(c, m);
        if (r) return r;
        chan_wait(c, INFINITE);
    }
}

static uint32_t sum(const uint8_t *p, uint32_t n) {
    uint32_t s = 2166136261u;
    for (uint32_t i = 0; i < n; i++) s = (s ^ p[i]) * 16777619u;
    return s;
}

static int child(HANDLE sec, HANDLE e0, HANDLE e1) {
    Chan c;
    if (!chan_attach(&c, sec, e0, e1, 1)) { fprintf(stderr, "child: attach failed\n"); return 1; }
    static ChanMsg m;
    uint64_t small = 0, big = 0, bad = 0, expect = 0;
    for (;;) {
        int r = next(&c, &m);
        if (r < 0) { fprintf(stderr, "child: channel broken\n"); return 1; }
        switch (m.type) {
        case M_PING:
            chan_send(&c, M_PONG, m.data, m.len, INFINITE);
            break;
        case M_SPIN_PING:
            chan_send(&c, M_SPIN_PONG, m.data, m.len, INFINITE);
            /* spin for the next one: no system call */
            while ((r = chan_recv(&c, &m)) == 0) YieldProcessor();
            while (r > 0 && m.type == M_SPIN_PING) {
                chan_send(&c, M_SPIN_PONG, m.data, m.len, INFINITE);
                while ((r = chan_recv(&c, &m)) == 0) YieldProcessor();
            }
            if (r > 0 && m.type == M_QUIT) return 0;
            if (r > 0 && m.type == M_DONE) chan_send(&c, M_DONE, NULL, 0, INFINITE);
            break;
        case M_SMALL: {
            uint64_t seq;
            memcpy(&seq, m.data, 8);
            if (seq != expect++ || m.len != 32) bad++;
            small++;
            break;
        }
        case M_BIG: {
            uint32_t s;
            memcpy(&s, m.data, 4);
            if (m.len != CHAN_MSG_MAX || s != sum(m.data + 4, m.len - 4)) bad++;
            big++;
            break;
        }
        case M_DONE: {
            uint64_t out[3] = { small, big, bad };
            chan_send(&c, M_DONE, out, sizeof(out), INFINITE);
            small = big = bad = expect = 0;
            break;
        }
        case M_QUIT:
            return 0;
        }
    }
}

int main(int argc, char **argv) {
    if (argc == 5 && strcmp(argv[1], "--child") == 0)
        return child(spawn_handle_from_arg(argv[2]), spawn_handle_from_arg(argv[3]),
                     spawn_handle_from_arg(argv[4]));

    Chan c;
    if (!chan_create(&c, 1u << 20)) { fprintf(stderr, "chan_create failed\n"); return 1; }
    char args[128];
    snprintf(args, sizeof(args), "--child %llu %llu %llu", spawn_handle_arg(c.section),
             spawn_handle_arg(c.ev[0]), spawn_handle_arg(c.ev[1]));
    HANDLE inh[3] = { c.section, c.ev[0], c.ev[1] };
    HANDLE proc = spawn_child(L"chan_bench.exe", args, inh, 3, NULL, NULL, 0);
    if (!proc) return 1;

    static ChanMsg m;
    uint8_t payload[64] = { 0 };

    /* Blocking round trips. */
    const int NB = 20000;
    for (int i = 0; i < 1000; i++) { chan_send(&c, M_PING, payload, 16, INFINITE); next(&c, &m); }
    double t0 = now_us();
    for (int i = 0; i < NB; i++) {
        memcpy(payload, &i, 4);
        chan_send(&c, M_PING, payload, 16, INFINITE);
        if (next(&c, &m) <= 0 || m.type != M_PONG || memcmp(m.data, &i, 4)) { fprintf(stderr, "bad pong\n"); return 1; }
    }
    double block_rtt = (now_us() - t0) / NB;

    /* Spinning round trips. */
    const int NS = 200000;
    t0 = now_us();
    for (int i = 0; i < NS; i++) {
        memcpy(payload, &i, 4);
        chan_send(&c, M_SPIN_PING, payload, 16, INFINITE);
        int r;
        while ((r = chan_recv(&c, &m)) == 0) YieldProcessor();
        if (r < 0 || m.type != M_SPIN_PONG || memcmp(m.data, &i, 4)) { fprintf(stderr, "bad spin pong\n"); return 1; }
    }
    double spin_rtt = (now_us() - t0) / NS;
    chan_send(&c, M_DONE, NULL, 0, INFINITE);   /* ends the child's spin loop */
    next(&c, &m);

    /* A stream of small messages. */
    const uint64_t NSMALL = 2000000;
    t0 = now_us();
    for (uint64_t i = 0; i < NSMALL; i++) {
        memcpy(payload, &i, 8);
        chan_send(&c, M_SMALL, payload, 32, INFINITE);
    }
    chan_send(&c, M_DONE, NULL, 0, INFINITE);
    next(&c, &m);
    double small_s = (now_us() - t0) / 1e6;
    uint64_t res[3];
    memcpy(res, m.data, sizeof(res));
    uint64_t bad = res[2];
    if (res[0] != NSMALL) bad++;

    /* A stream of 16 KB messages. */
    static uint8_t big[CHAN_MSG_MAX];
    for (int i = 4; i < CHAN_MSG_MAX; i++) big[i] = (uint8_t)(i * 31);
    const int NBIG = 100000;
    t0 = now_us();
    for (int i = 0; i < NBIG; i++) {
        big[4 + (i % 1000)] ^= 1;
        uint32_t s = sum(big + 4, CHAN_MSG_MAX - 4);
        memcpy(big, &s, 4);
        chan_send(&c, M_BIG, big, CHAN_MSG_MAX, INFINITE);
    }
    chan_send(&c, M_DONE, NULL, 0, INFINITE);
    next(&c, &m);
    double big_s = (now_us() - t0) / 1e6;
    memcpy(res, m.data, sizeof(res));
    bad += res[2];
    if (res[1] != (uint64_t)NBIG) bad++;

    chan_send(&c, M_QUIT, NULL, 0, INFINITE);
    WaitForSingleObject(proc, 5000);

    printf("blocking round trip:  %.2f us (%d round trips)\n", block_rtt, NB);
    printf("spinning round trip:  %.3f us (%d round trips)\n", spin_rtt, NS);
    printf("small messages (32 B): %.1f M/s (%llu messages)\n", NSMALL / small_s / 1e6, (unsigned long long)NSMALL);
    printf("16 KB messages:        %.0f MB/s (%.0f K/s; checksums included)\n",
           NBIG * (double)CHAN_MSG_MAX / big_s / 1048576.0, NBIG / big_s / 1e3);
    printf("errors: %llu\n", (unsigned long long)bad);
    return bad ? 1 : 0;
}

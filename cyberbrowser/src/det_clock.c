/* det_clock.c - the page clock and CYBER_DET_TIME (det_clock.h). */
#include "det_clock.h"
#include "cyber_env.h"
#include "platform.h"

#include <stdint.h>
#include <stdlib.h>
#include <sys/time.h>

/* Where the deterministic clock starts: 2026-01-01T00:00:00Z, and 1000 s on
 * the monotonic clock (nothing reads it as 0). */
#define DET_EPOCH_MS 1767225600000.0
#define DET_MONO_ORIGIN_MS 1000000.0
/* Page time per call made (det_clock.h): about what a call and the work
 * around it take on the replays, so the page sees time pass at roughly the
 * rate it does in real time. */
#define DET_MS_PER_CALL 0.0003
/* Page time per read of the clock, so a loop waiting for time to pass
 * ends even if it makes no calls. */
#define DET_MS_PER_READ 0.001

extern int64_t JS_time_offset_ms;    /* quickjs.c: the virtual-time offset */

int g_det_clock_on;
uint64_t g_det_calls;
static int g_on = -1;
static double g_det_read_ms;        /* advanced by the JS thread's reads */
static __thread int t_js_thread;

/* CYBER_VIRTUAL_TIME (det_clock.h): 1 or 0, read once. */
static int g_virtual = -1;

bool page_clock_virtual(void)
{
    if (g_virtual < 0) {
        const char *e = getenv("CYBER_VIRTUAL_TIME");
        g_virtual = e && atoi(e) ? 1 : 0;
    }
    return g_virtual;
}

unsigned long long page_clock_wait_begin(void)
{
    return page_clock_virtual() ? platform_get_time_ms() : 0;
}

void page_clock_wait_end(unsigned long long begun)
{
    if (page_clock_virtual()) JS_time_offset_ms += (int64_t)(platform_get_time_ms() - begun);
}

bool det_clock_on(void)
{
    if (g_on < 0) {
        g_on = cyber_getenv("CYBER_DET_TIME") ? 1 : 0;
        g_det_clock_on = g_on;
    }
    return g_on;
}

void det_clock_set_js_thread(void)
{
    t_js_thread = 1;
    det_clock_on();
}

static double det_read(void)
{
    if (t_js_thread)
        g_det_read_ms += DET_MS_PER_READ;
    return g_det_read_ms + (double)g_det_calls * DET_MS_PER_CALL;
}

/* (only a read's worth: a loop waiting for time to pass ends, and two reads
 * differ, at the 5 us performance.now is coarsened to) */
static double virtual_read(void)
{
    if (t_js_thread)
        g_det_read_ms += 0.006;
    return g_det_read_ms;
}

double page_clock_ms(void)
{
    if (det_clock_on())
        return DET_MONO_ORIGIN_MS + det_read();
    if (page_clock_virtual())
        return DET_MONO_ORIGIN_MS + virtual_read();
    return (double)platform_get_time_ms();
}

double page_clock_now_ms(void)
{
    if (det_clock_on() || page_clock_virtual())
        return page_clock_ms() + (double)JS_time_offset_ms;
    return (double)platform_get_time_ms() + (double)JS_time_offset_ms;
}

double page_clock_epoch_ms(void)
{
    if (det_clock_on())
        return DET_EPOCH_MS + det_read();
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (page_clock_virtual()) {
        static double start;        /* (today, as far as anyone can tell) */
        if (!start) start = (double)tv.tv_sec * 1000.0;
        return start + virtual_read();
    }
    return (double)tv.tv_sec * 1000.0 + (double)(tv.tv_usec / 1000);
}

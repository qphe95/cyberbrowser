/*
 * det_clock.h - the clock the page sees (timers, Date, performance.now,
 * media playback, observers), and CYBER_DET_TIME=1, which makes it
 * deterministic for benchmarking (JS_PERF_DESIGN.md section 4).
 *
 * Normally the page clock is real time (and the event loop's virtual-time
 * offset on top, JS_time_offset_ms).  Real time passing while the page
 * runs changes what it does: a slower run sees more interval callbacks and
 * smaller scheduler slices, so it does more work, and timing one engine
 * against another measures different work.  With CYBER_DET_TIME=1 the page
 * clock starts at a fixed point and advances by the work the page does: a
 * fixed amount per call (bytecode or native function; a count that does not
 * depend on which tier runs the code), and 1 us each time the JS thread
 * reads the clock (so code that waits for time to pass still finishes),
 * plus the virtual-time offset.  Time still passes while scripts run, as
 * the page expects, but the same at every run.  Math.random gets a fixed
 * seed too.
 */
#ifndef DET_CLOCK_H
#define DET_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

/* CYBER_DET_TIME: count a call (the engine does, at every call). */
extern int g_det_clock_on;
extern uint64_t g_det_calls;
#define DET_CLOCK_CALL() do { if (__builtin_expect(g_det_clock_on, 0)) g_det_calls++; } while (0)

#ifdef __cplusplus
extern "C" {
#endif

/* CYBER_DET_TIME is set (read once). */
bool det_clock_on(void);
/* The thread calling this is the one whose reads advance the clock (the
 * JS thread); call it once at startup. */
void det_clock_set_js_thread(void);
/* The page clock in ms: monotonic, from an arbitrary origin (real time
 * from platform_get_time_ms, or the deterministic clock). */
double page_clock_ms(void);
/* The page clock as UTC ms since 1970, for Date (without the virtual-time
 * offset, which Date adds). */
double page_clock_epoch_ms(void);
/* What the page's machinery times itself by (observer and layout
 * throttles, media playback): real time, or under CYBER_DET_TIME the
 * deterministic clock with the virtual-time offset, so that it advances as
 * the page's timers see time advance. */
double page_clock_now_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* DET_CLOCK_H */

#ifndef PAGE_ENGINE_H
#define PAGE_ENGINE_H

/* The page engine: loads a page into the JS realm, runs its event loop, and
 * lays it out into frames (a layout and its display list).
 *
 * The engine's state is per process: a process holds one page
 * (SANDBOX_DESIGN.md), so these functions act on that page.  The harness in
 * cyberbrowser_main.c drives them to load a page, wait until it settles and
 * save a screenshot; a tab process drives the same calls from its own loop. */

#include <stdbool.h>
#include "quickjs.h"
#include "html_dom.h"
#include "css_layout.h"
#include "display_list.h"
#include "image_cache.h"

typedef struct PageHooks {
    /* A milestone of the load ("scripts done"); may be NULL. */
    void (*mark)(const char *what);
    /* The page's scripts have run and its load events fired, before its
     * images are requested and it settles; may be NULL. */
    void (*loaded)(void);
} PageHooks;

/* The JS runtime and context and the browser APIs (hooks may be NULL). */
bool page_engine_init(const PageHooks *hooks);
/* Index the system fonts: before any script runs, since layout queries
 * from scripts measure text. */
void page_engine_load_fonts(void);
JSContextHandle page_engine_context(void);
/* At the end of a successful run: stop the GC thread and leave the heap to
 * the OS (CYBER_FULL_CLEANUP=1: free everything, as page_engine_cleanup). */
void page_engine_finish(void);
void page_engine_cleanup(void);

/* One event-loop drain: due timers, scheduler tasks and promise jobs, until
 * nothing is due; with `wait`, also wait (bounded) for timers due soon and
 * for fetches in flight.  True if anything ran. */
bool page_pump(bool wait);

/* Make `url` the page's URL (window.location, the base of relative URLs),
 * create the image cache and enable script-inserted scripts. */
void page_prepare(const char *url);
/* Fetch the page, parse it, run its scripts, dispatch DOMContentLoaded and
 * load, and upgrade its custom elements.  False if it couldn't be fetched. */
bool page_load(void);
/* Free the page's document, frame and image cache. */
void page_close(void);

ImageCache *page_image_cache(void);
/* The document as a string of HTML (malloc'd), or NULL. */
char *page_serialize(void);

/* A laid-out document and its display list. */
typedef struct PageFrame {
    bool valid;
    LayoutContext layout;
    DisplayList dl;
} PageFrame;

/* Lay out `doc` at width x height and build its display list; `doc` must
 * outlive the frame. */
void page_frame_build(PageFrame *f, HtmlDocument *doc, ImageCache *images, int width, int height);
void page_frame_free(PageFrame *f);

/* The frame of the page's current document (rebuilt by page_turn). */
PageFrame *page_frame(void);

/* One turn of the loaded page: run due work (unless !run_timers; with
 * `wait`, as page_pump), deliver finished images, and if the DOM changed,
 * rebuild the document and its frame at width x height. */
typedef struct PageTurn {
    bool had_timers;
    bool had_images;
    bool new_frame;     /* page_frame() was rebuilt */
} PageTurn;
void page_turn(bool run_timers, bool wait, int width, int height, PageTurn *out);
/* Lay the page out again at the next turn (the viewport changed). */
void page_request_layout(void);
/* The element under (x, y) of the current frame, in document coordinates
 * (a JS value), or JS_NULL. */
GCValue page_element_at(double x, double y);
/* The title of the current document (its first <title>), or NULL. */
const char *page_title(void);
/* A DOM change is waiting to be laid out. */
bool page_needs_layout(void);
/* Nothing ran in the last turn: wait for a fetch or an image to make
 * progress, or (with virtual time, nothing in flight) move the page clock
 * to the next timer or to the end of the quiet window that started at page
 * time `quiet_from` (quiet_ms 0: 100 ms ahead), whichever is first.  The
 * clock is read only in that last case: each read moves CYBER_DET_TIME's. */
void page_wait_for_work(unsigned long long quiet_from, unsigned long long quiet_ms);

#endif

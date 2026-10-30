#ifndef UI_CORE_H
#define UI_CORE_H

/* The UI's reading of what a tab sends it (SANDBOX_DESIGN.md sections 2.2 and
 * 8.7.3), and its rule for tabs a tab opens.  Pure C, no library calls,
 * bounded loops: proved with Frama-C/WP (tools/prove.sh ui) free of undefined
 * behavior for any bytes (P1), bounded (P7), accepting only messages of
 * these grammars, with every field it passes on in range:
 *
 *   TU_FRAME        TuFrame (six uint32), 1 <= width <= SURFACE_MAX_W,
 *                   1 <= height <= SURFACE_MAX_H
 *   TU_TITLE        0 .. PROTO_TITLE_MAX bytes of text (shown up to a NUL)
 *   TU_STATE        TuState (one uint32); TAB_LOADED and TAB_LOAD_FAILED
 *                   are themselves, anything else is TAB_LOADING
 *   TU_WANT_WEBGPU  nothing
 *   TU_NAVIGATE     TuNavigate (new_tab: one uint32, any nonzero is 1), then
 *                   a URL of 1 .. PROTO_URL_MAX bytes, none of them NUL
 *
 * The URL goes to the coordinator, which starts only http(s) and
 * about:blank (coord_url_ok, proved there).
 *
 * A tab may open a new tab (TU_NAVIGATE with new_tab) only in answer to a
 * click the UI sent it, once per click, within UI_GESTURE_MS of it: a page
 * can't open tabs by itself, and so can't use up the browser's 60 slots
 * (section 8.7.2). */

#include <stdbool.h>
#include <stdint.h>
#include "proto.h"

#define UI_GESTURE_MS 5000

typedef struct UiTabMsg {
    TuFrame frame;                  /* TU_FRAME */
    uint32_t state;                 /* TU_STATE: TAB_LOADING, TAB_LOADED or TAB_LOAD_FAILED */
    uint32_t new_tab;               /* TU_NAVIGATE: 0 or 1 */
    uint32_t text_len;              /* TU_TITLE, TU_NAVIGATE: of text */
    char text[PROTO_URL_MAX + 1];   /* the title or the URL, NUL-terminated after text_len */
} UiTabMsg;

/* The last click the UI sent a tab, not yet used to open a tab. */
typedef struct UiGesture {
    uint32_t tick;                  /* when (GetTickCount) */
    uint32_t pending;               /* 1: not used yet */
} UiGesture;

/*@ logic integer u32le{L}(uint8_t *p) = p[0] + p[1] * 0x100 + p[2] * 0x10000 + p[3] * 0x1000000;
 */

/* Decode a message of type from a tab (len bytes at data) into *out: its
 * type if it is of the grammar above, else 0 (the UI ignores it). */
/*@ requires \valid_read(data + (0 .. len - 1)) && \valid(out) && \separated(out, data + (0 .. len - 1));
    assigns *out;
    ensures \result == 0 || \result == type;
    ensures \result == TU_FRAME ==> type == TU_FRAME && len == sizeof(TuFrame) &&
        1 <= out->frame.width <= SURFACE_MAX_W && 1 <= out->frame.height <= SURFACE_MAX_H &&
        out->frame.frame == u32le(data) && out->frame.width == u32le(data + 8) &&
        out->frame.height == u32le(data + 12) && out->frame.scroll_y == u32le(data + 16) &&
        out->frame.content_height == u32le(data + 20);
    ensures \result == TU_TITLE ==> type == TU_TITLE && len <= PROTO_TITLE_MAX && out->text_len == len &&
        (\forall integer i; 0 <= i < len ==> out->text[i] == (char)data[i]) && out->text[len] == 0;
    ensures \result == TU_STATE ==> type == TU_STATE && len == sizeof(TuState) &&
        (out->state == TAB_LOADING || out->state == TAB_LOADED || out->state == TAB_LOAD_FAILED) &&
        ((u32le(data) == TAB_LOADED || u32le(data) == TAB_LOAD_FAILED) ==> out->state == u32le(data));
    ensures \result == TU_WANT_WEBGPU ==> type == TU_WANT_WEBGPU && len == 0;
    ensures \result == TU_NAVIGATE ==> type == TU_NAVIGATE &&
        sizeof(TuNavigate) < len <= sizeof(TuNavigate) + PROTO_URL_MAX &&
        out->new_tab == (u32le(data) != 0 ? 1 : 0) && out->text_len == len - sizeof(TuNavigate) &&
        (\forall integer i; 0 <= i < out->text_len ==>
            out->text[i] == (char)data[sizeof(TuNavigate) + i] && out->text[i] != 0) &&
        out->text[out->text_len] == 0;
 */
uint16_t ui_tab_message(uint16_t type, const uint8_t *data, uint32_t len, UiTabMsg *out);

/* The UI sent the tab a click at now. */
/*@ requires \valid(g);
    assigns *g;
    ensures g->pending == 1 && g->tick == now;
 */
void ui_gesture(UiGesture *g, uint32_t now);

/* May the tab open a new tab now: a click not used yet, at most
 * UI_GESTURE_MS ago (ticks wrap: mod 2^32).  Uses it if so. */
/*@ requires \valid(g);
    assigns *g;
    ensures \result ==> \old(g->pending) != 0 &&
        (now >= \old(g->tick) ? now - \old(g->tick) : now + 0x100000000 - \old(g->tick)) <= UI_GESTURE_MS;
    ensures \result ==> g->pending == 0;
    ensures !\result ==> g->pending == \old(g->pending) && g->tick == \old(g->tick);
 */
bool ui_may_open_tab(UiGesture *g, uint32_t now);

#endif

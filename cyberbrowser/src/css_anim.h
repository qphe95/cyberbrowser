/*
 * css_anim.h - CSS Transitions 1 and CSS Animations 1: the running
 * transitions and animations of every element, in one store of flat
 * arrays (css_anim.c).
 *
 * Layout (css_layout.c layout_animate) tells the store each animated box's
 * style as the cascade gave it: a transition starts where a property it
 * covers changed since the last layout, an animation where animation-name
 * names one that was not running.  Then it writes what each property is at
 * this time into the box's style.  The browser's event loop (anim_api.c)
 * fires their events and asks for a frame while any runs.
 *
 * Rows are keyed by element (NodeId, generation checked) and pseudo-element
 * (0 none, 1 ::before, 2 ::after), so moving an element (moveBefore) keeps
 * them; an element that leaves its document loses them.
 */
#ifndef CSS_ANIM_H
#define CSS_ANIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "dom_store.h"

/* The properties that animate (the others keep their cascaded value). */
enum { ANIM_LEFT, ANIM_TOP, ANIM_RIGHT, ANIM_BOTTOM, ANIM_OPACITY, ANIM_TRANSFORM, ANIM_WIDTH, ANIM_HEIGHT, ANIM_COLOR, ANIM_BGCOLOR,
       ANIM_PADDING_TOP, ANIM_PADDING_RIGHT, ANIM_PADDING_BOTTOM, ANIM_PADDING_LEFT,
       ANIM_MARGIN_TOP, ANIM_MARGIN_RIGHT, ANIM_MARGIN_BOTTOM, ANIM_MARGIN_LEFT,
       ANIM_BORDER_TOP, ANIM_BORDER_RIGHT, ANIM_BORDER_BOTTOM, ANIM_BORDER_LEFT,
       ANIM_FONT_SIZE, ANIM_LETTER_SPACING, ANIM_WORD_SPACING, ANIM_FONT_WEIGHT, ANIM_NPROP };

/* A value: px (or a number: opacity; a translation: x, y).  ok 0: one
 * that does not interpolate (auto, a percentage): it changes at once. */
typedef struct AnimVal {
    float x, y;
    uint8_t ok;
    float z, w;     /* (a color: x, y, z, w are r, g, b, a) */
} AnimVal;

/* ---- The specification: the transition-* and animation-* longhands as
 * one string per style (fields separated by 0x1F, "" the initial value) ---- */
enum {
    ANIM_F_TPROP, ANIM_F_TDUR, ANIM_F_TEASE, ANIM_F_TDELAY,
    ANIM_F_NAME, ANIM_F_DUR, ANIM_F_EASE, ANIM_F_DELAY, ANIM_F_ITER, ANIM_F_DIR, ANIM_F_FILL, ANIM_F_PLAY,
    ANIM_NFIELD
};
/* The field of longhand prop (-1: not one). */
int anim_spec_field(const char *prop);
/* spec (NULL: all initial) with field set to value: a new string (malloc). */
char *anim_spec_with(const char *spec, int field, const char *value);

/* ---- The clock: page time in ms (set by the browser; 0 until then) ---- */
void anim_set_clock(double (*now)(void));
double anim_now(void);

const char *anim_prop_name(int prop);
int anim_prop_of(const char *name);

/* A new document: nothing runs. */
void anim_reset(void);

/* ---- Layout ---- */

/* Whether element el (pseudo ps) has rows. */
bool anim_has(NodeId el, int ps);
/* Whether any element has rows. */
bool anim_any(void);

/* Resolves keyframe frame (its declarations) against the box's style:
 * the values it sets (has[p]). */
typedef void (*AnimFrameFn)(void *ud, const void *decls, int n, AnimVal out[ANIM_NPROP], bool has[ANIM_NPROP]);
/* Looks up @keyframes name: its frames (offsets and declarations), or 0. */
typedef int (*AnimKeyframesFn)(void *ud, const char *name, const float **offs, const void *const **decls,
                               const int **counts);

/* Box el/ps has style spec (NULL: none) and these values from the cascade:
 * start the transitions and animations that calls for (detect: a layout
 * whose styles are the document's), stop those it no longer does, then
 * *cur the values at now (set[p]: animated). */
void anim_box(NodeId el, int ps, const char *spec, const AnimVal base[ANIM_NPROP], bool detect, double now,
              AnimKeyframesFn kf, AnimFrameFn frame, void *ud, AnimVal cur[ANIM_NPROP], bool set[ANIM_NPROP]);

/* The values el/ps had before its style gained a transition, for any
 * property that has none yet. */
void anim_seed(NodeId el, int ps, const AnimVal base[ANIM_NPROP]);

/* Subtree root left its document: what runs in it is cancelled now, so that
 * putting it back does not continue them. */
void anim_node_removed(NodeId root);
/* el's pseudo-element ps has no box any more: as cancelled. */
void anim_pseudo_gone(NodeId el, int ps, double now);

/* The value of prop on el/ps at now, when something animates it. */
bool anim_value(NodeId el, int ps, int prop, double now, AnimVal *out);

/* ---- Events (anim_api.c) ---- */
enum { ANIM_EV_TRUN, ANIM_EV_TSTART, ANIM_EV_TEND, ANIM_EV_TCANCEL, ANIM_EV_ASTART, ANIM_EV_AITER, ANIM_EV_AEND,
       ANIM_EV_ACANCEL };
typedef struct AnimEvent {
    NodeId el;
    uint8_t ps, type;
    char name[64];      /* the property (transitions) or the animation */
    double elapsed;     /* s */
} AnimEvent;
/* Up to cap events due at now, each once; the rows of elements no longer
 * in a document dropped.  *live: whether anything still runs. */
int anim_tick(double now, AnimEvent *out, int cap, bool *live);
bool anim_cancels_due(void);

/* The documents of the elements with rows, up to cap. */
int anim_docs(NodeId *out, int cap);

/* ---- Element.getAnimations() ---- */
/* The running animations and transitions of el (its pseudo-elements too),
 * up to cap: their row ids. */
int anim_list(NodeId el, int *rows, int cap);
/* Row r (from anim_list): whether it is still that one (gen), its kind
 * (1 transition, 2 animation) and name. */
bool anim_row_info(int r, uint32_t gen, int *kind, const char **name, NodeId *el, int *ps);
uint32_t anim_row_gen(int r);
/* The properties row r sets at now, as CSS text (commitStyles): calls
 * fn(ud, property, value) for each. */
void anim_row_values(int r, double now, void (*fn)(void *ud, const char *prop, const char *value), void *ud);
void anim_row_cancel(int r);
/* A row's timing for script (Animation objects); times in ms. */
typedef struct AnimRowTiming {
    double start, delay, duration, iterations, current;
    bool paused, ended;
    int fill, direction;        /* (bit 0 forwards, bit 1 backwards); normal, reverse, alternate, alternate-reverse */
} AnimRowTiming;
bool anim_row_timing(int r, uint32_t gen, double now, AnimRowTiming *out);
void anim_row_set_time(int r, uint32_t gen, double now, double t);
void anim_row_set_start(int r, uint32_t gen, double start);
void anim_row_pause(int r, uint32_t gen, double now, bool paused);
/* A value as CSS text for prop (buf of cap). */
void anim_val_css(int prop, const AnimVal *v, char *buf, size_t cap);

#endif

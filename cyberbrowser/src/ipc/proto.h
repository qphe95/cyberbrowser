#ifndef PROTO_H
#define PROTO_H

/* Messages between the processes of the multi-process browser (M1 of
 * SANDBOX_DESIGN.md): the coordinator, the UI, and tabs.  Every payload is
 * a fixed struct, sometimes followed by UTF-8 text (its length is the rest of
 * the message, not NUL-terminated).  Receivers check the length of each
 * message before using it. */

#include <stdint.h>

#define PROTO_URL_MAX 4096
#define PROTO_TITLE_MAX 512

/* ---- UI -> coordinator ------------------------------------------------- */
enum {
    UC_OPEN_TAB = 1,    /* UcOpenTab + URL: start a tab process */
    UC_CLOSE_TAB,       /* UcTab: end it */
    UC_QUIT,            /* (empty): end everything */
};

typedef struct UcOpenTab {
    uint32_t tab;       /* the UI's id for it, unique for the session */
    uint32_t width, height;
} UcOpenTab;

typedef struct UcTab {
    uint32_t tab;
} UcTab;

/* ---- coordinator -> UI ------------------------------------------------- */
enum {
    CU_TAB_STARTED = 1, /* CuTabStarted: its channel and surface, as handles in the UI */
    CU_TAB_EXITED,      /* CuTabExited */
    CU_TAB_FAILED,      /* UcTab: it could not be started */
};

typedef struct CuTabStarted {
    uint32_t tab;
    uint64_t chan_section, chan_ev0, chan_ev1;   /* HANDLE values, valid in the UI */
    uint64_t surface;                            /* HANDLE: read-only section */
    uint64_t surface_bytes;
} CuTabStarted;

typedef struct CuTabExited {
    uint32_t tab;
    uint32_t exit_code;
} CuTabExited;

/* ---- the surface a tab paints into ------------------------------------ */
/* A section of SURFACE_BYTES: a header, then two buffers of
 * SURFACE_MAX_W * SURFACE_MAX_H pixels (32-bit BGRX, top-down rows of
 * `width` pixels).  The tab paints into one buffer while the UI shows the
 * other: it sends TU_FRAME, and paints into that buffer again only after the
 * UI answers UT_FRAME_DONE.  The UI takes width and height from the message,
 * not from shared memory, and checks them against the maximum. */
#define SURFACE_MAX_W 2560
#define SURFACE_MAX_H 1600
#define SURFACE_HEADER 4096
#define SURFACE_BUFFER_BYTES ((uint64_t)SURFACE_MAX_W * SURFACE_MAX_H * 4)
#define SURFACE_BYTES (SURFACE_HEADER + 2 * SURFACE_BUFFER_BYTES)

/* ---- tab -> UI ---------------------------------------------------------- */
enum {
    TU_FRAME = 1,       /* TuFrame: buffer `buffer` holds frame `frame` */
    TU_TITLE,           /* UTF-8 title */
    TU_NAVIGATE,        /* TuNavigate + URL: the user followed a link */
    TU_STATE,           /* TuState */
};

typedef struct TuFrame {
    uint32_t frame, buffer;
    uint32_t width, height;
    uint32_t scroll_y, content_height;
} TuFrame;

typedef struct TuNavigate {
    uint32_t new_tab;   /* target=_blank: open in a new tab */
} TuNavigate;

enum { TAB_LOADING = 1, TAB_LOADED, TAB_LOAD_FAILED };
typedef struct TuState {
    uint32_t state;
} TuState;

/* ---- UI -> tab ---------------------------------------------------------- */
enum {
    UT_RESIZE = 1,      /* UtSize */
    UT_SCROLL,          /* UtScroll */
    UT_CLICK,           /* UtPoint */
    UT_FRAME_DONE,      /* UtFrameDone: the UI is done with that frame's buffer */
};

typedef struct UtSize {
    uint32_t width, height;
} UtSize;

typedef struct UtScroll {
    int32_t dy;
} UtScroll;

typedef struct UtPoint {
    int32_t x, y;       /* in the viewport */
} UtPoint;

typedef struct UtFrameDone {
    uint32_t frame;
} UtFrameDone;

#endif

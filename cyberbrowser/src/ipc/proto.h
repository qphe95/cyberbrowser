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
    UC_WEBGPU,          /* UcTab: that tab asks for a channel to its site's web-GPU process */
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
    CU_TAB_STARTED = 1, /* CuTabStarted: its channel, as handles in the UI */
    CU_TAB_EXITED,      /* CuTabExited */
    CU_TAB_FAILED,      /* UcTab: it could not be started */
    CU_GPU,             /* CuGpu: the UI's channel to the GPU process (again, if it restarted) */
};

typedef struct CuTabStarted {
    uint32_t tab;
    uint64_t chan_section, chan_ev0, chan_ev1;   /* HANDLE values, valid in the UI */
} CuTabStarted;

typedef struct CuGpu {
    uint64_t chan_section, chan_ev0, chan_ev1;   /* HANDLE values, valid in the UI */
} CuGpu;

typedef struct CuTabExited {
    uint32_t tab;
    uint32_t exit_code;
} CuTabExited;

/* The largest viewport: a tab's content is drawn by the GPU process (the
 * tab <-> GPU process messages below). */
#define SURFACE_MAX_W 2560
#define SURFACE_MAX_H 1600

/* ---- tab -> UI ---------------------------------------------------------- */
enum {
    TU_FRAME = 1,       /* TuFrame: frame `frame` went to the GPU process */
    TU_TITLE,           /* UTF-8 title */
    TU_NAVIGATE,        /* TuNavigate + URL: the user followed a link */
    TU_STATE,           /* TuState */
    TU_WANT_WEBGPU,     /* (empty): it needs its site's web-GPU process (passed on as UC_WEBGPU) */
};

typedef struct TuFrame {
    uint32_t frame, unused;
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

/* ---- coordinator -> tab ------------------------------------------------- */
/* One way: the coordinator never reads from a tab. */
enum {
    CT_GPU = 1,         /* CtGpu: a new GPU process; draw on this channel from now on */
    CT_WEBGPU,          /* CtGpu: its channel to its site's web-GPU process (WebGL, WebGPU) */
    CT_WEBGPU_FAILED,   /* (empty): none can be had (no site, a blocked driver, ...), or the one given ended */
};

typedef struct CtGpu {
    uint64_t chan_section, chan_ev0, chan_ev1;   /* HANDLE values, valid in the tab */
} CtGpu;

/* ---- UI -> tab ---------------------------------------------------------- */
enum {
    UT_RESIZE = 1,      /* UtSize */
    UT_SCROLL,          /* UtScroll */
    UT_CLICK,           /* UtPoint */
    UT_VISIBLE,         /* UtVisible: whether the tab is shown (hidden tabs do not render) */
    UT_NAVIGATE,        /* URL: load it in this process, in place of the page (its own site only) */
};

typedef struct UtVisible {
    uint32_t visible;   /* 1 shown, 0 hidden */
} UtVisible;

typedef struct UtSize {
    uint32_t width, height;
} UtSize;

typedef struct UtScroll {
    int32_t dy;
    uint32_t seq;       /* the UI's count of scrolls to this tab: the GPU process gets the same (UG_SCROLL) */
} UtScroll;

typedef struct UtPoint {
    int32_t x, y;       /* in the viewport */
} UtPoint;

/* ---- coordinator -> network process and storage service ------------------ */
/* Each server gets a control channel from the coordinator.  A grant hands it
 * the server end of a channel to a client, with the client's badge: the
 * coordinator's label for it, never the client's own word. */
#define PROTO_SITE_MAX 128          /* = SITE_MAX */

enum {
    CS_GRANT = 1,       /* CsGrant */
    CS_REVOKE,          /* CsRevoke: stop serving that client, close its channel */
    CS_QUIT,            /* (empty): save what is pending and exit */
};

enum {
    GRANT_TAB = 1,      /* a tab, badged with its site ("" = opaque) */
    GRANT_NET,          /* at the storage service or the loopback relay: the network process */
    GRANT_STORE,        /* at the network process: its channel to the storage service */
    GRANT_LOOP,         /* at the network process: its channel to the loopback relay */
    GRANT_UI,           /* at the GPU process: the UI's channel */
};

typedef struct CsGrant {
    uint32_t client;    /* the tab's id (0 for GRANT_NET and GRANT_STORE) */
    uint32_t kind;
    uint64_t chan_section, chan_ev0, chan_ev1;   /* HANDLE values in the server */
    char site[PROTO_SITE_MAX];
} CsGrant;

typedef struct CsRevoke {
    uint32_t client;
} CsRevoke;

/* ---- tab <-> network process (bigmsg.h messages) -------------------------- */
enum {
    TN_REQUEST = 1,     /* NetRequest + method, url, headers, initiator (each NUL-terminated) + body */
    TN_COOKIE_GET,      /* NetCookieGet + url: document.cookie */
    TN_COOKIE_SET,      /* url, NUL, cookie line: document.cookie = ... */
    NT_RESPONSE,        /* NetResponse + url + headers + error + body */
    NT_PART,            /* u32 id, u32 url length, url (where it came from, after redirects) + bytes: a
                         * navigation's body as it arrives, decoded, before its NT_RESPONSE (which
                         * still has it whole) */
};

typedef struct NetRequest {
    uint32_t id;
    uint32_t method_len, url_len, headers_len;  /* each counts its NUL */
    uint32_t nheaders;
    uint32_t body_len;
    uint32_t mode;              /* HTTP_MODE_* (http_download.h): what it is for, the tab's word */
    uint32_t initiator_len;     /* the requesting document's origin, counting its NUL ("": the page's) */
} NetRequest;

typedef struct NetCookieGet {
    uint32_t id;
} NetCookieGet;

/* NetResponse.ok */
enum {
    NET_FAILED = 0,     /* error: why */
    NET_OK = 1,         /* url: where it ended, after redirects */
    NET_ELSEWHERE = 2,  /* the page's document is another site's (SANDBOX_DESIGN.md P10): url is where
                           it went, for the UI to navigate to in a tab of that site */
};

typedef struct NetResponse {
    uint32_t id;
    uint32_t ok;                /* NET_* */
    int32_t status;
    uint32_t headers_len, error_len, body_len;
    uint32_t url_len;           /* (no NUL) */
} NetResponse;

/* ---- clients <-> storage service (bigmsg.h messages) --------------------- */
/* A path is relative to the client's site folder (store_core.h); only the
 * network process names a site (for a site's cookies file). */
enum {
    TS_READ = 1,        /* StoreRequest + site + path */
    TS_WRITE,           /* StoreRequest + site + path + data: replace the file */
    TS_DELETE,          /* StoreRequest + site + path */
    ST_RESULT,          /* StoreResult + data (TS_READ) */
};

typedef struct StoreRequest {
    uint32_t id;
    uint32_t site_len, path_len, data_len;
} StoreRequest;

typedef struct StoreResult {
    uint32_t id;
    uint32_t ok;
    uint32_t data_len;
} StoreResult;

/* ---- network process <-> loopback relay (chan.h messages) ----------------- */
/* Connections to 127.0.0.1 and ::1 (loop_core.h).  Every message starts with
 * the connection's id, which the network process picks. */
#define LOOP_DATA_MAX 16380         /* bytes in one NL_DATA or LN_DATA: a channel message less the id */

enum { LOOP_V4 = 4, LOOP_V6 = 6 };  /* 127.0.0.1, ::1 */

enum {
    NL_CONNECT = 1,     /* NlConnect */
    NL_DATA,            /* id + 1..LOOP_DATA_MAX bytes; the next only after LN_SENT */
    NL_CREDIT,          /* NlCredit: it consumed that many bytes of LN_DATA */
    NL_CLOSE,           /* id */
    LN_CONNECTED,       /* LnConnected */
    LN_DATA,            /* id + 1..LOOP_DATA_MAX bytes */
    LN_SENT,            /* id: the last NL_DATA is written to the socket */
    LN_CLOSED,          /* id: the socket ended */
};

typedef struct NlConnect {
    uint32_t id;
    uint32_t family;    /* LOOP_V4 or LOOP_V6 */
    uint32_t port;
} NlConnect;

typedef struct NlCredit {
    uint32_t id;
    uint32_t bytes;
} NlCredit;

typedef struct LnConnected {
    uint32_t id;
    uint32_t ok;
} LnConnected;

/* ---- tab <-> GPU process (bigmsg.h messages, the tab's draw channel) ------- */
/* A tab sends what it draws as a display list (gpu_wire.h), and before it
 * the resources the list names that the GPU process doesn't have yet: bands
 * of its font slots' glyph atlases and decoded images.  Ids are the tab's
 * own: the GPU process keeps each tab's apart (gpu_decode.h). */
enum {
    TG_FRAME = 1,       /* GpuFrame + WireCmd[ncmds] + WireMask[nmasks] */
    TG_ATLAS,           /* TgAtlas + rows * width bytes: rows y0.. of a font slot's atlas */
    TG_IMAGE,           /* TgImage + width * height * 4 bytes (RGBA): an image, new or replaced */
    GT_FRAME_DONE,      /* GtFrameDone: drawn; the tab may send the next frame */
};

typedef struct TgAtlas {
    uint32_t slot;
    uint32_t width, height;     /* the whole atlas: the same at every upload of a slot */
    uint32_t y0, rows;
} TgAtlas;

typedef struct TgImage {
    uint32_t id;
    uint32_t width, height;
} TgImage;

typedef struct GtFrameDone {
    uint32_t frame;
} GtFrameDone;

/* ---- tab <-> its site's web-GPU process (bigmsg.h messages, the gl channel) - */
/* WebGL and WebGPU (SANDBOX_DESIGN.md section 5.7).  A context is named by
 * the tab's id for it; its calls go as records (webgl_backend.h, webgpu_wire.h)
 * and are answered only where the API returns something.  A presented
 * canvas comes back as pixels, which the tab draws like any image. */
enum {
    TW_GL_CREATE = 1,   /* TwGlCreate: a WebGL context */
    TW_GL_RESIZE,       /* TwGlResize */
    TW_GL_RUN,          /* TwGlRun + records; answered with WT_GL_RESULT if reply */
    TW_PRESENT,         /* TwPresent: the canvas's pixels, as WT_FRAME */
    TW_DESTROY,         /* TwContext: that context goes */
    TW_GPU_RUN,         /* TwGpuRun + WebGPU commands (webgpu_wire.h); WT_GPU_RESULT if reply */
    WT_GL_CREATED,      /* WtCreated + why not (UTF-8) */
    WT_GL_RESULT,       /* WtResult + the queries' results */
    WT_FRAME,           /* WtFrame + width * height * 4 bytes, RGBA, top-down */
    WT_LOST,            /* TwContext: the context is lost (a GPU reset) */
    WT_GPU_RESULT,      /* WtResult + results (webgpu_wire.h) */
};

typedef struct TwGlCreate {
    uint32_t ctx;
    uint32_t version;   /* 1 or 2 */
    uint32_t width, height;
    uint32_t flags;     /* WEBGL_ALPHA, ... (webgl_backend.h) */
} TwGlCreate;

typedef struct TwGlResize {
    uint32_t ctx;
    uint32_t width, height;
} TwGlResize;

typedef struct TwGlRun {
    uint32_t ctx;
    uint32_t seq;
    uint32_t reply;     /* answer when done (its queries' results, or none) */
} TwGlRun;

typedef struct TwPresent {
    uint32_t ctx;
    uint32_t seq;
} TwPresent;

typedef struct TwContext {
    uint32_t ctx;
} TwContext;

typedef struct TwGpuRun {
    uint32_t seq;
    uint32_t reply;
} TwGpuRun;

typedef struct WtCreated {
    uint32_t ctx;
    uint32_t ok;
} WtCreated;

typedef struct WtResult {
    uint32_t ctx;       /* 0 for WebGPU */
    uint32_t seq;
    uint32_t ok;        /* 0: a record was malformed, the rest dropped */
} WtResult;

typedef struct WtFrame {
    uint32_t ctx;
    uint32_t seq;
    uint32_t width, height;
} WtFrame;

/* ---- UI <-> GPU process ---------------------------------------------------- */
enum {
    UG_WINDOW = 1,      /* UgWindow: that content window shows that tab (0: none; 0 x 0: the window is gone) */
    UG_SHOT,            /* UgShot: send what that content window shows, as GU_SHOT */
    GU_SHOT,            /* GuShot + width * height * 4 bytes, BGRX, top-down */
    GU_SHOWN,           /* GuShown: a frame of that tab is in that content window */
    UG_SCROLL,          /* UgScroll: the user scrolled that tab; move its content now (compositor scrolling) */
};

/* Each of the UI's windows has a content window; the GPU process presents
 * the tab it shows there. */
typedef struct UgWindow {
    uint64_t hwnd;              /* the UI's content window */
    uint32_t tab;
    uint32_t width, height;
} UgWindow;

typedef struct UgShot {
    uint64_t hwnd;
} UgShot;

typedef struct UgScroll {
    uint32_t tab;
    uint32_t seq;               /* as in the UtScroll the tab gets */
    int32_t dy;
} UgScroll;

typedef struct GuShot {
    uint32_t width, height;     /* 0 x 0: nothing shown */
} GuShot;

typedef struct GuShown {
    uint32_t tab;
    uint64_t hwnd;
} GuShown;

#endif

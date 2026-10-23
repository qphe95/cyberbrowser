/*
 * Browser Stubs Types - Struct definitions for DOM/Browser API implementations
 * 
 * These structs are allocated from the GC heap and accessed via handle classes.
 * They are defined separately to allow handle classes to wrap them safely.
 */

#ifndef BROWSER_API_IMPL_TYPES_H
#define BROWSER_API_IMPL_TYPES_H

#include "quickjs.h"
#include "quickjs_gc_unified.h"
#include "lockfree_hash_table.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * XMLHttpRequest (from js_quickjs.c)
 * ============================================================================ */
typedef struct {
    char url[2048];
    char method[16];
    int ready_state;
    int status;
    char response_headers[2048];
    int with_credentials;         // XHR CORS credentials flag
    GCValue response_text;        // JS string; undefined until a response
    GCValue onload;
    GCValue onerror;
    GCValue onreadystatechange;
    GCValue headers;
    JSContextHandle ctx;
} XMLHttpRequest;

/* ============================================================================
 * HTMLVideoElement (from js_quickjs.c)
 * ============================================================================ */
typedef struct {
    char id[256];        // Element id for tracking
    char src[2048];
    int ready_state;
    int network_state;
    double current_time;
    double duration;
    int paused;
    int ended;
    int autoplay;
    int loop;
    int muted;
    double volume;
    double playback_rate;
    double default_playback_rate;
    char preload[64];
    char cross_origin[64];
    GCValue onloadstart;
    GCValue onloadedmetadata;
    GCValue oncanplay;
    GCValue onplay;
    GCValue onplaying;
    GCValue onerror;
    GCValue onvolumechange;
    JSContextHandle ctx;
    GCHandle dom_node;   // DomWrap record: the store element this video wraps
} HTMLVideoElement;

/* ============================================================================
 * DOMException (from browser_api_impl.c)
 * ============================================================================ */
typedef struct {
    char name[64];
    char message[256];
    int code;
} DOMExceptionData;

/* ============================================================================
 * Map (polyfill, from browser_api_impl.c)
 * ============================================================================ */
typedef struct {
    GCValue entries;  // Object storing key->value mappings
    int size;
} MapData;

/* ============================================================================
 * ShadowRoot (from browser_api_impl.c)
 * ============================================================================ */
typedef struct {
    GCValue host;                    // The element that hosts this shadow root
    char mode[16];                   // "open" or "closed"
    GCValue innerHTML;               // Shadow root content (as string for stub)
    GCHandle dom_node;               // DomWrap record: the store fragment of the shadow tree
    JSContextHandle ctx;
    bool delegates_focus;            // init.delegatesFocus
    bool manual_slot_assignment;     // init.slotAssignment == "manual"
} ShadowRootData;

/* ============================================================================
 * CustomElementRegistry (from browser_api_impl.c)
 * ============================================================================ */
typedef struct {
    GCValue registry;  // Map of tag names to constructor functions
} CustomElementRegistryData;

/* ============================================================================
 * Animation (from browser_api_impl.c)
 * ============================================================================ */
typedef struct {
    double current_time;
    double duration;
    int play_state;  // 0=idle, 1=running, 2=paused, 3=finished
    GCValue onfinish;
    GCValue effect;
    JSContextHandle ctx;
} AnimationData;

/* ============================================================================
 * KeyframeEffect (from browser_api_impl.c)
 * ============================================================================ */
typedef struct {
    GCValue target;
    GCValue keyframes;
    double duration;
    char easing[32];
} KeyFrameEffectData;

/* ============================================================================
 * FontFace (from browser_api_impl.c)
 * ============================================================================ */
typedef struct {
    char family[256];
    char source[512];
    char display[32];
} FontFaceData;

/* ============================================================================
 * FontFaceSet (from browser_api_impl.c)
 * ============================================================================ */
typedef struct {
    GCValue loaded_fonts;  // Array of loaded FontFace objects
} FontFaceSetData;

/* ============================================================================
 * MutationObserver (from browser_api_impl.c)
 * ============================================================================ */
typedef struct {
    GCValue callback;
    JSContextHandle ctx;
} MutationObserverData;

/* ============================================================================
 * ResizeObserver (from browser_api_impl.c)
 * ============================================================================ */
typedef struct {
    GCValue callback;
    JSContextHandle ctx;
} ResizeObserverData;

/* ============================================================================
 * IntersectionObserver (from browser_api_impl.c)
 * ============================================================================ */
typedef struct {
    GCValue callback;
    GCValue root;
    char rootMargin[32];
    double threshold;
    JSContextHandle ctx;
} IntersectionObserverData;

/* ============================================================================
 * Performance (from browser_api_impl.c)
 * ============================================================================ */

#define PERFORMANCE_MAX_ENTRIES 256

typedef struct {
    char name[256];
    char entryType[64];
    double startTime;
    double duration;
} PerformanceEntryData;

typedef struct {
    double start_time;
    double time_origin;
    PerformanceEntryData entries[PERFORMANCE_MAX_ENTRIES];
    int entry_count;
} PerformanceData;

/* ============================================================================
 * PerformanceObserver (from browser_api_impl.c)
 * ============================================================================ */
typedef struct {
    GCValue callback;
    JSContextHandle ctx;
} PerformanceObserverData;

/* ============================================================================
 * PerformanceTiming (from browser_api_impl.c)
 * ============================================================================ */
typedef struct {
    double navigationStart;
    double unloadEventStart;
    double unloadEventEnd;
    double redirectStart;
    double redirectEnd;
    double fetchStart;
    double domainLookupStart;
    double domainLookupEnd;
    double connectStart;
    double connectEnd;
    double secureConnectionStart;
    double requestStart;
    double responseStart;
    double responseEnd;
    double domLoading;
    double domInteractive;
    double domContentLoadedEventStart;
    double domContentLoadedEventEnd;
    double domComplete;
    double loadEventStart;
    double loadEventEnd;
} PerformanceTimingData;

/* ============================================================================
 * DOMRect/DOMRectReadOnly (from browser_api_impl.c)
 * ============================================================================ */
typedef struct {
    double x;
    double y;
    double width;
    double height;
    double top;
    double right;
    double bottom;
    double left;
} DOMRectData;

/* ============================================================================
 * MediaSource (from browser_api_impl.c)
 * ============================================================================ */
typedef struct {
    char source_buffers[1024];  // JSON array of source buffer types
    char active_source_buffers[1024];
    double duration;
    int ready_state;  // 0=closed, 1=open, 2=ended
    GCValue onsourceopen;
    GCValue onsourceended;
    GCValue onsourceclose;
    JSContextHandle ctx;
} MediaSourceData;

/* ============================================================================
 * SourceBuffer (from browser_api_impl.c)
 * ============================================================================ */
typedef struct {
    char mime_type[256];
    char mode[32];  // "segments" or "sequence"
    double timestamp_offset;
    double append_window_start;
    double append_window_end;
    int updating;
    GCValue onupdatestart;
    GCValue onupdate;
    GCValue onupdateend;
    GCValue onerror;
    GCValue onabort;
    JSContextHandle ctx;

    // Demuxed media (src/media/media_buffer.h); freed by the finalizer.
    struct MediaBuffer *mb;
} SourceBufferData;

/* ============================================================================
 * Event Data Structures (from browser_api_impl.c)
 * ============================================================================ */

typedef struct {
    char type[128];
    int bubbles;
    int cancelable;
    int composed;
    int defaultPrevented;
    int eventPhase;  // 1=capture, 2=target, 3=bubble
    double timeStamp;
    GCValue target;
    GCValue currentTarget;
    GCValue path;    // composed path array populated at dispatch time
    JSContextHandle ctx;
} EventData;

typedef struct {
    EventData base;
    GCValue detail;
} CustomEventData;

typedef struct {
    EventData base;
    double clientX;
    double clientY;
    double screenX;
    double screenY;
    double pageX;
    double pageY;
    int button;
    int buttons;
    int ctrlKey;
    int shiftKey;
    int altKey;
    int metaKey;
} MouseEventData;

typedef struct {
    EventData base;
    GCValue relatedTarget;
} FocusEventData;

/* ============================================================================
 * Real DOM Node Structure (for actual DOM tree implementation)
 * ============================================================================ */

// Node types matching standard DOM
#define DOM_NODE_TYPE_ELEMENT                1
#define DOM_NODE_TYPE_ATTRIBUTE              2
#define DOM_NODE_TYPE_TEXT                   3
#define DOM_NODE_TYPE_CDATA_SECTION          4
#define DOM_NODE_TYPE_ENTITY_REFERENCE       5
#define DOM_NODE_TYPE_ENTITY                 6
#define DOM_NODE_TYPE_PROCESSING_INSTRUCTION 7
#define DOM_NODE_TYPE_COMMENT                8
#define DOM_NODE_TYPE_DOCUMENT               9
#define DOM_NODE_TYPE_DOCUMENT_TYPE         10
#define DOM_NODE_TYPE_DOCUMENT_FRAGMENT     11
#define DOM_NODE_TYPE_NOTATION              12

typedef struct CssComputedStyle {
    LFHashTable *properties;          /* atom handle -> JS string value handle */
} CssComputedStyle;

/* DOM nodes live in the node store (dom_store.h); their JS wrappers carry a
 * DomWrap record (dom_wrap.h). */

/* ============================================================================
 * DOM mutation / layout invalidation
 * ============================================================================ */

/* Set to 1 by DOM mutation APIs. The main loop reads this flag to decide when
 * to rebuild the native HtmlDocument from the JS DOM and re-run layout. */
extern volatile int g_dom_needs_layout;
/* Bumped by every dom_request_layout(), including CSSOM sheet mutations the
 * store versions do not see. */
extern uint64_t g_dom_layout_requests;

/* Changes whenever the rendered document may have changed: tree,
 * attribute and text changes in the document (not detached nodes),
 * watched (style) object writes and layout
 * requests.  Equal stamps mean nothing a script did could show. */
uint64_t dom_change_stamp(void);

/* Mark the document as needing a layout pass. May be called from any thread
 * that holds the JS context (e.g. async image load callbacks). */
void dom_request_layout(void);

/* A layout request for a change the DOM does not show: a CSSOM sheet edit,
 * adopted sheets, an image that loaded.  Bumps g_dom_relayout_requests as
 * well (layout_api never keeps a layout across one). */
extern uint64_t g_dom_relayout_requests;
void dom_request_relayout(void);

/* Inserted <link rel=stylesheet>: their sheets download in the background
 * and fire load / error from the event loop.  The poll runs at the top of
 * each turn; the pending count keeps the page from settling before then. */
int dom_poll_stylesheet_loads(JSContextHandle ctx);
int dom_pending_stylesheet_loads(void);

/* ============================================================================
 * Location object data
 * ============================================================================ */
typedef struct {
    char href[2048];          // Full URL
    char protocol[32];        // 'https:'
    char host[256];           // 'www.youtube.com:8080'
    char hostname[256];       // 'www.youtube.com'
    char port[16];            // '8080' or ''
    char pathname[1024];      // '/watch'
    char search[2048];        // '?v=...'
    char hash[256];           // '#...'
    char origin[256];         // 'https://www.youtube.com'
} LocationData;

/* ============================================================================
 * ServiceWorker API data structures
 * ============================================================================ */

typedef struct {
    char script_url[1024];    // Service worker script URL
    char scope[1024];         // Registration scope
    int state;                // 0=installing, 1=installed, 2=activating, 3=activated
    GCValue installing;       // ServiceWorker object or null
    GCValue waiting;          // ServiceWorker object or null
    GCValue active;           // ServiceWorker object or null
    JSContextHandle ctx;
} ServiceWorkerRegistrationData;

typedef struct {
    char script_url[1024];    // Script URL
    char state[32];           // "installing", "installed", "activating", "activated", "redundant"
    int id;                   // Unique worker ID
} ServiceWorkerData;

typedef struct {
    GCValue registrations;    // Array of ServiceWorkerRegistration objects
    GCValue message_handlers; // Array of message event handlers
    GCValue controller;       // Active ServiceWorker or null
    int next_worker_id;       // ID counter for workers
} ServiceWorkerContainerData;

/* ============================================================================
 * Storage API data structures
 * ============================================================================ */

/* Web Storage quota per store, counted like Chromium: 5M UTF-16 code
 * units of keys + values (we count UTF-8 bytes, which is within 2x). */
#define STORAGE_QUOTA_BYTES (10u * 1024u * 1024u)

typedef struct {
    char *key;          /* malloc'd, NUL-terminated */
    char *value;        /* malloc'd, may contain NULs; length in value_len */
    size_t key_len;
    size_t value_len;
} StorageItem;

typedef struct {
    StorageItem *items; /* insertion order */
    int count;
    int cap;
    size_t bytes;       /* sum of key_len + value_len */
} StorageData;

/* ============================================================================
 * Console API data structures
 * ============================================================================ */

#define CONSOLE_MAX_TIMERS 64
#define CONSOLE_MAX_COUNTERS 64
#define CONSOLE_MAX_LABEL_LEN 128

typedef struct {
    char label[CONSOLE_MAX_LABEL_LEN];
    double start_time;        // Start time in milliseconds
    int active;               // 1 if timer is active
} ConsoleTimer;

typedef struct {
    char label[CONSOLE_MAX_LABEL_LEN];
    int count;                // Current count
    int active;               // 1 if counter is active
} ConsoleCounter;

typedef struct {
    ConsoleTimer timers[CONSOLE_MAX_TIMERS];
    ConsoleCounter counters[CONSOLE_MAX_COUNTERS];
    int group_depth;          // Current group nesting level
    int group_collapsed[16];  // Whether each level is collapsed
    int timer_count;
    int counter_count;
} ConsoleData;

/* ============================================================================
 * Date (from browser_api_impl.c)
 * ============================================================================ */
typedef struct {
    long long timestamp_ms;  // Milliseconds since epoch (UTC)
    int is_valid;            // 0 if invalid date
} DateData;

/* ============================================================================
 * Handle types
 * ============================================================================
 * Each struct above lives in the GC heap and is referred to by a GCHandle.
 * The *Handle names document which struct a handle points at; access the
 * fields with the SAFE_* macros from quickjs_handles.h, e.g.
 *     SAFE_GET_OR(vid, HTMLVideoElementHandle, paused, 0)
 * GC_STRUCT_OF_<HandleType> tells those macros which struct to cast to.
 */
typedef GCHandle XMLHttpRequestHandle;
typedef GCHandle HTMLVideoElementHandle;
typedef GCHandle DOMExceptionDataHandle;
typedef GCHandle MapDataHandle;
typedef GCHandle ShadowRootDataHandle;
typedef GCHandle CustomElementRegistryDataHandle;
typedef GCHandle AnimationDataHandle;
typedef GCHandle KeyFrameEffectDataHandle;
typedef GCHandle FontFaceDataHandle;
typedef GCHandle FontFaceSetDataHandle;
typedef GCHandle MutationObserverDataHandle;
typedef GCHandle ResizeObserverDataHandle;
typedef GCHandle IntersectionObserverDataHandle;
typedef GCHandle PerformanceDataHandle;
typedef GCHandle PerformanceEntryDataHandle;
typedef GCHandle PerformanceObserverDataHandle;
typedef GCHandle PerformanceTimingDataHandle;
typedef GCHandle DOMRectDataHandle;
typedef GCHandle MediaSourceDataHandle;
typedef GCHandle SourceBufferDataHandle;
typedef GCHandle EventHandle;
typedef GCHandle CustomEventHandle;
typedef GCHandle MouseEventHandle;
typedef GCHandle FocusEventHandle;
typedef GCHandle DateDataHandle;

#define GC_STRUCT_OF_XMLHttpRequestHandle            XMLHttpRequest
#define GC_STRUCT_OF_HTMLVideoElementHandle          HTMLVideoElement
#define GC_STRUCT_OF_DOMExceptionDataHandle          DOMExceptionData
#define GC_STRUCT_OF_MapDataHandle                   MapData
#define GC_STRUCT_OF_ShadowRootDataHandle            ShadowRootData
#define GC_STRUCT_OF_CustomElementRegistryDataHandle CustomElementRegistryData
#define GC_STRUCT_OF_AnimationDataHandle             AnimationData
#define GC_STRUCT_OF_KeyFrameEffectDataHandle        KeyFrameEffectData
#define GC_STRUCT_OF_FontFaceDataHandle              FontFaceData
#define GC_STRUCT_OF_FontFaceSetDataHandle           FontFaceSetData
#define GC_STRUCT_OF_MutationObserverDataHandle      MutationObserverData
#define GC_STRUCT_OF_ResizeObserverDataHandle        ResizeObserverData
#define GC_STRUCT_OF_IntersectionObserverDataHandle  IntersectionObserverData
#define GC_STRUCT_OF_PerformanceDataHandle           PerformanceData
#define GC_STRUCT_OF_PerformanceEntryDataHandle      PerformanceEntryData
#define GC_STRUCT_OF_PerformanceObserverDataHandle   PerformanceObserverData
#define GC_STRUCT_OF_PerformanceTimingDataHandle     PerformanceTimingData
#define GC_STRUCT_OF_DOMRectDataHandle               DOMRectData
#define GC_STRUCT_OF_MediaSourceDataHandle           MediaSourceData
#define GC_STRUCT_OF_SourceBufferDataHandle          SourceBufferData
#define GC_STRUCT_OF_EventHandle                     EventData
#define GC_STRUCT_OF_CustomEventHandle               CustomEventData
#define GC_STRUCT_OF_MouseEventHandle                MouseEventData
#define GC_STRUCT_OF_FocusEventHandle                FocusEventData
#define GC_STRUCT_OF_DateDataHandle                  DateData
#define GC_STRUCT_OF_XMLHttpRequest                  XMLHttpRequest
#define GC_STRUCT_OF_HTMLVideoElement                HTMLVideoElement
#define GC_STRUCT_OF_DOMExceptionData                DOMExceptionData
#define GC_STRUCT_OF_MapData                         MapData
#define GC_STRUCT_OF_ShadowRootData                  ShadowRootData
#define GC_STRUCT_OF_CustomElementRegistryData       CustomElementRegistryData
#define GC_STRUCT_OF_AnimationData                   AnimationData
#define GC_STRUCT_OF_KeyFrameEffectData              KeyFrameEffectData
#define GC_STRUCT_OF_FontFaceData                    FontFaceData
#define GC_STRUCT_OF_FontFaceSetData                 FontFaceSetData
#define GC_STRUCT_OF_MutationObserverData            MutationObserverData
#define GC_STRUCT_OF_ResizeObserverData              ResizeObserverData
#define GC_STRUCT_OF_IntersectionObserverData        IntersectionObserverData
#define GC_STRUCT_OF_PerformanceData                 PerformanceData
#define GC_STRUCT_OF_PerformanceEntryData            PerformanceEntryData
#define GC_STRUCT_OF_PerformanceObserverData         PerformanceObserverData
#define GC_STRUCT_OF_PerformanceTimingData           PerformanceTimingData
#define GC_STRUCT_OF_DOMRectData                     DOMRectData
#define GC_STRUCT_OF_MediaSourceData                 MediaSourceData
#define GC_STRUCT_OF_SourceBufferData                SourceBufferData
#define GC_STRUCT_OF_EventData                       EventData
#define GC_STRUCT_OF_CustomEventData                 CustomEventData
#define GC_STRUCT_OF_MouseEventData                  MouseEventData
#define GC_STRUCT_OF_FocusEventData                  FocusEventData
#define GC_STRUCT_OF_DateData                        DateData

#ifdef __cplusplus
} // extern "C"
#endif

#endif /* BROWSER_API_IMPL_TYPES_H */

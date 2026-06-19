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
 * XMLHttpRequest (from js_quickjs.cpp)
 * ============================================================================ */
typedef struct {
    char url[2048];
    char method[16];
    int ready_state;
    int status;
    char response_text[2097152];  // 256KB for large JSON responses
    char response_headers[2048];
    GCValue onload;
    GCValue onerror;
    GCValue onreadystatechange;
    GCValue headers;
    JSContextHandle ctx;
} XMLHttpRequest;

/* ============================================================================
 * HTMLVideoElement (from js_quickjs.cpp)
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
    GCValue onloadstart;
    GCValue onloadedmetadata;
    GCValue oncanplay;
    GCValue onplay;
    GCValue onplaying;
    GCValue onerror;
    JSContextHandle ctx;
} HTMLVideoElement;

/* ============================================================================
 * DOMException (from browser_api_impl.cpp)
 * ============================================================================ */
typedef struct {
    char name[64];
    char message[256];
    int code;
} DOMExceptionData;

/* ============================================================================
 * Map (polyfill, from browser_api_impl.cpp)
 * ============================================================================ */
typedef struct {
    GCValue entries;  // Object storing key->value mappings
    int size;
} MapData;

/* ============================================================================
 * ShadowRoot (from browser_api_impl.cpp)
 * ============================================================================ */
typedef struct {
    GCValue host;           // The element that hosts this shadow root
    char mode[16];          // "open" or "closed"
    GCValue innerHTML;      // Shadow root content (as string for stub)
    
    // Real DOM tree structure for shadow root children
    GCValue first_child;    // First child node or null
    GCValue last_child;     // Last child node or null
    int child_count;        // Number of child nodes
    
    // Reference back to shadow root JS object for tree operations
    JSContextHandle ctx;
} ShadowRootData;

/* ============================================================================
 * CustomElementRegistry (from browser_api_impl.cpp)
 * ============================================================================ */
typedef struct {
    GCValue registry;  // Map of tag names to constructor functions
} CustomElementRegistryData;

/* ============================================================================
 * Animation (from browser_api_impl.cpp)
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
 * KeyframeEffect (from browser_api_impl.cpp)
 * ============================================================================ */
typedef struct {
    GCValue target;
    GCValue keyframes;
    double duration;
    char easing[32];
} KeyFrameEffectData;

/* ============================================================================
 * FontFace (from browser_api_impl.cpp)
 * ============================================================================ */
typedef struct {
    char family[256];
    char source[512];
    char display[32];
} FontFaceData;

/* ============================================================================
 * FontFaceSet (from browser_api_impl.cpp)
 * ============================================================================ */
typedef struct {
    GCValue loaded_fonts;  // Array of loaded FontFace objects
} FontFaceSetData;

/* ============================================================================
 * MutationObserver (from browser_api_impl.cpp)
 * ============================================================================ */
typedef struct {
    GCValue callback;
    JSContextHandle ctx;
} MutationObserverData;

/* ============================================================================
 * ResizeObserver (from browser_api_impl.cpp)
 * ============================================================================ */
typedef struct {
    GCValue callback;
    JSContextHandle ctx;
} ResizeObserverData;

/* ============================================================================
 * IntersectionObserver (from browser_api_impl.cpp)
 * ============================================================================ */
typedef struct {
    GCValue callback;
    GCValue root;
    char rootMargin[32];
    double threshold;
    JSContextHandle ctx;
} IntersectionObserverData;

/* ============================================================================
 * Performance (from browser_api_impl.cpp)
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
 * PerformanceObserver (from browser_api_impl.cpp)
 * ============================================================================ */
typedef struct {
    GCValue callback;
    JSContextHandle ctx;
} PerformanceObserverData;

/* ============================================================================
 * PerformanceTiming (from browser_api_impl.cpp)
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
 * DOMRect/DOMRectReadOnly (from browser_api_impl.cpp)
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
 * MediaSource (from browser_api_impl.cpp)
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
 * SourceBuffer (from browser_api_impl.cpp)
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
} SourceBufferData;

/* ============================================================================
 * Event Data Structures (from browser_api_impl.cpp)
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

// Maximum attribute count per element
#define DOM_MAX_ATTRIBUTES 64

typedef struct DOMAttribute {
    char name[64];
    char value[2048];
} DOMAttribute;

typedef struct CssComputedStyle {
    LFHashTable *properties;          /* atom handle -> JS string value handle */
} CssComputedStyle;

typedef struct DOMNode {
    int node_type;                    // DOM_NODE_TYPE_*
    char node_name[64];               // tag name for elements, "#text" for text, etc.
    char node_value[4096];            // For text/comment nodes
    
    // Tree structure pointers (stored as JS object references via handles)
    GCValue parent_node;              // Parent node or null
    GCValue first_child;              // First child or null
    GCValue last_child;               // Last child or null
    GCValue previous_sibling;         // Previous sibling or null
    GCValue next_sibling;             // Next sibling or null
    GCValue owner_document;           // Owning document
    
    // Element-specific data
    DOMAttribute attributes[DOM_MAX_ATTRIBUTES];
    int attribute_count;
    char id[256];
    char class_name[1024];
    
    // Computed CSS properties produced by parallel CSS application.
    GCHandle computed_style_handle;   // handle to CssComputedStyle, or GC_HANDLE_NULL
    
    // Index-table list chaining (class/tag tables hold lists of elements).
    GCHandle next_class_sibling;      // next element with same class atom, or NULL
    GCHandle next_tag_sibling;        // next element with same tag atom, or NULL
    
    // Shadow DOM
    GCValue shadow_root;              // Attached shadow root or null
    
    // Back-reference to the JS object that owns this DOMNode data.
    // Used by index tables to return the public element object.
    GCValue js_object;
    
    // Internal reference to the JS object this data belongs to
    // Used for callbacks and reference management
    JSContextHandle ctx;
} DOMNode;

typedef struct CssDocumentState {
    LFHashTable *id_table;            /* atom handle -> DOMNode handle */
    LFHashTable *class_table;         /* atom handle -> head DOMNode handle (chained) */
    LFHashTable *tag_table;           /* atom handle -> head DOMNode handle (chained) */
} CssDocumentState;

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

#define STORAGE_MAX_ITEMS 1024
#define STORAGE_MAX_KEY_LEN 256
#define STORAGE_MAX_VALUE_LEN 8192

typedef struct {
    char key[STORAGE_MAX_KEY_LEN];
    char value[STORAGE_MAX_VALUE_LEN];
    int used;  // 1 if slot is used, 0 if empty
} StorageItem;

typedef struct {
    StorageItem items[STORAGE_MAX_ITEMS];
    int count;  // Number of active items
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
 * Date (from browser_api_impl.cpp)
 * ============================================================================ */
typedef struct {
    long long timestamp_ms;  // Milliseconds since epoch (UTC)
    int is_valid;            // 0 if invalid date
} DateData;

#ifdef __cplusplus
} // extern "C"
#endif

#endif /* BROWSER_API_IMPL_TYPES_H */

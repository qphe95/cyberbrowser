/*
 * cyber_profile.h - lightweight phase profiler for multithreaded flame graphs.
 *
 * Usage:
 *   CP_SCOPE("fetch")  -> records an event until end of scope
 *   CP_BEGIN("fetch"); ... CP_END("fetch");
 *   CP_INSTANT("mark");
 *   cp_profile_flush("profile.json");  -> writes events as JSON
 *
 * Enabled when CYBER_PROFILE is set in the environment; otherwise nearly free
 * (a branch on a cached flag).  Events are appended to a mutex-protected
 * buffer with per-thread open-event stacks, giving parent/child nesting that
 * the flame-graph renderer turns into stacked bars per thread lane.
 */
#ifndef CYBER_PROFILE_H
#define CYBER_PROFILE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Returns 1 when profiling is enabled (CYBER_PROFILE env var is set). */
int cp_profile_enabled(void);

/* Push/pop a named event on the current thread's open-event stack. */
void cp_profile_begin(const char *name, const char *cat);
void cp_profile_end(const char *name);

/* Record a zero-duration point event on the current thread. */
void cp_profile_instant(const char *name, const char *cat);

/* Write all recorded events to a JSON file (array of event objects). */
void cp_profile_flush(const char *path);

/* Render all recorded events as a multithreaded flame graph PNG.
 * One lane per thread (main thread first), bars stacked by call depth,
 * x axis is wall-clock time.  Uses stb_image_write + stb_truetype. */
void cp_profile_write_flamegraph(const char *path);

#ifdef __cplusplus
}

/* C++ RAII scope helper. */
struct CyberProfileScope {
    const char *name;
    explicit CyberProfileScope(const char *n, const char *cat = "main") : name(n) {
        if (cp_profile_enabled()) cp_profile_begin(n, cat);
    }
    ~CyberProfileScope() {
        if (cp_profile_enabled()) cp_profile_end(name);
    }
};
#define CP_SCOPE(n) CyberProfileScope _cp_scope_##__LINE__(n)
#define CP_SCOPE_CAT(n, c) CyberProfileScope _cp_scope_##__LINE__(n, c)
#else
#define CP_SCOPE(n) do { if (cp_profile_enabled()) cp_profile_begin(n, "main"); } while (0)
#define CP_SCOPE_CAT(n, c) do { if (cp_profile_enabled()) cp_profile_begin(n, c); } while (0)
#endif

#define CP_BEGIN(n) do { if (cp_profile_enabled()) cp_profile_begin(n, "main"); } while (0)
#define CP_BEGIN_CAT(n, c) do { if (cp_profile_enabled()) cp_profile_begin(n, c); } while (0)
#define CP_END(n) do { if (cp_profile_enabled()) cp_profile_end(n); } while (0)
#define CP_INSTANT(n) do { if (cp_profile_enabled()) cp_profile_instant(n, "main"); } while (0)
#define CP_INSTANT_CAT(n, c) do { if (cp_profile_enabled()) cp_profile_instant(n, c); } while (0)

#endif /* CYBER_PROFILE_H */

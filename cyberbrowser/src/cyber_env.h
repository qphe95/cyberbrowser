/*
 * Environment switches, read once (PARALLEL_DOM.md §9, phase 0).
 *
 * The debug switches (CYBER_TRACE_*, CYBER_DUMP_*, ...) used to be read with
 * getenv() inside QuickJS hot paths: property definition, the get-field
 * opcode, every throw.  getenv scans the whole environment, so that cost was
 * paid on every property access of every script.
 *
 *   - CYBER_ENV(bit): the hot switches, as bits of one flags word that is
 *     filled on first use.  One relaxed load and a test.
 *   - cyber_getenv(name): any variable, cached by name after the first
 *     lookup.  Lock-free, safe from any thread.
 *
 * The environment is read once per process; cyber_env_reset() forgets the
 * cache (tests that change variables at run time).
 */
#ifndef CYBER_ENV_H
#define CYBER_ENV_H

#include <stdatomic.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    CYBER_T_DF            = 1u << 0,   /* CYBER_TRACE_DF */
    CYBER_T_GV            = 1u << 1,   /* CYBER_TRACE_GV */
    CYBER_T_NAF           = 1u << 2,   /* CYBER_TRACE_NAF */
    CYBER_T_NULLPROP      = 1u << 3,   /* CYBER_TRACE_NULLPROP */
    CYBER_T_DEFINE        = 1u << 4,   /* CYBER_TRACE_DEFINE */
    CYBER_T_DELETEPROP    = 1u << 5,   /* CYBER_TRACE_DELETEPROP */
    CYBER_T_DUMP_BC       = 1u << 6,   /* CYBER_DUMP_BC */
    CYBER_T_LOG_THROW     = 1u << 7,   /* CYBER_LOG_THROW */
    CYBER_T_DUMP_SCRIPTS  = 1u << 8,   /* CYBER_DUMP_SCRIPTS */
    CYBER_T_RAF           = 1u << 9,   /* CYBER_TRACE_RAF */
    CYBER_T_LIFECYCLE     = 1u << 10,  /* CYBER_TRACE_LIFECYCLE */
    CYBER_T_ROOTINS       = 1u << 11,  /* CYBER_TRACE_ROOTINS */
    CYBER_T_INNERHTML     = 1u << 12,  /* CYBER_TRACE_INNERHTML */
    CYBER_T_UPGRADE       = 1u << 13,  /* CYBER_TRACE_UPGRADE */
    CYBER_T_REMOVE        = 1u << 14,  /* CYBER_TRACE_REMOVE */
    CYBER_T_INS           = 1u << 15,  /* CYBER_TRACE_INS */
    CYBER_T_DEBUG_TC      = 1u << 16,  /* CYBER_DEBUG_TC */
    CYBER_T_PAGEFLOW      = 1u << 17,  /* CYBER_TRACE_PAGEFLOW */
    CYBER_T_INITIALIZED   = 1u << 31
};

extern _Atomic uint32_t g_cyber_env_flags;
uint32_t cyber_env_flags_init(void);

static inline uint32_t cyber_env_flags(void) {
    uint32_t f = atomic_load_explicit(&g_cyber_env_flags, memory_order_relaxed);
    return f ? f : cyber_env_flags_init();
}
#define CYBER_ENV(bit) ((cyber_env_flags() & (uint32_t)(bit)) != 0)

/* getenv(name), cached after the first call for that name. */
const char *cyber_getenv(const char *name);

/* Forget everything read so far (tests). */
void cyber_env_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* CYBER_ENV_H */

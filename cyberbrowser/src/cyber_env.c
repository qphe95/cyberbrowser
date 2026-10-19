/* Environment switches read once (cyber_env.h). */
#include "cyber_env.h"

#include <stdlib.h>
#include <string.h>

_Atomic uint32_t g_cyber_env_flags;

static const struct { const char *name; uint32_t bit; } k_flag_names[] = {
    { "CYBER_TRACE_DF",         CYBER_T_DF },
    { "CYBER_TRACE_GV",         CYBER_T_GV },
    { "CYBER_TRACE_NAF",        CYBER_T_NAF },
    { "CYBER_TRACE_NULLPROP",   CYBER_T_NULLPROP },
    { "CYBER_TRACE_DEFINE",     CYBER_T_DEFINE },
    { "CYBER_TRACE_DELETEPROP", CYBER_T_DELETEPROP },
    { "CYBER_DUMP_BC",          CYBER_T_DUMP_BC },
    { "CYBER_LOG_THROW",        CYBER_T_LOG_THROW },
    { "CYBER_DUMP_SCRIPTS",     CYBER_T_DUMP_SCRIPTS },
    { "CYBER_TRACE_RAF",        CYBER_T_RAF },
    { "CYBER_TRACE_LIFECYCLE",  CYBER_T_LIFECYCLE },
    { "CYBER_TRACE_ROOTINS",    CYBER_T_ROOTINS },
    { "CYBER_TRACE_INNERHTML",  CYBER_T_INNERHTML },
    { "CYBER_TRACE_UPGRADE",    CYBER_T_UPGRADE },
    { "CYBER_TRACE_REMOVE",     CYBER_T_REMOVE },
    { "CYBER_TRACE_INS",        CYBER_T_INS },
    { "CYBER_DEBUG_TC",         CYBER_T_DEBUG_TC },
    { "CYBER_TRACE_PAGEFLOW",   CYBER_T_PAGEFLOW },
};

uint32_t cyber_env_flags_init(void) {
    uint32_t f = CYBER_T_INITIALIZED;
    for (size_t i = 0; i < sizeof(k_flag_names) / sizeof(k_flag_names[0]); i++)
        if (getenv(k_flag_names[i].name)) f |= k_flag_names[i].bit;
    /* Every thread computes the same word; the first store wins harmlessly. */
    atomic_store_explicit(&g_cyber_env_flags, f, memory_order_relaxed);
    return f;
}

/* ---- cached getenv: open addressing, insert-only, lock-free ---- */

enum { SLOT_EMPTY = 0, SLOT_BUSY = 1, SLOT_READY = 2 };
#define ENV_SLOTS 256u

typedef struct {
    _Atomic uint32_t state;
    uint32_t hash;
    char *name;
    char *value;    /* NULL when the variable is unset */
} EnvSlot;

static EnvSlot g_slots[ENV_SLOTS];

static uint32_t env_hash(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h ? h : 1;
}

static char *env_dup(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

const char *cyber_getenv(const char *name) {
    if (!name) return NULL;
    uint32_t h = env_hash(name);
    for (uint32_t probe = 0; probe < ENV_SLOTS; probe++) {
        EnvSlot *s = &g_slots[(h + probe) & (ENV_SLOTS - 1)];
        uint32_t st = atomic_load_explicit(&s->state, memory_order_acquire);
        if (st == SLOT_EMPTY) {
            uint32_t expect = SLOT_EMPTY;
            if (atomic_compare_exchange_strong_explicit(&s->state, &expect, SLOT_BUSY,
                                                        memory_order_acq_rel,
                                                        memory_order_acquire)) {
                const char *v = getenv(name);
                s->hash = h;
                s->name = env_dup(name);
                s->value = v ? env_dup(v) : NULL;
                atomic_store_explicit(&s->state, SLOT_READY, memory_order_release);
                return s->value;
            }
            st = expect;
        }
        if (st == SLOT_BUSY) return getenv(name);   /* being filled: don't wait */
        if (s->hash == h && s->name && strcmp(s->name, name) == 0) return s->value;
    }
    return getenv(name);   /* table full */
}

void cyber_env_reset(void) {
    /* Old strings are leaked on purpose: another thread may still hold them. */
    for (uint32_t i = 0; i < ENV_SLOTS; i++)
        atomic_store_explicit(&g_slots[i].state, SLOT_EMPTY, memory_order_release);
    atomic_store_explicit(&g_cyber_env_flags, 0, memory_order_relaxed);
}

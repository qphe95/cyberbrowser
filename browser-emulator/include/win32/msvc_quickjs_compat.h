#ifndef MSVC_QUICKJS_COMPAT_H
#define MSVC_QUICKJS_COMPAT_H

/*
 * Compatibility shims for building QuickJS with MSVC.
 * QuickJS uses GCC builtins and attributes that MSVC does not support.
 * Force-include this header (/FImsvc_quickjs_compat.h) when compiling
 * QuickJS sources with cl.exe.
 */

#ifdef _MSC_VER

#include <stdint.h>
#include <intrin.h>

/* Strip GCC attributes */
#define __attribute__(x)

/* __builtin_clz / __builtin_clzll */
static inline int __msvc_clz32(unsigned int x) {
    unsigned long index;
    _BitScanReverse(&index, x);
    return 31 - (int)index;
}

static inline int __msvc_clz64(uint64_t x) {
    unsigned long index;
    _BitScanReverse64(&index, x);
    return 63 - (int)index;
}

/* __builtin_ctz / __builtin_ctzll */
static inline int __msvc_ctz32(unsigned int x) {
    unsigned long index;
    _BitScanForward(&index, x);
    return (int)index;
}

static inline int __msvc_ctz64(uint64_t x) {
    unsigned long index;
    _BitScanForward64(&index, x);
    return (int)index;
}

#define __builtin_clz   __msvc_clz32
#define __builtin_clzll __msvc_clz64
#define __builtin_ctz   __msvc_ctz32
#define __builtin_ctzll __msvc_ctz64
#define __builtin_expect(x, y) (x)

#endif /* _MSC_VER */
#endif /* MSVC_QUICKJS_COMPAT_H */

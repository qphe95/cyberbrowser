/* intrin.h - for Eva: the compiler intrinsics that headers the shims
 * include (quickjs_gc_unified.h, through display_list.h) wrap.  The shims
 * don't call them; declared so those headers parse.  TRUSTED, see
 * windows.h. */
#ifndef FC_INTRIN_H
#define FC_INTRIN_H
#include <windows.h>

#define _ReadWriteBarrier() ((void)0)
#define _mm_pause() ((void)0)
void *_InterlockedCompareExchangePointer(void *volatile *p, void *exchange, void *comparand);
long _InterlockedCompareExchange(volatile long *p, long exchange, long comparand);
long long _InterlockedCompareExchange64(volatile long long *p, long long exchange, long long comparand);
long _InterlockedExchangeAdd(volatile long *p, long v);
long long _InterlockedExchangeAdd64(volatile long long *p, long long v);
long _InterlockedIncrement(volatile long *p);
long _InterlockedDecrement(volatile long *p);
long long _InterlockedIncrement64(volatile long long *p);
long long _InterlockedDecrement64(volatile long long *p);
long _InterlockedExchange(volatile long *p, long v);
long long _InterlockedExchange64(volatile long long *p, long long v);
void *_InterlockedExchangePointer(void *volatile *p, void *v);
long _InterlockedOr(volatile long *p, long v);
long _InterlockedAnd(volatile long *p, long v);
unsigned char _BitScanForward(unsigned long *index, unsigned long mask);
unsigned char _BitScanReverse(unsigned long *index, unsigned long mask);
unsigned char _BitScanForward64(unsigned long *index, unsigned long long mask);
unsigned char _BitScanReverse64(unsigned long *index, unsigned long long mask);
unsigned long long __rdtsc(void);

#endif

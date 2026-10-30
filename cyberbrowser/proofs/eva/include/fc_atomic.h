/* fc_atomic.h - GCC's type-generic __atomic builtins, which Frama-C 33
 * doesn't know, as the plain accesses they are in one thread.  For Eva
 * only (included by windows.h).  Ordering between threads is outside what
 * Eva checks: chan.c's use of it is reviewed (SANDBOX_DESIGN.md 8.6, P2). */
#ifndef FC_ATOMIC_H
#define FC_ATOMIC_H

#define __atomic_load_n(p, order) (*(p))
#define __atomic_store_n(p, v, order) ((void)(*(p) = (v)))
#define __atomic_thread_fence(order) ((void)0)

#endif

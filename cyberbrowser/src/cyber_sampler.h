/* Sampling profiler of the main thread (CYBER_SAMPLE=<path>; Windows). */
#ifndef CYBER_SAMPLER_H
#define CYBER_SAMPLER_H

#include <stdint.h>

/* Start sampling the calling thread (no-op unless CYBER_SAMPLE is set). */
void cyber_sampler_start(void);
/* Stop and write the samples. */
void cyber_sampler_stop(void);
/* Validate the C heaps continuously and report the first damage with the
 * calling thread's stack (no-op unless CYBER_HEAPCHECK is set). */
void cyber_heapcheck_start(void);

#endif /* CYBER_SAMPLER_H */

#ifndef GPU_BLOCKLIST_H
#define GPU_BLOCKLIST_H

/* Which GPUs and drivers may run WebGL and WebGPU, and with what
 * (SANDBOX_DESIGN.md section 5.7): drivers with known exploitable bugs, or
 * older than a minimum, get neither (no software fallback); drivers with a
 * poor record get fewer features. */

#include <stdbool.h>
#include <stdint.h>

typedef struct GpuDevice {
    uint32_t vendor, device;        /* PCI ids; 0 if unknown */
    const char *description;        /* the library's: "NVIDIA GeForce ...", ANGLE's renderer string */
    const char *driver;             /* the driver's version, as the library gives it, or "" */
} GpuDevice;

enum {
    GPU_FEATURE_COMPUTE = 1,        /* WebGPU compute pipelines */
    GPU_FEATURE_STORAGE_TEXTURES = 2,
    GPU_FEATURE_EXTENSIONS = 4,     /* uncommon WebGL extensions and WebGPU features */
};

typedef struct GpuVerdict {
    bool blocked;
    uint32_t denied;                /* GPU_FEATURE_* turned off */
    char why[160];
} GpuVerdict;

/* The verdict for a device.  CYBER_GPU_BLOCK=<text> (tests) blocks every
 * device whose description contains it; CYBER_GPU_DENY=<n> denies features
 * n (GPU_FEATURE_*) on every device. */
GpuVerdict gpu_blocklist_check(const GpuDevice *d);

#endif

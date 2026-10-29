/* gpu_blocklist.c - which GPUs and drivers run WebGL and WebGPU
 * (gpu_blocklist.h).  A starting list, after Chrome's software rendering
 * list: entries name a vendor, optionally a device, and a driver version
 * below which they apply. */
#include "gpu_blocklist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VENDOR_INTEL 0x8086
#define VENDOR_AMD 0x1002
#define VENDOR_NVIDIA 0x10de
#define VENDOR_MICROSOFT 0x1414     /* WARP and the Basic Render Driver: software */

typedef struct Rule {
    uint32_t vendor, device;        /* device 0: any */
    const char *below;              /* driver version below which it applies ("" any) */
    bool block;
    uint32_t deny;
    const char *why;
} Rule;

static const Rule RULES[] = {
    /* Windows drivers older than these have known GPU-process escapes, or
     * lack robust buffer access: Intel's before the 2019 DCH drivers,
     * AMD's before Adrenalin 2019, NVIDIA's before R418. */
    { VENDOR_INTEL, 0, "26.20.100.6000", true, 0, "Intel drivers before 26.20.100.6000" },
    { VENDOR_AMD, 0, "26.20.11000.0", true, 0, "AMD drivers before 26.20.11000" },
    { VENDOR_NVIDIA, 0, "25.21.14.1800", true, 0, "NVIDIA drivers before 418" },
    /* Software devices: slow, and no reason to expose compute to pages. */
    { VENDOR_MICROSOFT, 0, "", false, GPU_FEATURE_COMPUTE | GPU_FEATURE_STORAGE_TEXTURES, "a software device" },
};

/* a < b, comparing dotted numbers ("32.0.16.1088"); an empty or unknown
 * version is taken as current (not below). */
static bool version_below(const char *a, const char *b) {
    if (!a || !*a || !b || !*b) return false;
    while (*a || *b) {
        long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
        if (x != y) return x < y;
        if (*a == '.') a++;
        if (*b == '.') b++;
        if (!*a && !*b) break;
        if (!*a) return true;
        if (!*b) return false;
    }
    return false;
}

GpuVerdict gpu_blocklist_check(const GpuDevice *d) {
    GpuVerdict v;
    memset(&v, 0, sizeof(v));
    const char *test = getenv("CYBER_GPU_BLOCK");
    if (test && *test && d->description && strstr(d->description, test)) {
        v.blocked = true;
        snprintf(v.why, sizeof(v.why), "blocked for tests (CYBER_GPU_BLOCK=%s)", test);
        return v;
    }
    for (size_t i = 0; i < sizeof(RULES) / sizeof(RULES[0]); i++) {
        const Rule *r = &RULES[i];
        if (r->vendor != d->vendor || (r->device && r->device != d->device)) continue;
        if (r->below[0] && !version_below(d->driver, r->below)) continue;
        if (r->block) {
            v.blocked = true;
            snprintf(v.why, sizeof(v.why), "%s", r->why);
            return v;
        }
        v.denied |= r->deny;
    }
    const char *deny = getenv("CYBER_GPU_DENY");
    if (deny && *deny) v.denied |= (uint32_t)atoi(deny);
    return v;
}

#ifndef EMBEDDED_SHADERS_H
#define EMBEDDED_SHADERS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const uint8_t flamegraph_vert_spv[];
extern const size_t flamegraph_vert_spv_len;

extern const uint8_t flamegraph_frag_spv[];
extern const size_t flamegraph_frag_spv_len;

#ifdef __cplusplus
}
#endif

#endif /* EMBEDDED_SHADERS_H */

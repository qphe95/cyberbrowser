#ifndef VULKAN_FONT_STUB_H
#define VULKAN_FONT_STUB_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FONT_ATLAS_SIZE 1024

/* Stub font atlas builder: fills the atlas with a neutral gray. */
bool build_ttf_atlas(uint8_t *atlas);

#ifdef __cplusplus
}
#endif

#endif /* VULKAN_FONT_STUB_H */

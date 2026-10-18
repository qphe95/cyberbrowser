#include "vulkan_font_stub.h"
#include <string.h>

bool build_ttf_atlas(uint8_t *atlas)
{
    if (!atlas) return false;
    memset(atlas, 0x80, FONT_ATLAS_SIZE * FONT_ATLAS_SIZE);
    return true;
}

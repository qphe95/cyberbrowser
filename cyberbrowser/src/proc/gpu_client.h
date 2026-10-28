#ifndef GPU_CLIENT_H
#define GPU_CLIENT_H

/* A tab's side of the GPU process (SANDBOX_DESIGN.md section 5.2): what it
 * draws goes there as display lists (gpu_wire.h), with the glyph atlas bands
 * and decoded images they name, each sent once (and again when it changes). */

#include <stdbool.h>
#include <stdint.h>
#include <windows.h>
#include "display_list.h"

/* Attach to the draw channel the coordinator gave the tab (side 0). */
bool gpu_client_attach(HANDLE section, HANDLE ev0, HANDLE ev1);

/* A new GPU process: forget what the old one had, so everything goes again. */
void gpu_client_reset(void);

/* Send the resources dl needs and dl itself (a viewport's display list) as
 * frame `frame` of width x height.  False if the channel is broken. */
bool gpu_client_send(const DisplayList *dl, uint32_t frame, uint32_t width, uint32_t height);

/* Take what the GPU process sent: 1 a frame was drawn (*frame), 0 nothing
 * (yet), -1 the channel broke. */
int gpu_client_poll(uint32_t *frame);

/* The channel's event, to wait on. */
HANDLE gpu_client_event(void);
bool gpu_client_arm(void);
void gpu_client_disarm(void);

/* Encode dl as a TG_FRAME message (gpu_wire.h) into *out (malloc'd, the
 * caller frees): its length, 0 if out of memory.  Image handles at or
 * beyond max_image are left as they are (the decoder drops them). */
uint32_t gpu_encode_frame(const DisplayList *dl, uint32_t frame, uint32_t width, uint32_t height, uint8_t **out);

#endif

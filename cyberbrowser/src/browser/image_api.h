/* HTMLImageElement loading behaviour (see image_api.c). */
#ifndef IMAGE_API_H
#define IMAGE_API_H

#include "quickjs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* naturalWidth/naturalHeight/complete/currentSrc/decode on the prototype,
 * and the Image constructor. */
void image_api_install(JSContextHandle ctx, GCValue img_proto);

/* Start fetching every connected <img> (natural size, then pixels). */
void image_api_scan(JSContextHandle ctx);

/* An image URL finished fetching (or failed): fire load/error on the
 * connected <img> elements that show it.  Main thread. */
void image_api_notify(JSContextHandle ctx, const char *url);

#ifdef __cplusplus
}
#endif

#endif

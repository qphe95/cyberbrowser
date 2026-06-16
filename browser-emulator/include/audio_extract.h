#ifndef AUDIO_EXTRACT_H
#define AUDIO_EXTRACT_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

bool audio_extract_to_fd(const char *inputPath, int outFd,
                         char *err, size_t errLen);

#ifdef __cplusplus
}
#endif

#endif

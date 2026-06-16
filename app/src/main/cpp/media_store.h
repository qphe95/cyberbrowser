#ifndef MEDIA_STORE_H
#define MEDIA_STORE_H

#include <jni.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MediaStoreHandle {
    int fd;
    jobject uri_ref;   /* JNI global ref for Uri object */
    jobject pfd_ref;   /* JNI global ref for ParcelFileDescriptor */
} MediaStoreHandle;

bool media_store_create_audio(JavaVM *vm, jobject activity,
                              const char *displayName, const char *mimeType,
                              MediaStoreHandle *outHandle,
                              char *err, size_t errLen);
bool media_store_finalize(JavaVM *vm, jobject activity,
                          MediaStoreHandle *handle,
                          char *err, size_t errLen);
void media_store_close(JavaVM *vm, MediaStoreHandle *handle);
bool media_store_get_uri_string(JavaVM *vm, MediaStoreHandle *handle,
                                char *out, size_t outLen);

#ifdef __cplusplus
}
#endif

#endif

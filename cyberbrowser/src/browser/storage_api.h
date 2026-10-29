#ifndef STORAGE_API_H
#define STORAGE_API_H

/* Persisting localStorage.  Each origin with a document in the process
 * (the page's, and its frames') has a store.  With a backend set, it is
 * loaded on first use; whoever runs the page saves those
 * storage_local_take_changes says changed.  An opaque origin's ("null")
 * is never loaded or saved. */

#include <stdbool.h>
#include <stddef.h>

typedef struct StorageBackend {
    /* origin's saved store (storage_local_take_changes' format, malloc'd);
     * false if there is none. */
    bool (*load)(const char *origin, char **data, size_t *len);
} StorageBackend;

void storage_set_backend(const StorageBackend *backend);

/* One origin whose localStorage changed since it was last taken: its
 * origin and its whole store, serialized (both malloc'd), and true; false
 * when none has. */
bool storage_local_take_changes(char **origin, char **data, size_t *len);

#endif

#ifndef STORAGE_API_H
#define STORAGE_API_H

/* Persisting localStorage.  The page's origin has one store (a process
 * holds one page).  With a backend set, it is loaded on first use; whoever
 * runs the page saves it when storage_local_take_changes says it changed. */

#include <stdbool.h>
#include <stddef.h>

typedef struct StorageBackend {
    /* The saved store (storage_local_take_changes' format, malloc'd); false
     * if there is none. */
    bool (*load)(char **data, size_t *len);
} StorageBackend;

void storage_set_backend(const StorageBackend *backend);

/* If localStorage changed since the last call: the whole store, serialized
 * (malloc'd), and true. */
bool storage_local_take_changes(char **data, size_t *len);

#endif

/*
 * import_map.h - the document's import map (HTML §8.1.5.3): the
 * <script type="importmap"> mappings that resolve module specifiers such as
 * a bare "react" to URLs.
 */
#ifndef IMPORT_MAP_H
#define IMPORT_MAP_H

#include <stddef.h>

/* Forget every mapping (a new document). */
void import_map_reset(void);

/* Merge an import map's JSON text, its URLs relative to base_url, into the
 * document's (entries already mapped keep their first mapping).  Invalid
 * JSON or a non-object top level is ignored. */
void import_map_add(const char *json, size_t len, const char *base_url);

/* Resolve specifier, imported from base_url, through the import map: a
 * malloc'd absolute URL, or NULL when no mapping applies. */
char *import_map_resolve(const char *specifier, const char *base_url);

#endif

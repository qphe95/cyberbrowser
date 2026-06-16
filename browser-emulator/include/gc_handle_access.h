/*
 * GC Handle Access - Handle-based object access utilities
 */

#ifndef GC_HANDLE_ACCESS_H
#define GC_HANDLE_ACCESS_H

#include "quickjs_gc_unified.h"
#include "quickjs.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Property lookup using handles. Returns property index or -1.
 */
extern int find_own_property_handle(GCHandle obj_handle, JSAtom atom,
                                     JSShapeProperty **pprs);

/*
 * Get property pointer by index (temporary - don't store across GC!).
 */
extern JSProperty *get_property_by_index(GCHandle obj_handle, int prop_idx);

/*
 * Check if object has a prototype.
 */
extern int gc_obj_has_prototype(GCHandle obj_handle);

/*
 * Get prototype handle of an object.
 */
extern GCHandle gc_obj_get_prototype_handle(GCHandle obj_handle);

#ifdef __cplusplus
}
#endif

#endif /* GC_HANDLE_ACCESS_H */

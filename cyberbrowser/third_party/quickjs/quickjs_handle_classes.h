/*
 * QuickJS Handle Compatibility Shim
 * ============================================================================
 *
 * The C++ handle wrapper classes that used to live in this file have been
 * removed. GC-safe access is now provided by the macro system in
 * quickjs_handles.h (SAFE_GET / SAFE_SET / SAFE_PTR / ...).
 *
 * This file is kept as an include shim so existing `#include
 * "quickjs_handle_classes.h"` directives keep working.
 * The C replacements for the old classes' non-trivial methods
 * (js_rt_atom_size(), js_obj_shape_handle(), ...) are in
 * quickjs_handle_helpers.h, included at the end of quickjs.h.
 * ============================================================================
 */

#ifndef QUICKJS_HANDLE_CLASSES_H
#define QUICKJS_HANDLE_CLASSES_H

/* quickjs-internal.h provides the struct definitions (JSPropertyEnum,
 * JSPromiseStateEnum, ...) that quickjs.h relies on; the old class header
 * pulled it in, so keep doing so. */
#include "quickjs-internal.h"
#include "quickjs_handles.h"

#endif /* QUICKJS_HANDLE_CLASSES_H */

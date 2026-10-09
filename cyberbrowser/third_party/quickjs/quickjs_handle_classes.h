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
 * ============================================================================
 */

#ifndef QUICKJS_HANDLE_CLASSES_H
#define QUICKJS_HANDLE_CLASSES_H

#include "quickjs_handles.h"

#endif /* QUICKJS_HANDLE_CLASSES_H */

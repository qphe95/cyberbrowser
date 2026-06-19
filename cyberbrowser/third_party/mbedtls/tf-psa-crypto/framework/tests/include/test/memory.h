/**
 * \file memory.h
 *
 * \brief   Helper macros and functions related to testing memory management.
 */

/*
 *  Copyright The Mbed TLS Contributors
 *  SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
 */

#ifndef TEST_MEMORY_H
#define TEST_MEMORY_H

#include "mbedtls/build_info.h"
#include "mbedtls/platform.h"
#include "test/helpers.h"

/** \def MBEDTLS_TEST_MEMORY_POISON(buf, size)
 *
 * Poison a memory area so that any attempt to read or write from it will
 * cause a runtime failure.
 *
 * This is a no-op in builds without a poisoning method.
 *
 * \param buf   Pointer to the beginning of the memory area to poison.
 * \param size  Size of the memory area in bytes.
 */

/** \def MBEDTLS_TEST_MEMORY_UNPOISON(buf, size)
 *
 * Undo the effect of #MBEDTLS_TEST_MEMORY_POISON.
 *
 * This is a no-op in builds without a poisoning method.
 *
 * \param buf   Pointer to the beginning of the memory area to unpoison.
 * \param size  Size of the memory area in bytes.
 */

/* Memory poisoning support removed - ASAN no longer supported */
#define MBEDTLS_TEST_MEMORY_POISON(ptr, size) ((void) (ptr), (void) (size))
#define MBEDTLS_TEST_MEMORY_UNPOISON(ptr, size) ((void) (ptr), (void) (size))

#endif /* TEST_MEMORY_H */

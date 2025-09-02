/*
rvvm_types.c - RVVM integer types
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef RVVM_TYPES_H
#define RVVM_TYPES_H

#include "compiler.h"

#include <stdbool.h>
#include <stddef.h>

#if CHECK_INCLUDE(inttypes.h, 1)
#include <inttypes.h>
#endif

#if defined(_WIN32) || !defined(PRIx64) || !defined(PRIu64)

/*
 * Fix awful msvcrt PRI* macros implementation
 */

#if defined(HOST_32BIT) || defined(_WIN32)
#define PRI64_PREFIX "ll"
#else
#define PRI64_PREFIX "l"
#endif

#if defined(HOST_32BIT)
#define PRIPTR_PREFIX
#else
#define PRIPTR_PREFIX PRI64_PREFIX
#endif

#undef PRId32
#undef PRIi32
#undef PRIu32
#undef PRIo32
#undef PRIx32
#undef PRIX32

#undef PRId64
#undef PRIi64
#undef PRIu64
#undef PRIo64
#undef PRIx64
#undef PRIX64

#undef PRIdPTR
#undef PRIiPTR
#undef PRIuPTR
#undef PRIoPTR
#undef PRIxPTR
#undef PRIXPTR

#define PRId32  "d"
#define PRIi32  "i"
#define PRIu32  "u"
#define PRIo32  "o"
#define PRIX32  "X"

#define PRId64  PRI64_PREFIX "d"
#define PRIi64  PRI64_PREFIX "i"
#define PRIu64  PRI64_PREFIX "u"
#define PRIo64  PRI64_PREFIX "o"
#define PRIx64  PRI64_PREFIX "x"
#define PRIX64  PRI64_PREFIX "X"

#define PRIdPTR PRIPTR_PREFIX "d"
#define PRIiPTR PRIPTR_PREFIX "i"
#define PRIuPTR PRIPTR_PREFIX "u"
#define PRIoPTR PRIPTR_PREFIX "o"
#define PRIXPTR PRIPTR_PREFIX "X"

#endif

/*
 * Provide int128_t / uint128_t types
 */

#if defined(__SIZEOF_INT128__)

typedef unsigned __int128 uint128_t;
typedef __int128          int128_t;

#define INT128_SUPPORT 1

#endif

/*
 * Provide regid_t / bitcnt_t types
 */

typedef uint8_t regid_t;
typedef uint8_t bitcnt_t;

#endif

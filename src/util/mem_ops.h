/*
mem_ops.h - Explicit memory intrinsics
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef LEKKIT_MEM_OPS_H
#define LEKKIT_MEM_OPS_H

#include <stdint.h>
#include <string.h>

#include "compiler.h"

/*
 * Simple memory operations (write, read integers) for internal usage,
 * and load/store instructions.
 */

/*
 * Handle misaligned operaions properly, to prevent
 * crashes on old ARM CPUs, etc
 */

static forceinline uint64_t read_uint64_le_m(const void* addr)
{
#if defined(HOST_LITTLE_ENDIAN)
    uint64_t val = 0;
    memcpy(&val, addr, sizeof(val));
    return val;
#else
    const uint8_t* arr = (const uint8_t*)addr;
    return ((uint64_t)arr[0])         //
         | (((uint64_t)arr[1]) << 8)  //
         | (((uint64_t)arr[2]) << 16) //
         | (((uint64_t)arr[3]) << 24) //
         | (((uint64_t)arr[4]) << 32) //
         | (((uint64_t)arr[5]) << 40) //
         | (((uint64_t)arr[6]) << 48) //
         | (((uint64_t)arr[7]) << 56);
#endif
}

static forceinline void write_uint64_le_m(void* addr, uint64_t val)
{
#if defined(HOST_LITTLE_ENDIAN)
    memcpy(addr, &val, sizeof(val));
#else
    uint8_t* arr = (uint8_t*)addr;

    arr[0] = val & 0xFF;
    arr[1] = (val >> 8) & 0xFF;
    arr[2] = (val >> 16) & 0xFF;
    arr[3] = (val >> 24) & 0xFF;
    arr[4] = (val >> 32) & 0xFF;
    arr[5] = (val >> 40) & 0xFF;
    arr[6] = (val >> 48) & 0xFF;
    arr[7] = (val >> 56) & 0xFF;
#endif
}

static forceinline uint32_t read_uint32_le_m(const void* addr)
{
#if defined(HOST_LITTLE_ENDIAN)
    uint32_t val = 0;
    memcpy(&val, addr, sizeof(val));
    return val;
#else
    const uint8_t* arr = (const uint8_t*)addr;
    return ((uint32_t)arr[0])         //
         | (((uint32_t)arr[1]) << 8)  //
         | (((uint32_t)arr[2]) << 16) //
         | (((uint32_t)arr[3]) << 24);
#endif
}

static forceinline void write_uint32_le_m(void* addr, uint32_t val)
{
#if defined(HOST_LITTLE_ENDIAN)
    memcpy(addr, &val, sizeof(val));
#else
    uint8_t* arr = (uint8_t*)addr;

    arr[0] = val & 0xFF;
    arr[1] = (val >> 8) & 0xFF;
    arr[2] = (val >> 16) & 0xFF;
    arr[3] = (val >> 24) & 0xFF;
#endif
}

static forceinline uint16_t read_uint16_le_m(const void* addr)
{
#if defined(HOST_LITTLE_ENDIAN)
    uint16_t val = 0;
    memcpy(&val, addr, sizeof(val));
    return val;
#else
    const uint8_t* arr = (const uint8_t*)addr;
    return ((uint16_t)arr[0]) | (((uint16_t)arr[1]) << 8);
#endif
}

static forceinline void write_uint16_le_m(void* addr, uint16_t val)
{
#if defined(HOST_LITTLE_ENDIAN)
    memcpy(addr, &val, sizeof(val));
#else
    uint8_t* arr = (uint8_t*)addr;

    arr[0] = val & 0xFF;
    arr[1] = (val >> 8) & 0xFF;
#endif
}

/*
 * Big-endian operations
 */

static forceinline uint64_t read_uint64_be_m(const void* addr)
{
    const uint8_t* arr = (const uint8_t*)addr;
    return ((uint64_t)arr[7])         //
         | (((uint64_t)arr[6]) << 8)  //
         | (((uint64_t)arr[5]) << 16) //
         | (((uint64_t)arr[4]) << 24) //
         | (((uint64_t)arr[3]) << 32) //
         | (((uint64_t)arr[2]) << 40) //
         | (((uint64_t)arr[1]) << 48) //
         | (((uint64_t)arr[0]) << 56);
}

static forceinline void write_uint64_be_m(void* addr, uint64_t val)
{
    uint8_t* arr = (uint8_t*)addr;

    arr[7] = val & 0xFF;
    arr[6] = (val >> 8) & 0xFF;
    arr[5] = (val >> 16) & 0xFF;
    arr[4] = (val >> 24) & 0xFF;
    arr[3] = (val >> 32) & 0xFF;
    arr[2] = (val >> 40) & 0xFF;
    arr[1] = (val >> 48) & 0xFF;
    arr[0] = (val >> 56) & 0xFF;
}

static forceinline uint32_t read_uint32_be_m(const void* addr)
{
    const uint8_t* arr = (const uint8_t*)addr;
    return ((uint32_t)arr[3])         //
         | (((uint32_t)arr[2]) << 8)  //
         | (((uint32_t)arr[1]) << 16) //
         | (((uint32_t)arr[0]) << 24);
}

static forceinline void write_uint32_be_m(void* addr, uint32_t val)
{
    uint8_t* arr = (uint8_t*)addr;

    arr[3] = val & 0xFF;
    arr[2] = (val >> 8) & 0xFF;
    arr[1] = (val >> 16) & 0xFF;
    arr[0] = (val >> 24) & 0xFF;
}

static forceinline uint16_t read_uint16_be_m(const void* addr)
{
    const uint8_t* arr = (const uint8_t*)addr;
    return ((uint16_t)arr[1]) | (((uint16_t)arr[0]) << 8);
}

static forceinline void write_uint16_be_m(void* addr, uint16_t val)
{
    uint8_t* arr = (uint8_t*)addr;

    arr[1] = val & 0xFF;
    arr[0] = (val >> 8) & 0xFF;
}

/*
 * Strictly aligned access for performace
 * Falls back to byte-bang operations on big-endian systems
 */

TSAN_SUPPRESS static forceinline uint64_t read_uint64_le(const void* addr)
{
#if defined(HOST_LITTLE_ENDIAN)
    return *(const safe_aliasing uint64_t*)addr;
#else
    return read_uint64_le_m(addr);
#endif
}

TSAN_SUPPRESS static forceinline void write_uint64_le(void* addr, uint64_t val)
{
#if defined(HOST_LITTLE_ENDIAN)
    *(safe_aliasing uint64_t*)addr = val;
#else
    write_uint64_le_m(addr, val);
#endif
}

TSAN_SUPPRESS static forceinline uint32_t read_uint32_le(const void* addr)
{
#if defined(HOST_LITTLE_ENDIAN)
    return *(const safe_aliasing uint32_t*)addr;
#else
    return read_uint32_le_m(addr);
#endif
}

TSAN_SUPPRESS static forceinline void write_uint32_le(void* addr, uint32_t val)
{
#if defined(HOST_LITTLE_ENDIAN)
    *(safe_aliasing uint32_t*)addr = val;
#else
    write_uint32_le_m(addr, val);
#endif
}

TSAN_SUPPRESS static forceinline uint16_t read_uint16_le(const void* addr)
{
#if defined(HOST_LITTLE_ENDIAN)
    return *(const safe_aliasing uint16_t*)addr;
#else
    return read_uint16_le_m(addr);
#endif
}

TSAN_SUPPRESS static forceinline void write_uint16_le(void* addr, uint16_t val)
{
#if defined(HOST_LITTLE_ENDIAN)
    *(safe_aliasing uint16_t*)addr = val;
#else
    write_uint16_le_m(addr, val);
#endif
}

TSAN_SUPPRESS static forceinline uint8_t read_uint8(const void* addr)
{
    return *(const uint8_t*)addr;
}

TSAN_SUPPRESS static forceinline void write_uint8(void* addr, uint8_t val)
{
    *(uint8_t*)addr = val;
}

#endif

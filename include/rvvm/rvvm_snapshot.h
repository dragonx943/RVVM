/*
<rvvm/rvvm_snapshot.h> - RVVM Snapshot serialization
Copyright (C) 2020-2026 LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _RVVM_SNAPSHOT_H
#define _RVVM_SNAPSHOT_H

#include <rvvm/rvvm_base.h>

RVVM_EXTERN_C_BEGIN

/**
 * @defgroup rvvm_snapshot RVVM Snapshot serialization
 * @addtogroup rvvm_snapshot
 * @{
 */

/**
 * Open snapshot for reading or writing from block device handle
 */
RVVM_PUBLIC rvvm_snapshot_t* rvvm_snapshot_open(rvvm_blk_dev_t* blk, bool write);

/**
 * Close snapshot handle, sync to backing storage after writing
 */
RVVM_PUBLIC void rvvm_snapshot_close(rvvm_snapshot_t* snap);

/**
 * Get or set failure flag on snapshot
 *
 * Failure flag means either:
 * - IO error on backing block device storage
 * - Requested section wasn't found when reading
 * - Tried to read beyond end of section
 * - Device (de)serialization manually called rvvm_snapshot_fail(snap, true)
 *
 * If failure flag is set, the restored machine state is likely incomplete.
 * However, it might still be worth a try to load partial state after
 * incompatible migration, and print a warning. The broken devices can be
 * re-hot-plugged to fix them on guest side after that.
 */
RVVM_PUBLIC bool rvvm_snapshot_fail(rvvm_snapshot_t* snap, bool fail);

/**
 * Select snapshot section (For separate device states), reset pointer
 *
 * Example: "nvme-v0.7"
 *
 * When reading: Creates new section with same name
 * When writing: Loads next section with same name
 */
RVVM_PUBLIC void rvvm_snapshot_section(rvvm_snapshot_t* snap, const char* name);

/**
 * Write opaque data to current snapshot section, advance pointer
 */
RVVM_PUBLIC void rvvm_snapshot_write(rvvm_snapshot_t* snap, const void* data, size_t size);

/**
 * Read opaque data from current snapshot section, advance pointer
 */
RVVM_PUBLIC void rvvm_snapshot_read(rvvm_snapshot_t* snap, void* data, size_t size);

/**
 * Write u8 value to current snapshot section, advance pointer
 */
static inline void rvvm_snapshot_write_u8(rvvm_snapshot_t* snap, uint8_t val)
{
    rvvm_snapshot_write(snap, &val, sizeof(val));
}

/**
 * Read u8 value from current snapshot section, advance pointer
 */
static inline uint8_t rvvm_snapshot_read_u8(rvvm_snapshot_t* snap)
{
    uint8_t buf = 0;
    rvvm_snapshot_read(snap, &buf, sizeof(buf));
    return buf;
}

/**
 * Write little-endian u16 value to current snapshot section, advance pointer
 */
static inline void rvvm_snapshot_write_u16(rvvm_snapshot_t* snap, uint16_t val)
{
    uint8_t buf[2] = {(uint8_t)val, (uint8_t)(val >> 8)};
    rvvm_snapshot_write(snap, buf, sizeof(buf));
}

/**
 * Read little-endian u16 value from current snapshot section, advance pointer
 */
static inline uint16_t rvvm_snapshot_read_u16(rvvm_snapshot_t* snap)
{
    uint8_t buf[2] = {0};
    rvvm_snapshot_read(snap, buf, sizeof(buf));
    return buf[0] | (((uint16_t)buf[1]) << 8);
}

/**
 * Write little-endian u32 value to current snapshot section, advance pointer
 */
static inline void rvvm_snapshot_write_u32(rvvm_snapshot_t* snap, uint32_t val)
{
    uint8_t buf[4] = {(uint8_t)val, (uint8_t)(val >> 8), (uint8_t)(val >> 16), (uint8_t)(val >> 24)};
    rvvm_snapshot_write(snap, buf, sizeof(buf));
}

/**
 * Read little-endian u32 value from current snapshot section, advance pointer
 */
static inline uint32_t rvvm_snapshot_read_u32(rvvm_snapshot_t* snap)
{
    uint8_t buf[4] = {0};
    rvvm_snapshot_read(snap, buf, sizeof(buf));
    return buf[0] | (((uint32_t)buf[1]) << 8) | (((uint32_t)buf[2]) << 16) | (((uint32_t)buf[3]) << 24);
}

/**
 * Write little-endian u64 value to current snapshot section, advance pointer
 */
static inline void rvvm_snapshot_write_u64(rvvm_snapshot_t* snap, uint64_t val)
{
    uint8_t buf[8] = {(uint8_t)val,         (uint8_t)(val >> 8),  (uint8_t)(val >> 16), (uint8_t)(val >> 24),
                      (uint8_t)(val >> 32), (uint8_t)(val >> 40), (uint8_t)(val >> 48), (uint8_t)(val >> 56)};
    rvvm_snapshot_write(snap, buf, sizeof(buf));
}

/**
 * Read little-endian u64 value from current snapshot section, advance pointer
 */
static inline uint64_t rvvm_snapshot_read_u64(rvvm_snapshot_t* snap)
{
    uint8_t buf[8] = {0};
    rvvm_snapshot_read(snap, buf, sizeof(buf));
    return buf[0] | (((uint64_t)buf[1]) << 8)                      /**/
         | (((uint64_t)buf[2]) << 16) | (((uint64_t)buf[3]) << 24) /**/
         | (((uint64_t)buf[4]) << 32) | (((uint64_t)buf[5]) << 40) /**/
         | (((uint64_t)buf[6]) << 48) | (((uint64_t)buf[7]) << 56);
}

/** @}*/

RVVM_EXTERN_C_END

#endif

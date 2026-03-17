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
 *
 * \param blk Block device handle
 * \param wr  True to begin writing state to snapshot
 * \return Snapshot handle
 */
RVVM_PUBLIC rvvm_snapshot_t* rvvm_snapshot_open(rvvm_blk_dev_t* blk, bool wr);

/**
 * Close snapshot handle, finalize and sync to backing storage after writing
 *
 * \param snap Snapshot handle
 */
RVVM_PUBLIC void rvvm_snapshot_close(rvvm_snapshot_t* snap);

/**
 * Get and clear failure flag on snapshot since last call
 *
 * Failure flag means either:
 * - IO error on backing block device storage
 * - Requested section wasn't found when reading
 * - Tried to read beyond end of section
 *
 * If failure flag is set, the restored state is likely incomplete
 *
 * However, it might still be worth a try to load partial state after
 * incompatible migration, and print a warning. The broken devices can be
 * re-hot-plugged to fix them on guest side after that
 *
 * \param snap Snapshot handle
 * \param fail True to set failure flag, or current failure flag is returned and cleared
 * \return Failure flag
 */
RVVM_PUBLIC bool rvvm_snapshot_fail(rvvm_snapshot_t* snap);

/**
 * Select next snapshot section, reset data pointer
 *
 * Every device state should be stored in a separate section, optionally versioned
 *
 * When writing to snapshot:
 * - Creates new section on each call, finalizes previous section
 * When reading from snapshot:
 * - Sequentially opens next section with the requested name
 * - Sets failure flag if the section is missing
 *
 * \param snap Snapshot handle
 * \param name Section name for reading/writing
 * \return True if we are writing state to snapshot
 */
RVVM_PUBLIC bool rvvm_snapshot_section(rvvm_snapshot_t* snap, const char* name);

/**
 * Read/write opaque data to current snapshot section, advance pointer
 *
 * The read/write direction is decided by current snapshot open mode
 *
 * Sets failure flag on over-reading beyond the current section
 *
 * \param snap Snapshot handle
 * \param data Opaque data pointer
 * \param size Opaque data size
 */
RVVM_PUBLIC void rvvm_snapshot_data(rvvm_snapshot_t* snap, void* data, size_t size);

/**
 * Read/write opaque field to current snapshot section, advance pointer
 *
 * \param snap  Snapshot handle
 * \param field Opaque variable field
 */
#define rvvm_snapshot_field(snap, field) rvvm_snapshot_data(snap, &(field), sizeof(field))

/** @}*/

RVVM_EXTERN_C_END

#endif

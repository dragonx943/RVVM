/*
rvvm_snapshot.c - RVVM Snapshot serialization
Copyright (C) 2020-2026 LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include <rvvm/rvvm_blk.h>
#include <rvvm/rvvm_snapshot.h>

#include "mem_ops.h"
#include "utils.h"

#define SNAP_MAGIC "\x7Frvvm-snapshot0\xFF"

PUSH_OPTIMIZATION_SIZE

struct rvvm_snapshot {
    rvvm_blk_dev_t* blk;

    uint64_t off;

    bool out;
    bool err;
};

RVVM_PUBLIC rvvm_snapshot_t* rvvm_snapshot_open(rvvm_blk_dev_t* blk, bool write)
{
    rvvm_snapshot_t* snap = safe_new_obj(rvvm_snapshot_t);

    snap->blk = blk;
    snap->out = write;

    return snap;
}

RVVM_PUBLIC bool rvvm_snapshot_close(rvvm_snapshot_t* snap)
{
    if (snap) {
        bool ret = !snap->err;
        rvvm_blk_close(snap->blk);
        free(snap);
        return ret;
    }
    return false;
}

static bool rvvm_snapshot_section_next(rvvm_snapshot_t* snap)
{
    uint8_t tmp[24] = {0};
    if (snap->out) {
        uint64_t prev = snap->off;
        snap->off     = rvvm_blk_tell_head(snap->blk);
        memcpy(tmp, SNAP_MAGIC, 16);
        write_uint64_le_m(tmp + 16, snap->off);
        if (rvvm_blk_write(snap->blk, tmp, sizeof(tmp), prev) == sizeof(tmp)) {
            write_uint64_le_m(tmp + 16, 0);
            if (rvvm_blk_write_head(snap->blk, tmp, sizeof(tmp)) == sizeof(tmp)) {
                return true;
            }
        }
    } else {
        if (rvvm_blk_read_head(snap->blk, tmp, sizeof(tmp)) == sizeof(tmp) && //
            !memcmp(tmp, SNAP_MAGIC, 16)) {
            snap->off = read_uint64_le(tmp + 16);
            return true;
        }
    }
    snap->err = true;
    return false;
}

RVVM_PUBLIC bool rvvm_snapshot_section(rvvm_snapshot_t* snap, const char* name)
{
    if (snap && name && !snap->err) {
        char   buf[256] = {0};
        size_t len      = rvvm_strlen(name);
        if (len > 255) {
            len = 255;
        }
        if (snap->out) {
            memcpy(buf, name, len);
        }
        do {
            if (!rvvm_snapshot_section_next(snap) || !rvvm_snapshot_data(snap, buf, len)) {
                return false;
            }
        } while (!snap->out && rvvm_strcmp(buf, name) == false);
        return true;
    }
    return false;
}

RVVM_PUBLIC bool rvvm_snapshot_writing(rvvm_snapshot_t* snap)
{
    if (snap) {
        return snap->out;
    }
    return false;
}

RVVM_PUBLIC bool rvvm_snapshot_data(rvvm_snapshot_t* snap, void* data, size_t size)
{
    if (snap && data && !snap->err) {
        if (snap->out) {
            if (rvvm_blk_write_head(snap->blk, data, size) != size) {
                return false;
            }
        } else {
            if (rvvm_blk_tell_head(snap->blk) + size > snap->off) {
                return false;
            }
            if (rvvm_blk_read_head(snap->blk, data, size) != size) {
                return false;
            }
        }
        return true;
    }
    return false;
}

RVVM_PUBLIC bool rvvm_snapshot_host(rvvm_snapshot_t* snap, void* data, size_t size)
{
    if (snap && data && !(((size_t)data) & (size - 1))) {
        switch (size) {
            case 1:
                return rvvm_snapshot_data(snap, data, size);
            case 2: {
                uint16_t tmp = atomic_load_uint16_relax(data);
                write_uint16_le(&tmp, tmp);
                if (!rvvm_snapshot_data(snap, &tmp, sizeof(tmp))) {
                    return false;
                } else if (!snap->out) {
                    atomic_store_uint16_relax(data, read_uint16_le(&tmp));
                }
                return true;
            }
            case 4: {
                uint32_t tmp = atomic_load_uint32_relax(data);
                write_uint32_le(&tmp, tmp);
                if (!rvvm_snapshot_data(snap, &tmp, sizeof(tmp))) {
                    return false;
                } else if (!snap->out) {
                    atomic_store_uint32_relax(data, read_uint32_le(&tmp));
                }
                return true;
            }
            case 8: {
                uint64_t tmp = atomic_load_uint64_relax(data);
                write_uint64_le(&tmp, tmp);
                if (!rvvm_snapshot_data(snap, &tmp, sizeof(tmp))) {
                    return false;
                } else if (!snap->out) {
                    atomic_store_uint64_relax(data, read_uint64_le(&tmp));
                }
                return true;
            }
        }
    }
    return false;
}

POP_OPTIMIZATION_SIZE

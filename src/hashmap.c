/*
hashmap.c - Open-addressing hashmap implementation
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "hashmap.h"
#include "bit_ops.h"
#include "compiler.h"
#include "utils.h"

#define HASHMAP_MAX_PROBES 256

static void hashmap_resize(hashmap_t* map, size_t size)
{
    hashmap_t tmp = ZERO_INIT;
    size          = bit_next_pow2(EVAL_MAX(size, 16));
    tmp.data      = safe_calloc(size, sizeof(hashmap_cell_t));
    tmp.mask      = size - 1;

    hashmap_foreach (map, k, v) {
        hashmap_put(&tmp, k, v);
    }

    hashmap_free(map);

    map->data  = tmp.data;
    map->mask  = tmp.mask;
    map->count = tmp.count;
}

static void hashmap_rebalance_internal(hashmap_t* map, size_t index)
{
    size_t j = index, k;
    while (true) {
        map->data[index].val = 0;
        do {
            j = (j + 1) & map->mask;
            if (!map->data[j].val) {
                return;
            }
            k = hashmap_hash(map->data[j].key) & map->mask;
        } while ((index <= j) ? (index < k && k <= j) : (index < k || k <= j));

        map->data[index] = map->data[j];
        index            = j;
    }
}

void hashmap_put_internal(hashmap_t* map, size_t hash, size_t key, void* val)
{
    if (likely(map->data)) {
        for (size_t i = 0; i < HASHMAP_MAX_PROBES; ++i) {
            size_t index = (hash + i) & map->mask;

            if (likely(!map->data[index].val)) {
                // Empty cell found, the key is unused
                if (likely(val)) {
                    map->data[index].key = key;
                    map->data[index].val = val;
                    map->count++;
                }
                return;
            } else if (likely(map->data[index].key == key)) {
                // The key is already used, change value
                map->data[index].val = val;
                if (!val) {
                    // Cell was erased
                    map->count--;
                    if (!map->count) {
                        // Free the emptied hashmap
                        hashmap_free(map);
                    } else if (map->count < (hashmap_capacity(map) >> 2)) {
                        // Compact the hashmap
                        hashmap_resize(map, hashmap_capacity(map) >> 1);
                    } else {
                        // Rebalance trailing cells
                        hashmap_rebalance_internal(map, index);
                    }
                }
                return;
            }
        }
    }

    // Grow the hashmap and retry inserting
    hashmap_resize(map, hashmap_capacity(map) << 1);
    hashmap_put_ptr(map, key, val);
}

void* hashmap_get_internal(const hashmap_t* map, size_t hash, size_t key)
{
    if (likely(map->data)) {
        for (size_t i = 0; i < HASHMAP_MAX_PROBES; ++i) {
            size_t index = (hash + i) & map->mask;
            if (likely(map->data[index].key == key || !map->data[index].val)) {
                return map->data[index].val;
            }
        }
    }
    return 0;
}

void hashmap_free(hashmap_t* map)
{
    if (map->data && map->mask) {
        safe_free(map->data);
    }
    map->data  = NULL;
    map->mask  = 0;
    map->count = 0;
}

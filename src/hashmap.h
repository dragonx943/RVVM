/*
hashmap.h - Open-addressing hashmap implementation
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef LEKKIT_HASHMAP_H
#define LEKKIT_HASHMAP_H

#include "compiler.h"

#include <stddef.h>
#include <stdint.h>

typedef struct randomize_layout {
    size_t key;
    void*  val;
} hashmap_cell_t;

typedef struct randomize_layout {
    hashmap_cell_t* data;
    size_t          mask;
    size_t          count;
} hashmap_t;

/*
 * Internal implementation functions
 */

void  hashmap_put_internal(hashmap_t* map, size_t hash, size_t key, void* val);
void* hashmap_get_internal(const hashmap_t* map, size_t hash, size_t key);

static forceinline size_t hashmap_hash(size_t k)
{
    k ^= k << 21;
    k ^= k >> 17;
#if (SIZE_MAX > 0xFFFFFFFF)
    k ^= k >> 35;
    k ^= k >> 51;
#endif
    return k;
}

/*
 * Hashmap interfaces
 */

void hashmap_free(hashmap_t* map);

static forceinline void hashmap_put_ptr(hashmap_t* map, size_t key, void* val)
{
    size_t hash = hashmap_hash(key);

    if (likely(map->data)) {
        size_t index = hash & map->mask;
        if (likely(!map->data[index].val)) {
            // Found empty cell
            if (likely(val)) {
                map->data[index].key = key;
                map->data[index].val = val;
                map->count++;
            }
            return;
        } else if (likely(map->data[index].key == key && val)) {
            // Found cell with matching key
            map->data[index].val = val;
            return;
        }
    }

    hashmap_put_internal(map, hash, key, val);
}

static forceinline void* hashmap_get_ptr(const hashmap_t* map, size_t key)
{
    size_t hash = hashmap_hash(key);

    if (likely(map->data)) {
        size_t index = hash & map->mask;
        if (likely(map->data[index].key == key)) {
            // Found cell with matching key
            return map->data[index].val;
        }
        if (map->data[index].val) {
            // There are non-empty trailing cells
            return hashmap_get_internal(map, hash, key);
        }
    }

    return 0;
}

static forceinline void hashmap_erase(hashmap_t* map, size_t key)
{
    // Treat NULL as empty cell
    hashmap_put_ptr(map, key, NULL);
}

static forceinline size_t hashmap_size(const hashmap_t* map)
{
    return map->count;
}

static forceinline size_t hashmap_capacity(const hashmap_t* map)
{
    return map->mask + 1;
}

static forceinline size_t hashmap_used_mem(const hashmap_t* map)
{
    return hashmap_capacity(map) * sizeof(hashmap_cell_t);
}

/*
 * Foreach loop helper
 */

#define hashmap_foreach(map, k, v)                                                                                     \
    if (likely((map)->data))                                                                                                   \
        for (size_t k, v, MACRO_IDENT(iter) = 0;                                                                       \
             (MACRO_IDENT(iter) <= (map)->mask)                                                                        \
                 ? (k = (map)->data[MACRO_IDENT(iter)].key, v = (size_t)(map)->data[MACRO_IDENT(iter)].val, 1)         \
                 : 0;                                                                                                  \
             ++MACRO_IDENT(iter), (void)k)                                                                             \
            if (v)

/*
 * Helpers & old interfaces
 */

#define hashmap_init(map, ...)                                                                                         \
    do {                                                                                                               \
        (map)->data  = NULL;                                                                                           \
        (map)->mask  = 0;                                                                                              \
        (map)->count = 0;                                                                                              \
    } while (0)

#define hashmap_clear(map)         hashmap_free(map)

#define hashmap_get(map, key)      ((size_t)hashmap_get_ptr(map, (size_t)(key)))

#define hashmap_put(map, key, val) hashmap_put_ptr(map, (size_t)(key), (void*)(val))

#define hashmap_remove(map, key)   hashmap_erase(map, (size_t)(key))

#define hashmap_destroy(map)       hashmap_free(map)

#endif

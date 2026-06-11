/*
vector.c - Vector Container
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "vector.h"
#include "compiler.h"
#include "mem_ops.h"
#include "utils.h"

SOURCE_OPTIMIZATION_SIZE

typedef vector_t(void) safe_aliasing vector_punned_t;

// Grow factor: 1.5 (Better memory reusage), initial capacity: 1
slow_path void vector_grow_internal(void* vec, size_t elem_size, size_t pos)
{
    vector_punned_t* vector   = vec;
    size_t           new_size = pos ? 2 : 1;
    if (new_size < vector->size) {
        new_size = vector->size;
    }
    while (pos >= new_size) {
        new_size += (new_size >> 1);
    }
    if (new_size > vector->size) {
        vector->data = safe_realloc(vector->data, new_size * elem_size);
        vector->size = new_size;
    }
}

static void vector_move_elem_internal(vector_punned_t* vector, size_t elem_size, size_t pos, bool erase)
{
    size_t move_count = (vector->count - pos) * elem_size;
    void*  move_from  = ((uint8_t*)vector->data) + ((pos + (erase ? 1 : 0)) * elem_size);
    void*  move_to    = ((uint8_t*)vector->data) + ((pos + (erase ? 0 : 1)) * elem_size);
    memmove(move_to, move_from, move_count);
}

void vector_emplace_internal(void* vec, size_t elem_size, size_t pos)
{
    vector_punned_t* vector    = vec;
    size_t           new_count = EVAL_MAX(vector->count, pos) + 1;
    if (unlikely(new_count >= vector->size)) {
        vector_grow_internal(vec, elem_size, new_count - 1);
    }
    if (pos < vector->count) {
        vector_move_elem_internal(vector, elem_size, pos, false);
    } else {
        void* old_end = ((uint8_t*)vector->data) + (vector->count * elem_size);
        memset(old_end, 0, (new_count - vector->count) * elem_size);
    }
    vector->count = new_count;
}

void vector_erase_internal(void* vec, size_t elem_size, size_t pos)
{
    vector_punned_t* vector = vec;
    if (pos < vector->count) {
        vector->count--;
        vector_move_elem_internal(vector, elem_size, pos, true);
    }
    if (!vector->count) {
        vector_free_internal(vec);
    } else if (vector->count < (vector->size >> 1)) {
        vector->size -= (vector->size / 3);
        vector->data  = safe_realloc(vector->data, vector->size * elem_size);
    }
}

void vector_free_internal(void* vec)
{
    vector_punned_t* vector = vec;
    if (vector->data && vector->size) {
        safe_free(vector->data);
    }
    vector->data  = NULL;
    vector->size  = 0;
    vector->count = 0;
}

void vector_copy_internal(void* vec_dst, const void* vec_src, size_t elem_size)
{
    vector_punned_t*       vector_dst = vec_dst;
    const vector_punned_t* vector_src = vec_src;
    if (vector_dst->size < vector_src->count) {
        vector_grow_internal(vector_dst, elem_size, vector_src->count - 1);
    }
    memcpy(vector_dst->data, vector_src->data, vector_src->count * elem_size);
    vector_dst->count = vector_src->count;
}

void vector_swap_internal(void* vec_a, void* vec_b)
{
    vector_punned_t tmp = ZERO_INIT;
    memcpy(&tmp, vec_a, sizeof(tmp));
    memcpy(vec_a, vec_b, sizeof(tmp));
    memcpy(vec_b, &tmp, sizeof(tmp));
}

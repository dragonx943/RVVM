/*
vector.h - Vector Container
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef LEKKIT_VECTOR_H
#define LEKKIT_VECTOR_H

#include "compiler.h"

#include <stddef.h>

/*
 * Internal implementation functions
 */

slow_path void vector_grow_internal(void* vec, size_t elem_size, size_t pos);

void vector_emplace_internal(void* vec, size_t elem_size, size_t pos);
void vector_erase_internal(void* vec, size_t elem_size, size_t pos);
void vector_free_internal(void* vec);
void vector_copy_internal(void* vec_dst, const void* vec_src, size_t elem_size);
void vector_swap_internal(void* vec_a, void* vec_b);

/**
 * vector_t() - Template-like vector type
 *
 * vector_t(int) vec_int = ZERO_INIT;
 */
#define vector_t(vec_elem_type)                                                                                        \
    struct {                                                                                                           \
        vec_elem_type* data;                                                                                           \
        size_t         size;                                                                                           \
        size_t         count;                                                                                          \
    }

/**
 * vector_init() - Initialize vector (By zeroing it's fields, may be done statically)
 */
#define vector_init(vec)                                                                                               \
    do {                                                                                                               \
        (vec).data  = NULL;                                                                                            \
        (vec).size  = 0;                                                                                               \
        (vec).count = 0;                                                                                               \
    } while (0)

/**
 * vector_at() - Access vector element at specific position
 */
#define vector_at(vec, pos)  ((vec).data[pos])

/**
 * vector_buffer() - Get vector elements buffer (Pointer to internal array)
 */
#define vector_buffer(vec)   ((vec).data)

/**
 * vector_size() - Get vector element count
 */
#define vector_size(vec)     ((vec).count)

/**
 * vector_capacity() - Get underlying buffer capacity
 */
#define vector_capacity(vec) ((vec).size)

/**
 * vector_clear() - Empty the vector
 */
#define vector_clear(vec)                                                                                              \
    do {                                                                                                               \
        (vec).count = 0;                                                                                               \
    } while (0)

/**
 * vector_free() - Empty the vector and free it's internal buffer
 *
 * May be called multiple times, the vector is empty yet reusable afterwards (Identically to vector_clear())
 * Redundant after a vector_erase() call which left the vector empty
 */
#define vector_free(vec) vector_free_internal(&(vec))

/**
 * vector_put() - Put element at specific position, resizing the vector if needed
 */
#define vector_put(vec, pos, val)                                                                                      \
    do {                                                                                                               \
        size_t MACRO_IDENT(index) = pos;                                                                               \
        if (unlikely(MACRO_IDENT(index) >= (vec).count)) {                                                             \
            vector_emplace_internal(&(vec), sizeof(*((vec).data)), MACRO_IDENT(index));                                \
        }                                                                                                              \
        (vec).data[MACRO_IDENT(index)] = val;                                                                          \
    } while (0)

/**
 * Insert new element at specific position, move trailing elements forward
 */
#define vector_insert(vec, pos, val)                                                                                   \
    do {                                                                                                               \
        size_t MACRO_IDENT(index) = pos;                                                                               \
        vector_emplace_internal(&(vec), sizeof(*((vec).data)), MACRO_IDENT(index));                                    \
        (vec).data[MACRO_IDENT(index)] = val;                                                                          \
    } while (0)

/**
 * vector_push_back() - Insert new element at the end of the vector
 */
#define vector_push_back(vec, val)                                                                                     \
    do {                                                                                                               \
        if (unlikely((vec).count >= (vec).size)) {                                                                     \
            vector_grow_internal(&(vec), sizeof(*((vec).data)), (vec).count);                                          \
        }                                                                                                              \
        (vec).data[(vec).count++] = val;                                                                               \
    } while (0)

/**
 * vector_push_front() - Insert new element at the beginning of the vector
 */
#define vector_push_front(vec, val)    vector_insert(vec, 0, val)

/**
 * vector_emplace() - Emplace new element at specific position, move trailing elements forward
 */
#define vector_emplace(vec, pos)       vector_emplace_internal(&(vec), sizeof(*((vec).data)), pos)

/**
 * vector_emplace_back() - Emplace (Construct in place) new element at the end of the vector
 */
#define vector_emplace_back(vec)       vector_emplace(vec, (vec).count)

/**
 * vector_emplace_front() - Emplace new element at the beginning of the vector
 */
#define vector_emplace_front(vec)      vector_emplace(vec, 0)

/**
 * vector_erase() - Erase element at specific posision, move trailing elements backward
 *
 * Will compact / free the vector buffer if needed
 */
#define vector_erase(vec, pos)         vector_erase_internal(&(vec), sizeof(*((vec).data)), pos)

/**
 * vector_foreach() - Iterates the vector in forward order
 * NOTE: It is prohibited to use vector_erase() inside such loops
 */
#define vector_foreach(vec, iter)      for (size_t iter = 0; iter < (vec).count; ++iter)

/**
 * vector_foreach_back() - Iterates the vector in reversed order, which is safe for vector_erase()
 */
#define vector_foreach_back(vec, iter) for (size_t iter = (vec).count; iter--;)

/**
 * vector_copy() - Copy vector contents into another one
 */
#define vector_copy(dst, src)                                                                                          \
    do {                                                                                                               \
        if ((dst).data != (src).data) {                                                                                \
            vector_copy_internal(&(dst), &(src), sizeof(*((src).data)));                                               \
        }                                                                                                              \
    } while (0)

/**
 * vector_swap() - Swap contents of two vectors
 */
#define vector_swap(a, b)                                                                                              \
    do {                                                                                                               \
        if ((a).data != (b).data) {                                                                                    \
            vector_swap_internal(&(a), &(b));                                                                          \
        }                                                                                                              \
    } while (0)

/**
 * vector_resize() - Resize the vector, zeroing any newly alocated elements
 */
#define vector_resize(vec, size)                                                                                       \
    do {                                                                                                               \
        if (unlikely((size) > (vec).count)) {                                                                          \
            vector_emplace_internal(&(vec), sizeof(*((vec).data)), (size) - 1);                                        \
        }                                                                                                              \
        (vec).count = (size);                                                                                          \
    } while (0)

/**
 * vector_insert_sorted_ex() - Insert a value into a sorted vector (Comparing manually)
 *
 * vector_insert_sorted_ex(vec_some, elem, i, vector_at(vec_some, i).cmpval, elem.cmpval);
 */
#define vector_insert_sorted_ex(vec, val, iter, cmpval1, cmpval2)                                                      \
    do {                                                                                                               \
        size_t MACRO_IDENT(low) = 0, MACRO_IDENT(high) = vector_size(vec) - 1;                                         \
        for (; (int64_t)MACRO_IDENT(low) <= (int64_t)MACRO_IDENT(high);) {                                             \
            size_t iter = MACRO_IDENT(low) + ((MACRO_IDENT(high) - MACRO_IDENT(low)) >> 1);                            \
            if ((cmpval1) < (cmpval2)) {                                                                               \
                MACRO_IDENT(low) = iter + 1;                                                                           \
            } else if ((cmpval1) > (cmpval2)) {                                                                        \
                MACRO_IDENT(high) = iter - 1;                                                                          \
            } else {                                                                                                   \
                break;                                                                                                 \
            }                                                                                                          \
        }                                                                                                              \
        vector_insert(vec, MACRO_IDENT(low), val);                                                                     \
    } while (0)

/**
 * vector_insert_sorted() - Insert a value into a sorted vector (Comparing via element field)
 *
 * vector_insert_sorted(vec_some, elem, .cmpval)
 */
#define vector_insert_sorted(vec, val, field)                                                                          \
    vector_insert_sorted_ex(vec, val, MACRO_IDENT(iter), vector_at(vec, MACRO_IDENT(iter)) field, val field)

/**
 * vector_search_sorted_ex() - Searches a sorted vector, runs the scope on each iteration (Accepts expression)
 *
 * vector_search_sorted(vec_some, i, vector_at(i).cmpval, 10) {
 *     // Will get run on each checked element while searching for value 10
 *     log("Element at %lu: %d", i, vector_at(i).cmpval);
 * }
 */
#define vector_search_sorted_ex(vec, iter, expr, cmpval)                                                               \
    for (size_t MACRO_IDENT(low) = 0, MACRO_IDENT(high) = vector_size(vec) - 1, MACRO_IDENT(flag) = 0, iter = 0; /**/  \
         ((int64_t)MACRO_IDENT(low) <= (int64_t)MACRO_IDENT(high))                                               /**/  \
             ? (                                                                                                 /**/  \
                iter = MACRO_IDENT(low) + ((MACRO_IDENT(high) - MACRO_IDENT(low)) >> 1),                         /**/  \
                ((expr) != (cmpval))                                                                             /**/  \
                    ? (                                                                                          /**/  \
                       (MACRO_IDENT(flag) = 2),                                                                  /**/  \
                       (((expr) < (cmpval) ? (MACRO_IDENT(low) = iter + 1) : (MACRO_IDENT(high) = iter - 1))))   /**/  \
                    : 0,                                                                                         /**/  \
                MACRO_IDENT(flag))                                                                               /**/  \
             : 0;                                                                                                /**/  \
         --MACRO_IDENT(flag))

/**
 * vector_search_sorted() - Searches a sorted vector, runs the scope on each iteration
 */
#define vector_search_sorted(vec, iter, field, cmpval)                                                                 \
    vector_search_sorted_ex(vec, iter, vector_at(vec, iter) field, cmpval)

/**
 * vector_find_sorted() - Searches a sorted vector, runs the scope on found element
 *
 * vector_find_sorted(vec_ints, i, vector_at(i).cmpval, 10) {
 *     // Will get run on element with field cmpval equal to 10
 *     log("Element at %lu: %d", i, vector_at(i).cmpval);
 * }
 */
#define vector_find_sorted(vec, iter, field, cmpval)                                                                   \
    vector_search_sorted (vec, iter, field, cmpval)                                                                    \
        if (MACRO_IDENT(flag) == 1)                                                                                    \
            BREAKABLE_SCOPE (found)

#endif

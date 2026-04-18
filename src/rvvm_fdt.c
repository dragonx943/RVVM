/*
rvvm_fdt.c - Flattened Device Tree Library
Copyright (C) 2021  cerg2010cerg2010 <github.com/cerg2010cerg2010>
                    LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include <rvvm/rvvm_fdt.h>

#include "mem_ops.h"
#include "spinlock.h"
#include "utils.h"
#include "vector.h"

PUSH_OPTIMIZATION_SIZE

#define FDT_MAGIC        0xD00DFEED
#define FDT_VERSION      17
#define FDT_COMP_VERSION 16

#define FDT_BEGIN_NODE   1
#define FDT_END_NODE     2
#define FDT_PROP         3
#define FDT_NOP          4
#define FDT_END          9

#define FDT_HDR_SIZE     40
#define FDT_RSV_SIZE     16

typedef struct {
    char*    name;
    void*    data;
    uint32_t size;
} rvvm_fdt_prop_t;

struct fdt_node {
    char*            name;
    rvvm_fdt_node_t* parent;
    // Used as last_phandle in root node (parent = NULL)
    uint32_t phandle;

    vector_t(rvvm_fdt_prop_t)  props;
    vector_t(rvvm_fdt_node_t*) nodes;
};

struct fdt_size_desc {
    uint32_t struct_size;
    uint32_t string_size;
};

struct fdt_serializer_ctx {
    char*    buf;
    uint32_t struct_off;
    uint32_t strings_begin;
    uint32_t strings_off;
    uint32_t reserve_off;
};

static spinlock_t fdt_lock = ZERO_INIT;

static void* heap_duplicate(const void* data, size_t size)
{
    if (data && size) {
        void* buffer = safe_malloc(size);
        memcpy(buffer, data, size);
        return buffer;
    }
    return NULL;
}

static char* str_duplicate(const char* str)
{
    return heap_duplicate(str, rvvm_strlen(str) + 1);
}

static void fdt_prop_free(rvvm_fdt_prop_t* prop)
{
    free(prop->name);
    free(prop->data);
}

static size_t fdt_name_with_addr(char* buffer, size_t size, const char* name, uint64_t addr)
{
    size_t len  = rvvm_strlcpy(buffer, name, size);
    len        += rvvm_strlcpy(buffer + len, "@", size - len);
    len        += uint_to_str_base(buffer + len, size, addr, 16);
    return len;
}

RVVM_PUBLIC rvvm_fdt_node_t* rvvm_fdt_init(const char* name)
{
    rvvm_fdt_node_t* node = safe_new_obj(rvvm_fdt_node_t);
    node->name            = name ? str_duplicate(name) : NULL;
    return node;
}

RVVM_PUBLIC rvvm_fdt_node_t* rvvm_fdt_init_reg(const char* name, uint64_t addr)
{
    char buffer[256];
    fdt_name_with_addr(buffer, sizeof(buffer), name, addr);
    return rvvm_fdt_init(buffer);
}

RVVM_PUBLIC void rvvm_fdt_attach(rvvm_fdt_node_t* parent, rvvm_fdt_node_t* child)
{
    if (child) {
        spin_lock(&fdt_lock);
        if (child->parent) {
            // Detach from previous parent node
            vector_foreach_back (child->parent->nodes, i) {
                if (vector_at(child->parent->nodes, i) == child) {
                    vector_erase(child->parent->nodes, i);
                    break;
                }
            }
        }
        if (parent) {
            // Add child FDT node
            child->parent = parent;
            vector_push_back(parent->nodes, child);
        }
        spin_unlock(&fdt_lock);
        if (!parent) {
            // Free FDT node recursively
            vector_foreach_back (child->props, i) {
                fdt_prop_free(&vector_at(child->props, i));
            }
            vector_foreach_back (child->nodes, i) {
                rvvm_fdt_free(vector_at(child->nodes, i));
            }
            vector_free(child->props);
            vector_free(child->nodes);
            free(child->name);
            free(child);
        }
    }
}

static void fdt_get_tree_size(rvvm_fdt_node_t* node, struct fdt_size_desc* desc)
{
    size_t name_len    = align_size_up(node->name ? rvvm_strlen(node->name) + 1 : 1, sizeof(uint32_t));
    desc->struct_size += sizeof(uint32_t) + name_len; // FDT_BEGIN_NODE, name

    vector_foreach (node->props, i) {
        rvvm_fdt_prop_t* prop  = &vector_at(node->props, i);
        desc->struct_size     += sizeof(uint32_t) * 3; // FDT_PROP, struct fdt_prop_desc
        desc->struct_size     += align_size_up(prop->size, sizeof(uint32_t));
        desc->string_size     += align_size_up(rvvm_strlen(prop->name) + 1, sizeof(uint32_t));
    }

    vector_foreach (node->nodes, i) {
        rvvm_fdt_node_t* child = vector_at(node->nodes, i);
        fdt_get_tree_size(child, desc);
    }

    desc->struct_size += sizeof(uint32_t); // FDT_END_NODE
}

static void fdt_serialize_u32(struct fdt_serializer_ctx* ctx, uint32_t value)
{
    write_uint32_be_m(ctx->buf + ctx->struct_off, value);
    ctx->struct_off += sizeof(uint32_t);
}

static void fdt_serialize_string(struct fdt_serializer_ctx* ctx, const char* str)
{
    if (!str) {
        str = "";
    }
    rvvm_strlcpy(ctx->buf + ctx->struct_off, str, -1);
    ctx->struct_off = align_size_up(ctx->struct_off + rvvm_strlen(str) + 1, sizeof(uint32_t));
}

static void fdt_serialize_data(struct fdt_serializer_ctx* ctx, const char* data, uint32_t len)
{
    if (len) {
        memcpy(ctx->buf + ctx->struct_off, data, len);
    }
    ctx->struct_off = align_size_up(ctx->struct_off + len, sizeof(uint32_t));
}

static void fdt_serialize_name(struct fdt_serializer_ctx* ctx, const char* str)
{
    if (!str) {
        str = "";
    }
    rvvm_strlcpy(ctx->buf + ctx->strings_off, str, -1);
    ctx->strings_off = align_size_up(ctx->strings_off + rvvm_strlen(str) + 1, sizeof(uint32_t));
}

static void fdt_serialize_tree(struct fdt_serializer_ctx* ctx, rvvm_fdt_node_t* node)
{
    fdt_serialize_u32(ctx, FDT_BEGIN_NODE);
    fdt_serialize_string(ctx, node->name);

    vector_foreach (node->props, i) {
        rvvm_fdt_prop_t* prop = &vector_at(node->props, i);
        fdt_serialize_u32(ctx, FDT_PROP);

        // struct fdt_prop_desc
        fdt_serialize_u32(ctx, prop->size);
        fdt_serialize_u32(ctx, ctx->strings_off - ctx->strings_begin);

        fdt_serialize_data(ctx, prop->data, prop->size);

        fdt_serialize_name(ctx, prop->name);
    }

    vector_foreach (node->nodes, i) {
        rvvm_fdt_node_t* child = vector_at(node->nodes, i);
        fdt_serialize_tree(ctx, child);
    }

    fdt_serialize_u32(ctx, FDT_END_NODE);
}

RVVM_PUBLIC size_t rvvm_fdt_serialize(rvvm_fdt_node_t* node, void* buff, size_t size)
{
    if (node == NULL) {
        return 0;
    }
    struct fdt_size_desc      size_desc = {0};
    struct fdt_serializer_ctx ctx       = {0};

    fdt_get_tree_size(node, &size_desc);
    size_desc.struct_size += sizeof(uint32_t); // FDT_END

    uint32_t buf_size  = FDT_HDR_SIZE + FDT_RSV_SIZE + size_desc.struct_size;
    ctx.reserve_off    = FDT_HDR_SIZE;
    ctx.struct_off     = FDT_HDR_SIZE + FDT_RSV_SIZE;
    ctx.strings_begin  = buf_size;
    ctx.strings_off    = ctx.strings_begin;
    buf_size          += size_desc.string_size;

    if (buff) {
        if (buf_size > size) {
            return 0;
        }

        memset(buff, 0, buf_size);
        ctx.buf = buff;
        write_uint32_be_m(ctx.buf, FDT_MAGIC);
        write_uint32_be_m(ctx.buf + 4, buf_size);
        write_uint32_be_m(ctx.buf + 8, ctx.struct_off);
        write_uint32_be_m(ctx.buf + 12, ctx.strings_off);
        write_uint32_be_m(ctx.buf + 16, ctx.reserve_off);
        write_uint32_be_m(ctx.buf + 20, FDT_VERSION);
        write_uint32_be_m(ctx.buf + 24, FDT_COMP_VERSION);
        write_uint32_be_m(ctx.buf + 32, size_desc.string_size);
        write_uint32_be_m(ctx.buf + 36, size_desc.struct_size);

        fdt_serialize_tree(&ctx, node);
        fdt_serialize_u32(&ctx, FDT_END);
    }

    return buf_size;
}

RVVM_PUBLIC rvvm_fdt_node_t* rvvm_fdt_find(rvvm_fdt_node_t* node, const char* name)
{
    if (node) {
        spin_lock(&fdt_lock);
        vector_foreach_back (node->nodes, i) {
            rvvm_fdt_node_t* child = vector_at(node->nodes, i);
            if (rvvm_strcmp(child->name, name)) {
                spin_unlock(&fdt_lock);
                return child;
            }
        }
        spin_unlock(&fdt_lock);
    }
    return NULL;
}

RVVM_PUBLIC rvvm_fdt_node_t* rvvm_fdt_find_reg(rvvm_fdt_node_t* node, const char* name, uint64_t addr)
{
    char buffer[256];
    fdt_name_with_addr(buffer, sizeof(buffer), name, addr);
    return rvvm_fdt_find(node, buffer);
}

RVVM_PUBLIC rvvm_fdt_node_t* rvvm_fdt_find_reg_any(rvvm_fdt_node_t* node, const char* name)
{
    char   buffer[256] = {0};
    size_t len         = rvvm_strlcpy(buffer, name, sizeof(buffer));
    rvvm_strlcpy(buffer + len, "@", sizeof(buffer) - len);
    if (node) {
        spin_lock(&fdt_lock);
        vector_foreach_back (node->nodes, i) {
            rvvm_fdt_node_t* child = vector_at(node->nodes, i);
            if (rvvm_strfind(child->name, buffer) == child->name) {
                spin_unlock(&fdt_lock);
                return child;
            }
        }
        spin_unlock(&fdt_lock);
    }
    return NULL;
}

RVVM_PUBLIC uint32_t rvvm_fdt_phandle(rvvm_fdt_node_t* node)
{
    uint32_t phandle = 0;
    if (node && node->name) {
        phandle = atomic_load_uint32_relax(&node->phandle);
        if (!phandle) {
            rvvm_fdt_node_t* root = node;
            spin_lock(&fdt_lock);
            phandle = atomic_load_uint32_relax(&node->phandle);
            if (!phandle) {
                while (root->parent) {
                    root = root->parent;
                }
                if (root->name) {
                    rvvm_warn("rvvm_fdt_phandle(%s): Invalid hierarchy", node->name);
                } else {
                    phandle = atomic_load_uint32_relax(&root->phandle) + 1;
                    atomic_store_uint32_relax(&root->phandle, phandle);
                    atomic_store_uint32_relax(&node->phandle, phandle);
                }
            }
            spin_unlock(&fdt_lock);
            rvvm_fdt_prop_set_u32(node, "phandle", node->phandle);
        }
    }
    return phandle;
}

static rvvm_fdt_prop_t* rvvm_fdt_find_prop(rvvm_fdt_node_t* node, const char* name)
{
    if (node) {
        vector_foreach (node->props, i) {
            rvvm_fdt_prop_t* prop = &vector_at(node->props, i);
            if (rvvm_strcmp(prop->name, name)) {
                return prop;
            }
        }
    }
    return NULL;
}

RVVM_PUBLIC void rvvm_fdt_prop_set(rvvm_fdt_node_t* node, const char* name, const void* data, size_t size)
{
    if (node) {
        if (node->parent) {
            spin_lock(&fdt_lock);
        }
        // Replace old prop if present
        rvvm_fdt_prop_t* old_prop = rvvm_fdt_find_prop(node, name);
        if (old_prop) {
            free(old_prop->data);
            old_prop->data = heap_duplicate(data, size);
            old_prop->size = size;
        } else {
            // Put a new prop
            rvvm_fdt_prop_t prop = {
                .name = str_duplicate(name),
                .data = heap_duplicate(data, size),
                .size = size,
            };
            vector_push_back(node->props, prop);
        }
        if (node->parent) {
            spin_unlock(&fdt_lock);
        }
    }
}

RVVM_PUBLIC void rvvm_fdt_prop_set_u32(rvvm_fdt_node_t* node, const char* name, uint32_t val)
{
    write_uint32_be_m(&val, val);
    rvvm_fdt_prop_set(node, name, &val, sizeof(val));
}

RVVM_PUBLIC void rvvm_fdt_prop_set_u64(rvvm_fdt_node_t* node, const char* name, uint64_t val)
{
    write_uint64_be_m(&val, val);
    rvvm_fdt_prop_set(node, name, &val, sizeof(val));
}

RVVM_PUBLIC void rvvm_fdt_prop_set_cells(rvvm_fdt_node_t* node, const char* name, const uint32_t* cell, size_t size)
{
    uint32_t* buffer = safe_new_arr(uint32_t, size);
    for (uint32_t i = 0; i < size; ++i) {
        write_uint32_be_m(&buffer[i], cell[i]);
    }
    rvvm_fdt_prop_set(node, name, buffer, size * sizeof(uint32_t));
    free(buffer);
}

RVVM_PUBLIC void rvvm_fdt_prop_set_str(rvvm_fdt_node_t* node, const char* name, const char* val)
{
    rvvm_fdt_prop_set(node, name, val, rvvm_strlen(val) + 1);
}

RVVM_PUBLIC void rvvm_fdt_prop_set_reg(rvvm_fdt_node_t* node, const char* name, uint64_t addr, uint64_t size)
{
    uint64_t arr[2] = {0};
    write_uint64_be_m(&arr[0], addr);
    write_uint64_be_m(&arr[1], size);
    rvvm_fdt_prop_set(node, name, arr, sizeof(arr));
}

RVVM_PUBLIC void rvvm_fdt_prop_del(rvvm_fdt_node_t* node, const char* name)
{
    if (node) {
        if (node->parent) {
            spin_lock(&fdt_lock);
        }
        vector_foreach_back (node->props, i) {
            rvvm_fdt_prop_t* prop = &vector_at(node->props, i);
            if (rvvm_strcmp(prop->name, name)) {
                fdt_prop_free(prop);
                vector_erase(node->props, i);
                break;
            }
        }
        if (node->parent) {
            spin_unlock(&fdt_lock);
        }
    }
}

RVVM_PUBLIC const void* rvvm_fdt_prop_get_data(rvvm_fdt_node_t* node, const char* name)
{
    void* data = NULL;
    if (node) {
        if (node->parent) {
            spin_lock(&fdt_lock);
        }
        rvvm_fdt_prop_t* prop = rvvm_fdt_find_prop(node, name);
        if (prop) {
            data = prop->data;
        }
        if (node->parent) {
            spin_unlock(&fdt_lock);
        }
    }
    return data;
}

RVVM_PUBLIC size_t rvvm_fdt_prop_get_size(rvvm_fdt_node_t* node, const char* name)
{
    size_t size = 0;
    if (node) {
        if (node->parent) {
            spin_lock(&fdt_lock);
        }
        rvvm_fdt_prop_t* prop = rvvm_fdt_find_prop(node, name);
        if (prop) {
            size = prop->size;
        }
        if (node->parent) {
            spin_unlock(&fdt_lock);
        }
    }
    return size;
}

POP_OPTIMIZATION_SIZE

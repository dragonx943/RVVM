/*
fdtlib.h - Flattened Device Tree Library
Copyright (C) 2021  cerg2010cerg2010 <github.com/cerg2010cerg2010>
                    LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef RVVM_FDTLIB_H
#define RVVM_FDTLIB_H

#include <rvvm/rvvm.h>
#include <rvvm/rvvm_fdt.h>

static inline struct fdt_node* fdt_node_create(const char* name)
{
    return rvvm_fdt_init(name);
}

static inline struct fdt_node* fdt_node_create_reg(const char* name, uint64_t addr)
{
    return rvvm_fdt_init_reg(name, addr);
}

static inline void fdt_node_add_child(struct fdt_node* node, struct fdt_node* child)
{
    rvvm_fdt_attach(node, child);
}

static inline struct fdt_node* fdt_node_find(struct fdt_node* node, const char* name)
{
    return rvvm_fdt_find(node, name);
}

static inline struct fdt_node* fdt_node_find_reg(struct fdt_node* node, const char* name, uint64_t addr)
{
    return rvvm_fdt_find_reg(node, name, addr);
}

static inline struct fdt_node* fdt_node_find_reg_any(struct fdt_node* node, const char* name)
{
    return rvvm_fdt_find_reg_any(node, name);
}

static inline uint32_t fdt_node_get_phandle(struct fdt_node* node)
{
    return rvvm_fdt_phandle(node);
}

static inline void fdt_node_add_prop(struct fdt_node* node, const char* name, const void* data, uint32_t len)
{
    rvvm_fdt_prop_set(node, name, data, len);
}

static inline void fdt_node_add_prop_u32(struct fdt_node* node, const char* name, uint32_t val)
{
    rvvm_fdt_prop_set_u32(node, name, val);
}

static inline void fdt_node_add_prop_u64(struct fdt_node* node, const char* name, uint64_t val)
{
    rvvm_fdt_prop_set_u64(node, name, val);
}

static inline void fdt_node_add_prop_cells(struct fdt_node* node, const char* name, uint32_t* cells, uint32_t count)
{
    rvvm_fdt_prop_set_cells(node, name, cells, count);
}

static inline void fdt_node_add_prop_str(struct fdt_node* node, const char* name, const char* val)
{
    rvvm_fdt_prop_set_str(node, name, val);
}

static inline void fdt_node_add_prop_reg(struct fdt_node* node, const char* name, uint64_t begin, uint64_t size)
{
    rvvm_fdt_prop_set_reg(node, name, begin, size);
}

static inline const void* fdt_node_get_prop_data(struct fdt_node* node, const char* name)
{
    return rvvm_fdt_prop_get_data(node, name);
}

static inline size_t fdt_node_get_prop_size(struct fdt_node* node, const char* name)
{
    return rvvm_fdt_prop_get_size(node, name);
}

static inline bool fdt_node_del_prop(struct fdt_node* node, const char* name)
{
    rvvm_fdt_prop_del(node, name);
    return true;
}

static inline void fdt_node_free(struct fdt_node* node)
{
    rvvm_fdt_free(node);
}

static inline size_t fdt_size(struct fdt_node* node)
{
    return rvvm_fdt_size(node);
}

static inline size_t fdt_serialize(struct fdt_node* node, void* buffer, size_t size, uint32_t boot_cpuid)
{
    (void)boot_cpuid;
    return rvvm_fdt_serialize(node, buffer, size);
}

#endif

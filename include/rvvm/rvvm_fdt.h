/*
<rvvm/rvvm_fdt.h> - Flattened Device Tree API
Copyright (C) 2020-2026 LekKit <github.com/LekKit>
                        cerg2010cerg2010 <github.com/cerg2010cerg2010>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _RVVM_FDT_API_H
#define _RVVM_FDT_API_H

#include <rvvm/rvvm_base.h>

RVVM_EXTERN_C_BEGIN

/**
 * @defgroup rvvm_fdt_api Flattened Device Tree API
 * @addtogroup rvvm_fdt_api
 * @{
 */

/**
 * Create FDT node
 *
 * \param name FDT node name, or NULL for a root node
 * \return     FDT node handle
 */
RVVM_PUBLIC rvvm_fdt_node_t* rvvm_fdt_init(const char* name);

/**
 * Create FDT node with hex address, like <device@10000>
 *
 * \param name FDT node device name
 * \param addr FDT node device address
 * \return     FDT node handle
 */
RVVM_PUBLIC rvvm_fdt_node_t* rvvm_fdt_init_reg(const char* name, uint64_t addr);

/**
 * Attach child FDT node
 *
 * The root node recursively owns the entire device tree
 *
 * Detaches from previous parent node, if any
 *
 * Attaching to NULL frees child node tree
 *
 * \param parent FDT parent node handle
 * \param child  FDT child node handle
 *
 * This function is thread-safe
 */
RVVM_PUBLIC void rvvm_fdt_attach(rvvm_fdt_node_t* parent, rvvm_fdt_node_t* child);

/**
 * Serialize FDT into a buffer
 *
 * If buffer is NULL, no data is written and required size is returned
 *
 * \param node FDT node handle
 * \param buff Buffer to serialize to or NULL
 * \param size Buffer size
 * \return     Serialized FDT size
 *
 * This function is thread-safe
 */
RVVM_PUBLIC size_t rvvm_fdt_serialize(rvvm_fdt_node_t* node, void* buff, size_t size);

/**
 * Get required buffer size for serialized FDT
 *
 * \param node FDT node handle
 * \return     FDT structure size
 *
 * This function is thread-safe
 */
static inline size_t rvvm_fdt_size(rvvm_fdt_node_t* node)
{
    return rvvm_fdt_serialize(node, NULL, 0);
}

/**
 * Free FDT node tree
 *
 * Detaches from parent node, if any
 *
 * \param node FDT node handle
 *
 * This function is thread-safe
 */
static inline void rvvm_fdt_free(rvvm_fdt_node_t* node)
{
    rvvm_fdt_attach(NULL, node);
}

/**
 * Look up child FDT node by name
 *
 * \param node FDT node handle
 * \param name Child FDT node name
 * \return     Child FDT node handle or NULL
 *
 * This function is thread-safe
 */
RVVM_PUBLIC rvvm_fdt_node_t* rvvm_fdt_find(rvvm_fdt_node_t* node, const char* name);

/**
 * Look up child FDT node by name with hex address, like <device@10000>
 *
 * \param node FDT node handle
 * \param name Child FDT node device name
 * \param addr Child FDT node device address
 * \return     Child FDT node handle or NULL
 *
 * This function is thread-safe
 */
RVVM_PUBLIC rvvm_fdt_node_t* rvvm_fdt_find_reg(rvvm_fdt_node_t* node, const char* name, uint64_t addr);

/**
 * Look up child FDT node by name with any address
 *
 * \param node FDT node handle
 * \param name Child FDT node device name
 * \return     Child FDT node handle or NULL
 *
 * This function is thread-safe
 */
RVVM_PUBLIC rvvm_fdt_node_t* rvvm_fdt_find_reg_any(rvvm_fdt_node_t* node, const char* name);

/**
 * Obtain FDT node phandle
 *
 * \param node FDT node handle
 * \return     FDT node phandle
 * \note       Allocates phandles, must be attached from the FDT root
 *
 * This function is thread-safe
 */
RVVM_PUBLIC uint32_t rvvm_fdt_phandle(rvvm_fdt_node_t* node);

/**
 * Set arbitrary byte data property on FDT node
 *
 * Overwrites previous property with same name
 *
 * \param node FDT node handle
 * \param name Property name
 * \param data Property data, copied internally
 * \param size Property size
 *
 * This function is thread-safe
 */
RVVM_PUBLIC void rvvm_fdt_prop_set(rvvm_fdt_node_t* node, const char* name, const void* data, size_t size);

/**
 * Set flag property without data payload on FDT node, like "dma-coherent"
 *
 * \param node FDT node handle
 * \param name Property name
 *
 * This function is thread-safe
 */
static inline void rvvm_fdt_prop_set_flag(rvvm_fdt_node_t* node, const char* name)
{
    rvvm_fdt_prop_set(node, name, NULL, 0);
}

/**
 * Set single-cell u32 property on FDT node
 *
 * \param node FDT node handle
 * \param name Property name
 * \param val  Property value
 *
 * This function is thread-safe
 */
RVVM_PUBLIC void rvvm_fdt_prop_set_u32(rvvm_fdt_node_t* node, const char* name, uint32_t val);

/**
 * Set double-cell u64 property on FDT node
 *
 * \param node FDT node handle
 * \param name Property name
 * \param val  Property value
 *
 * This function is thread-safe
 */
RVVM_PUBLIC void rvvm_fdt_prop_set_u64(rvvm_fdt_node_t* node, const char* name, uint64_t val);

/**
 * Set multi-cell property on FDT node
 *
 * \param node  FDT node handle
 * \param name  Property name
 * \param cells Property cells
 * \param count Property cells count
 *
 * This function is thread-safe
 */
RVVM_PUBLIC void rvvm_fdt_prop_set_cells(rvvm_fdt_node_t* node, const char* name, const uint32_t* cells, size_t count);

/**
 * Set string property on FDT node
 *
 * \param node FDT node handle
 * \param name Property name
 * \param str  Property string
 *
 * This function is thread-safe
 */
RVVM_PUBLIC void rvvm_fdt_prop_set_str(rvvm_fdt_node_t* node, const char* name, const char* str);

/**
 * Set region range property on FDT node (addr cells: 2, size cells: 2)
 *
 * \param node FDT node handle
 * \param name Property name
 * \param addr Property region address
 * \param size Property region size
 *
 * This function is thread-safe
 */
RVVM_PUBLIC void rvvm_fdt_prop_set_reg(rvvm_fdt_node_t* node, const char* name, uint64_t addr, uint64_t size);

/**
 * Set string list property on FDT node, like multiple compatible strings
 *
 * List must be a compile-time string literal, separated by NUL bytes (\0)
 *
 * \param node FDT node handle
 * \param name Property name
 * \param list String list literal in the form "one\0two\0three"
 *
 * This function is thread-safe
 */
#define rvvm_fdt_prop_set_str_list(node, name, list)                                                                   \
    do {                                                                                                               \
        static const char rvvm_tmp_prop_list[] = list;                                                                 \
        rvvm_fdt_prop_set(node, name, rvvm_tmp_prop_list, sizeof(rvvm_tmp_prop_list));                                 \
    } while (0)

/**
 * Delete property from FDT node
 *
 * \param node FDT node handle
 * \param name Property name
 *
 * This function is thread-safe
 */
RVVM_PUBLIC void rvvm_fdt_prop_del(rvvm_fdt_node_t* node, const char* name);

/**
 * Get FDT node property data pointer
 *
 * \param node FDT node handle
 * \param name Property name
 * \return     Property data (Valid until property is changed) or NULL
 *
 * This function is thread-safe
 */
RVVM_PUBLIC const void* rvvm_fdt_prop_get_data(rvvm_fdt_node_t* node, const char* name);

/**
 * Get FDT node property data size
 *
 * \param node FDT node handle
 * \param name Property name
 * \return     Property size
 *
 * This function is thread-safe
 */
RVVM_PUBLIC size_t rvvm_fdt_prop_get_size(rvvm_fdt_node_t* node, const char* name);

/** @}*/

RVVM_EXTERN_C_END

#endif

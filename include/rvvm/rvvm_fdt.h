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
 * Create FDT node, root node should have name = NULL
 *
 * \param name FDT node name, or NULL for a root node
 * \return FDT node handle
 */
RVVM_PUBLIC rvvm_fdt_node_t* rvvm_fdt_init(const char* name);

/**
 * Create FDT node with address, like <device@10000>
 *
 * \param name FDT node device name
 * \param addr FDT node device address
 * \return FDT node handle
 */
RVVM_PUBLIC rvvm_fdt_node_t* rvvm_fdt_init_reg(const char* name, uint64_t addr);

/**
 * Add child node to a FDT node
 *
 * The root FDT node recursively owns the entire device tree
 * If attach fails (Parent node is NULL), child node is freed
 *
 * \param node  Parent FDT node handle
 * \param child Child FDT node handle
 */
RVVM_PUBLIC void rvvm_fdt_node_add(rvvm_fdt_node_t* node, rvvm_fdt_node_t* child);

/**
 * Serialize FDT into a buffer
 *
 * If buffer is NULL, no data is written
 * If buffer is too small, zero is returned
 *
 * \param node FDT node handle
 * \param buff Buffer to serialize to, or NULL
 * \param size Buffer size
 * \return FDT structure size
 */
RVVM_PUBLIC size_t rvvm_fdt_serialize(rvvm_fdt_node_t* node, void* buff, size_t size);

/**
 * Get required buffer size for serialized FDT
 *
 * \param node FDT node handle
 * \return FDT structure size
 */
static inline size_t rvvm_fdt_size(rvvm_fdt_node_t* node)
{
    return rvvm_fdt_serialize(node, NULL, 0);
}

/**
 * Free FDT node and its child nodes recursively
 *
 * Detaches from parent node, if any
 *
 * \param node FDT node handle
 */
RVVM_PUBLIC void rvvm_fdt_free(rvvm_fdt_node_t* node);

/**
 * Look up for child node by name
 *
 * \param node FDT node handle
 * \param name Child FDT node name
 * \return Child FDT node handle or NULL
 */
RVVM_PUBLIC rvvm_fdt_node_t* rvvm_fdt_find(rvvm_fdt_node_t* node, const char* name);

/**
 * Look up for child node by name with address, like <device@10000>
 *
 * \param node FDT node handle
 * \param name Child FDT node device name
 * \param addr Child FDT node device address
 * \return Child FDT node handle or NULL
 */
RVVM_PUBLIC rvvm_fdt_node_t* rvvm_fdt_find_reg(rvvm_fdt_node_t* node, const char* name, uint64_t addr);

/**
 * Look up for child node by name with any address
 *
 * \param node FDT node handle
 * \param name Child FDT node device name
 * \return Child FDT node handle or NULL
 */
RVVM_PUBLIC rvvm_fdt_node_t* rvvm_fdt_find_reg_any(rvvm_fdt_node_t* node, const char* name);

/**
 * Get FDT node phandle
 *
 * Performs automatic phandle allocation
 * Node must be attached all the way from the root
 *
 * \param node FDT node handle
 * \return FDT node phandle
 */
RVVM_PUBLIC uint32_t rvvm_fdt_get_phandle(rvvm_fdt_node_t* node);

/**
 * Set arbitrary byte buffer property in FDT node
 *
 * Overwrites previous property with same name
 *
 * \param node FDT node handle
 * \param name Property name
 * \param data Property data
 * \param size Property size
 */
RVVM_PUBLIC void rvvm_fdt_prop_set(rvvm_fdt_node_t* node, const char* name, const void* data, size_t size);

/**
 * Set single-cell u32 property in FDT node
 *
 * \param node FDT node handle
 * \param name Property name
 * \param val  Property value
 */
RVVM_PUBLIC void rvvm_fdt_prop_set_u32(rvvm_fdt_node_t* node, const char* name, uint32_t val);

/**
 * Set double-cell u64 property in FDT node
 *
 * \param node FDT node handle
 * \param name Property name
 * \param val  Property value
 */
RVVM_PUBLIC void rvvm_fdt_prop_set_u64(rvvm_fdt_node_t* node, const char* name, uint64_t val);

/**
 * Set multi-cell property in FDT node
 *
 * \param node FDT node handle
 * \param name Property name
 * \param cell Property cells
 * \param size Property cells count
 */
RVVM_PUBLIC void rvvm_fdt_prop_set_cells(rvvm_fdt_node_t* node, const char* name, const uint32_t* cell, size_t size);

/**
 * Set string property in FDT node
 *
 * \param node FDT node handle
 * \param name Property name
 * \param str  Property string
 */
RVVM_PUBLIC void rvvm_fdt_prop_set_str(rvvm_fdt_node_t* node, const char* name, const char* str);

/**
 * Set region range property in FDT node (addr cells: 2, size cells: 2)
 *
 * \param node FDT node handle
 * \param name Property name
 * \param addr Property region address
 * \param size Property region size
 */
RVVM_PUBLIC void rvvm_fdt_prop_set_reg(rvvm_fdt_node_t* node, const char* name, uint64_t addr, uint64_t size);

/**
 * Delete property from FDT node
 *
 * \param node FDT node handle
 * \param name Property name
 */
RVVM_PUBLIC void rvvm_fdt_prop_del(rvvm_fdt_node_t* node, const char* name);

/**
 * Get FDT node property data pointer
 *
 * \param node FDT node handle
 * \param name Property name
 * \return FDT property data or NULL
 */
RVVM_PUBLIC const void* rvvm_fdt_prop_get_data(rvvm_fdt_node_t* node, const char* name);

/**
 * Get FDT node property data size
 *
 * \param node FDT node handle
 * \param name Property name
 * \return FDT property size
 */
RVVM_PUBLIC size_t rvvm_fdt_prop_get_size(rvvm_fdt_node_t* node, const char* name);

/** @}*/

RVVM_EXTERN_C_END

#endif

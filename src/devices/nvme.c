/*
nvme.c - Non-Volatile Memory Express
Copyright (C) 2022  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "nvme.h"

#include "atomics.h"
#include "bit_ops.h"
#include "mem_ops.h"
#include "rvtimer.h"
#include "threading.h"
#include "utils.h"

PUSH_OPTIMIZATION_SIZE

/*
 * NVMe Controller Registers
 */

#define NVME_REG_CAP1         0x00 // Controller Capabilities
#define NVME_REG_CAP2         0x04 // Controller Capabilities
#define NVME_REG_VS           0x08 // Version
#define NVME_REG_INTMS        0x0C // Interrupt Mask Set
#define NVME_REG_INTMC        0x10 // Interrupt Mask Clear
#define NVME_REG_CC           0x14 // Controller Configuration
#define NVME_REG_CSTS         0x1C // Controller Status
#define NVME_REG_AQA          0x24 // Admin Queue Attributes
#define NVME_REG_ASQ1         0x28 // Admin Submission Queue Base Address
#define NVME_REG_ASQ2         0x2C
#define NVME_REG_ACQ1         0x30 // Admin Completion Queue Base Address
#define NVME_REG_ACQ2         0x34

/*
 * NVMe Register constants
 */

#define NVME_CAP1_MQES        0x0000FFFF // Maximum Queue Entries Supported: 65536
#define NVME_CAP1_CQR         0x00010000 // Contiguous Queues Required
#define NVME_CAP1_TO          0xFF000000 // Timeout: Max

#define NVME_CAP2_DSTRD       0x00000000 // Doorbell Stride (0 means 2-bit shift)
#define NVME_CAP2_CSS         0x00000020 // Command Sets Supported (NVM Command Set)

#define NVME_VS_VERSION       0x00010400 // NVMe v1.4

#define NVME_CC_EN            0x00000001 // Enabled
#define NVME_CC_SHN           0x0000C000 // Shutdown Notification
#define NVME_CC_IOQES         0x00460000 // IO Queue Entry Sizes (16b:64b)

#define NVME_CSTS_RDY         0x00000001 // Ready
#define NVME_CSTS_SHST        0x00000008 // Shutdown complete

/*
 * NVMe Queue IDs
 */
#define NVME_ADMIN_SQ         0x00 // Admin Submission Queue
#define NVME_ADMIN_CQ         0x01 // Admin Completion Queue

/*
 * NVMe Admin Command Set
 */
#define NVME_DELETE_IO_SQ     0x00 // Delete IO Submission Queue
#define NVME_CREATE_IO_SQ     0x01 // Create IO Submission Queue
#define NVME_DELETE_IO_CQ     0x04 // Delete IO Completion Queue
#define NVME_CREATE_IO_CQ     0x05 // Create IO Completion Queue
#define NVME_IDENTIFY         0x06 // Identify
#define NVME_ABORT            0x08 // Abort Command
#define NVME_SET_FEATURE      0x09 // Set Features
#define NVME_GET_FEATURE      0x0A // Get Features

/*
 * Create IO Completion Queue Dword 11 bits
 */
#define NVME_CQ_FLAGS_PC      0x01 // Physically Contiguous
#define NVME_CQ_FLAGS_IEN     0x02 // Interrupts Enabled

/*
 * NVMe Identify command fields
 */
#define NVME_IDENT_NS         0x00 // Identify Namespace
#define NVME_IDENT_CTRL       0x01 // Identify Controller
#define NVME_IDENT_NSLS       0x02 // Identify Namespace List
#define NVME_IDENT_NIDS       0x03 // Identify Namespace Descriptors

/*
 * NVMe Set Feature command fields
 */
#define NVME_FEAT_ARBITRATION 0x01 // Arbitration
#define NVME_FEAT_POWER_MGMT  0x02 // Power Management
#define NVME_FEAT_TEMP_THRESH 0x04 // Temperature Threshold
#define NVME_FEAT_ERROR_RECOV 0x05 // Error Recovery
#define NVME_FEAT_VOLATILE_WC 0x06 // Volatile Write Cache
#define NVME_FEAT_NUM_QUEUES  0x07 // Number of Queues
#define NVME_FEAT_IRQ_COALESC 0x08 // Interrupt Coalescing
#define NVME_FEAT_IRQ_VECTOR  0x09 // Interrupt Vector Configuration
#define NVME_FEAT_WR_ATOMIC   0x0A // Write Atomicity Normal
#define NVME_FEAT_ASYNC_EVENT 0x0B // Asynchronous Event Configuration

/*
 * NVMe IO Command Set
 */
#define NVME_IO_FLUSH         0x00 // Flush buffers
#define NVME_IO_WRITE         0x01 // Write
#define NVME_IO_READ          0x02 // Read
#define NVME_IO_WRITEZ        0x08 // Write Zeroes
#define NVME_IO_DTSM          0x09 // Dataset Management

/*
 * NVMe Completion Queue Status Codes
 */
#define NVME_SC_SUCCESS       0x00 // Successful Completion
#define NVME_SC_BAD_OPCODE    0x01 // Invalid Command Opcode
#define NVME_SC_BAD_FIELD     0x02 // Invalid Field in Command
#define NVME_SC_DATA_ERR      0x04 // Data Transfer Error
#define NVME_SC_ABORT         0x07 // Command Abort Requested
#define NVME_SC_SQ_DELETED    0x08 // Command Aborted due to SQ Deletion
#define NVME_SC_FEAT_NSAVE    0x0D // Feature Identifier Not Saveable
#define NVME_SC_FEAT_NCHG     0x0E // Feature Identifier Not Changeable
#define NVME_SC_LBA_RANGE     0x80 // LBA Out of Range

/*
 * NVMe Command Specific Status
 */
#define NVME_CS_CQ_INVALID    0x00 // Completion Queue Invalid
#define NVME_CS_ID_INVALID    0x01 // Invalid Queue Identifier
#define NVME_CS_SIZE_INVALID  0x02 // Invalid Queue Size

/*
 * RVVM NVMe Implementation constants
 */
#define NVME_LBAS             0x09 // LBA Block Size Shift (512b blocks)
#define NVME_MAX_QUEUES       0x10 // Max IO Queues (16)
#define NVME_FEAT_NQES        ((NVME_MAX_QUEUES - 1) | ((NVME_MAX_QUEUES - 1) << 16))

#define NVME_PAGE_SIZE        0x1000ULL
#define NVME_PAGE_MASK        0xFFFULL
#define NVME_PRP2_END         0xFF8ULL

// IO + Admin SQ/CQ
#define NVME_NQUEUES          ((NVME_MAX_QUEUES + 1) << 1)

typedef struct {
    uint32_t addr_l;
    uint32_t addr_h;
    uint32_t size;
    uint32_t head;
    uint32_t tail;

    // Submission queue data
    uint32_t cq_id;

    // Completion queue data
    uint32_t irq;
} nvme_queue_t;

typedef struct {
    pci_func_t* pci_func;

    rvvm_blk_dev_t* blk;

    uint32_t     threads;
    uint32_t     conf;
    uint32_t     irq_mask;
    char         serial[12];
    nvme_queue_t queues[NVME_NQUEUES];
} nvme_dev_t;

typedef struct {
    rvvm_addr_t prp1;
    rvvm_addr_t prp2;
    uint8_t*    prp2_dma;
    size_t      prp2_off;
    size_t      size;
    size_t      cur;
} nvme_prp_ctx_t;

typedef struct {
    const uint8_t* ptr;
    nvme_prp_ctx_t prp;

    // Command submission information
    uint32_t cmd_id;
    uint32_t sq_head_id;
    uint32_t cq_id;

    // Command specific status
    uint32_t cs;
} nvme_cmd_t;

// Helper for head/tail manipulation on dequeue/enqueue
static inline uint32_t nvme_queue_next(uint32_t* head_or_tail, uint32_t queue_size)
{
    uint32_t entry = atomic_load_uint32(head_or_tail);
    uint32_t next  = 0;
    do {
        if (entry >= queue_size) {
            next = 0;
        } else {
            next = entry + 1;
        }
    } while (!atomic_cas_uint32_loop(head_or_tail, &entry, next));
    return entry;
}

static void nvme_queue_raise_irq(nvme_dev_t* nvme, nvme_queue_t* queue)
{
    uint32_t irq_reg = atomic_load_uint32_relax(&queue->irq);
    uint32_t irq_vec = irq_reg >> 16;
    if ((irq_reg & NVME_CQ_FLAGS_IEN) && !(atomic_load_uint32_relax(&nvme->irq_mask) & bit_set32(irq_vec))) {
        pci_raise_irq(nvme->pci_func, irq_vec);
    }
}

static void nvme_queue_lower_irq(nvme_dev_t* nvme, nvme_queue_t* queue)
{
    uint32_t irq_vec = atomic_load_uint32_relax(&queue->irq) >> 16;
    pci_lower_irq(nvme->pci_func, irq_vec);
}

static inline rvvm_addr_t nvme_queue_addr(const nvme_queue_t* queue)
{
    uint32_t low  = atomic_load_uint32_relax(&queue->addr_l);
    uint32_t high = atomic_load_uint32_relax(&queue->addr_h);
    return low | (((uint64_t)high) << 32);
}

static void nvme_queue_setup(nvme_dev_t* nvme, nvme_queue_t* queue, rvvm_addr_t addr, uint32_t size)
{
    atomic_store_uint32(&queue->head, 0);
    atomic_store_uint32(&queue->tail, 0);
    atomic_store_uint32(&queue->addr_l, addr & ~NVME_PAGE_MASK);
    atomic_store_uint32(&queue->addr_h, addr >> 32);
    atomic_store_uint32(&queue->size, size);
    nvme_queue_lower_irq(nvme, queue);
}

static void nvme_reset(nvme_dev_t* nvme)
{
    // Wait for IO workers to halt
    while (atomic_load_uint32(&nvme->threads)) {
        sleep_ms(1);
    }
    // Reset queues
    for (size_t i = 0; i < STATIC_ARRAY_SIZE(nvme->queues); ++i) {
        uint64_t addr = 0;
        uint32_t size = 0;
        if (i == NVME_ADMIN_SQ || i == NVME_ADMIN_CQ) {
            // Keep addr/size intact for Admin queues
            addr = nvme_queue_addr(&nvme->queues[i]);
            size = atomic_load_uint32_relax(&nvme->queues[i].size);
        }
        nvme_queue_setup(nvme, &nvme->queues[i], addr, size);
    }
}

static void nvme_remove(rvvm_mmio_dev_t* dev)
{
    nvme_dev_t* nvme = dev->data;
    nvme_reset(nvme);
    rvvm_blk_close(nvme->blk);
    free(nvme);
}

static rvvm_mmio_type_t nvme_type = {
    .name   = "nvme",
    .remove = nvme_remove,
};

static void nvme_complete_cmd(nvme_dev_t* nvme, nvme_cmd_t* cmd, uint32_t status)
{
    nvme_queue_t* queue      = &nvme->queues[(cmd->cq_id << 1) + 1];
    uint32_t      queue_size = atomic_load_uint32_relax(&queue->size);
    uint32_t      queue_tail = nvme_queue_next(&queue->tail, queue_size);
    rvvm_addr_t   addr       = nvme_queue_addr(queue) + (queue_tail << 4);

    uint8_t* ptr = pci_get_dma_ptr(nvme->pci_func, addr, 16);
    if (likely(ptr)) {
        uint32_t phase = (~read_uint32_le(ptr + 12)) & 0x10000;
        write_uint32_le(ptr, cmd->cs);             // Command Specific 1
        write_uint32_le(ptr + 4, 0);               // Command Specific 2
        write_uint32_le(ptr + 8, cmd->sq_head_id); // SQ Head Pointer & Identifier

        // Command Identifier, Phase Bit, Status Field
        atomic_store_uint32_le(ptr + 12, cmd->cmd_id | (status << 17) | phase);
    }
    nvme_queue_raise_irq(nvme, queue);
}

static inline void nvme_complete_cmd_cs(nvme_dev_t* nvme, nvme_cmd_t* cmd, uint32_t status, uint32_t cs)
{
    cmd->cs = cs;
    nvme_complete_cmd(nvme, cmd, status);
}

static size_t nvme_process_prp_chunk(nvme_dev_t* nvme, nvme_cmd_t* cmd)
{
    nvme_prp_ctx_t* prp  = &cmd->prp;
    rvvm_addr_t     addr = prp->prp1;
    size_t          len  = NVME_PAGE_SIZE;

    if (prp->cur >= prp->size) {
        // End of transfer
        return 0;
    }

    if (prp->cur == 0) {
        // Consume the first page, may be misaligned
        len = NVME_PAGE_SIZE - (prp->prp1 & NVME_PAGE_MASK);
        if (len < prp->size && prp->size <= NVME_PAGE_SIZE + len) {
            // PRP2 encodes second page address directly
            prp->prp1 = prp->prp2;
            if (prp->prp1 == addr + len) {
                len += NVME_PAGE_SIZE;
            }
            if (len >= prp->size) {
                len = prp->size;
            }
            prp->cur = len;
            return len;
        }
        if (len >= prp->size) {
            prp->cur = prp->size;
            return prp->size;
        }
    }

    while ((prp->cur + len) < prp->size) {
        // Process PRP2 entries until we reach end of transfer
        if (prp->prp2_dma == NULL) {
            prp->prp2_dma = pci_get_dma_ptr(nvme->pci_func, prp->prp2, NVME_PAGE_SIZE);
            if (!prp->prp2_dma) {
                // PRP list DMA error
                nvme_complete_cmd(nvme, cmd, NVME_SC_DATA_ERR);
                return 0;
            }
        }
        if (prp->prp2_off >= NVME_PRP2_END) {
            // Fetch next PRP list in the chain
            prp->prp2     = read_uint64_le_m(prp->prp2_dma + NVME_PRP2_END);
            prp->prp2_off = 0;
            prp->prp2_dma = NULL;
        } else {
            // Process next PRP list entry
            prp->prp1      = read_uint64_le_m(prp->prp2_dma + prp->prp2_off);
            prp->prp2_off += 8;
        }

        // Non-continuous page, split the chunk
        if (prp->prp1 != (addr + len)) {
            break;
        }
        len += NVME_PAGE_SIZE;
    }

    if ((prp->cur + len) > prp->size) {
        // Fixup length overrun
        len = prp->size - prp->cur;
    }

    prp->cur += len;
    return len;
}

static void* nvme_get_prp_chunk(nvme_dev_t* nvme, nvme_cmd_t* cmd, size_t* size)
{
    rvvm_addr_t addr = cmd->prp.prp1;
    *size            = nvme_process_prp_chunk(nvme, cmd);
    if (*size == 0) {
        return NULL;
    }
    void* ret = pci_get_dma_ptr(nvme->pci_func, addr, *size);
    if (unlikely(!ret)) {
        nvme_complete_cmd(nvme, cmd, NVME_SC_DATA_ERR);
    }
    return ret;
}

static bool nvme_write_prp(nvme_dev_t* nvme, nvme_cmd_t* cmd, const void* data, size_t size)
{
    const uint8_t* src = data;
    uint8_t*       dest;
    size_t         tmp_size;
    cmd->prp.size = size;
    while (cmd->prp.cur < cmd->prp.size) {
        dest = nvme_get_prp_chunk(nvme, cmd, &tmp_size);
        if (!dest) {
            return false;
        }
        memcpy(dest, src, tmp_size);
        src += tmp_size;
    }
    return true;
}

static void nvme_identify(nvme_dev_t* nvme, nvme_cmd_t* cmd)
{
    uint8_t* ptr = safe_new_arr(uint8_t, NVME_PAGE_SIZE);
    switch (cmd->ptr[40]) {
        case NVME_IDENT_NS: {
            uint64_t lbas = rvvm_blk_get_size(nvme->blk) >> NVME_LBAS;
            write_uint64_le(ptr, lbas);
            write_uint64_le(ptr + 8, lbas);
            write_uint64_le(ptr + 16, lbas);
            // Supports Deallocate bit in Write Zeros
            ptr[33]  = 0x8;
            ptr[130] = NVME_LBAS;
            break;
        }
        case NVME_IDENT_CTRL: {
            // PCI Vendor ID
            write_uint16_le(ptr, 0x1F31);
            write_uint16_le(ptr + 2, 0x1F31);
            // Serial Number
            memcpy(ptr + 4, nvme->serial, sizeof(nvme->serial));
            // Model Number
            rvvm_strlcpy((char*)ptr + 24, "NVMe Storage", 40);
            // Firmware Revision
            rvvm_strlcpy((char*)ptr + 64, "R1905", 8);
            // Version
            write_uint32_le(ptr + 80, NVME_VS_VERSION);
            // Controller Type: I/O Controller
            ptr[111] = 1;
            // Submission Queue Max/Cur Entry Size
            ptr[512] = 0x66;
            // Completion Queue Max/Cur Entry Size
            ptr[513] = 0x44;
            // Number of Namespaces
            ptr[516] = 1;
            // Supports Write Zeroes, Dataset Management
            ptr[520] = 0xC;
            // NVMe Qualified Name (Includes serial to distinguish targets)
            size_t nqn_off = rvvm_strlcpy((char*)ptr + 768, "nqn.2022-04.lekkit:nvme:", 256);
            memcpy(ptr + 768 + nqn_off, nvme->serial, sizeof(nvme->serial));
            break;
        }
        case NVME_IDENT_NSLS:
            // Namespace #1
            write_uint32_le(ptr, 0x1);
            break;
        case NVME_IDENT_NIDS:
            // Namespace uses UUID
            ptr[0] = 3;
            // UUID length
            ptr[1] = 16;
            break;
        default:
            nvme_complete_cmd(nvme, cmd, NVME_SC_BAD_FIELD);
            free(ptr);
            return;
    }
    if (nvme_write_prp(nvme, cmd, ptr, NVME_PAGE_SIZE)) {
        nvme_complete_cmd(nvme, cmd, NVME_SC_SUCCESS);
    }
    free(ptr);
}

static void nvme_create_io_sq(nvme_dev_t* nvme, nvme_cmd_t* cmd)
{
    rvvm_addr_t sq_addr = cmd->prp.prp1;
    uint32_t    sq_id   = read_uint16_le(cmd->ptr + 40);
    uint32_t    sq_size = read_uint16_le(cmd->ptr + 42);
    uint32_t    cq_flag = read_uint32_le(cmd->ptr + 44);
    uint32_t    cq_id   = cq_flag >> 16;
    if (!sq_id || sq_id > NVME_MAX_QUEUES) {
        // Queue ID invalid
        nvme_complete_cmd_cs(nvme, cmd, NVME_SC_BAD_FIELD, NVME_CS_ID_INVALID);
    } else if (!cq_id || cq_id > NVME_MAX_QUEUES) {
        // Completion queue invalid
        nvme_complete_cmd_cs(nvme, cmd, NVME_SC_BAD_FIELD, NVME_CS_CQ_INVALID);
    } else if (!sq_size) {
        // Queue size invalid
        nvme_complete_cmd_cs(nvme, cmd, NVME_SC_BAD_FIELD, NVME_CS_SIZE_INVALID);
    } else if (!(cq_flag & NVME_CQ_FLAGS_PC)) {
        // Non-contiguous queue
        nvme_complete_cmd(nvme, cmd, NVME_SC_BAD_FIELD);
    } else {
        nvme_queue_t* queue = &nvme->queues[(sq_id << 1)];
        nvme_queue_setup(nvme, queue, sq_addr, sq_size);
        atomic_store_uint32_relax(&queue->cq_id, cq_id);
        nvme_complete_cmd(nvme, cmd, NVME_SC_SUCCESS);
    }
}

static void nvme_create_io_cq(nvme_dev_t* nvme, nvme_cmd_t* cmd)
{
    rvvm_addr_t cq_addr = cmd->prp.prp1;
    uint32_t    cq_id   = read_uint16_le(cmd->ptr + 40);
    uint32_t    cq_size = read_uint16_le(cmd->ptr + 42);
    uint32_t    cq_flag = read_uint32_le(cmd->ptr + 44);
    if (!cq_id || cq_id > NVME_MAX_QUEUES) {
        // Queue ID invalid
        nvme_complete_cmd_cs(nvme, cmd, NVME_SC_BAD_FIELD, NVME_CS_ID_INVALID);
    } else if (!cq_size) {
        // Queue size invalid
        nvme_complete_cmd_cs(nvme, cmd, NVME_SC_BAD_FIELD, NVME_CS_SIZE_INVALID);
    } else if (!(cq_flag & NVME_CQ_FLAGS_PC)) {
        // Non-contiguous queue
        nvme_complete_cmd(nvme, cmd, NVME_SC_BAD_FIELD);
    } else {
        nvme_queue_t* queue = &nvme->queues[(cq_id << 1) + 1];
        nvme_queue_setup(nvme, queue, cq_addr, cq_size);
        atomic_store_uint32_relax(&queue->irq, cq_flag);
        nvme_complete_cmd(nvme, cmd, NVME_SC_SUCCESS);
    }
}

static void nvme_delete_io_queue(nvme_dev_t* nvme, nvme_cmd_t* cmd, bool is_cq)
{
    size_t q_id = read_uint16_le(cmd->ptr + 40);
    if (!q_id || q_id > NVME_MAX_QUEUES) {
        // Queue ID invalid
        nvme_complete_cmd_cs(nvme, cmd, NVME_SC_BAD_FIELD, NVME_CS_ID_INVALID);
    } else {
        nvme_queue_setup(nvme, &nvme->queues[(q_id << 1) + is_cq], 0, 0);
        nvme_complete_cmd(nvme, cmd, NVME_SC_SUCCESS);
    }
}

static void nvme_admin_cmd(nvme_dev_t* nvme, nvme_cmd_t* cmd)
{
    uint8_t opcode = cmd->ptr[0];
    switch (opcode) {
        case NVME_IDENTIFY:
            nvme_identify(nvme, cmd);
            return;
        case NVME_CREATE_IO_SQ:
            nvme_create_io_sq(nvme, cmd);
            return;
        case NVME_CREATE_IO_CQ:
            nvme_create_io_cq(nvme, cmd);
            return;
        case NVME_DELETE_IO_SQ:
        case NVME_DELETE_IO_CQ:
            nvme_delete_io_queue(nvme, cmd, opcode == NVME_DELETE_IO_CQ);
            return;
        case NVME_SET_FEATURE:
        case NVME_GET_FEATURE:
            switch (cmd->ptr[40]) {
                case NVME_FEAT_NUM_QUEUES:
                    nvme_complete_cmd_cs(nvme, cmd, NVME_SC_SUCCESS, NVME_FEAT_NQES);
                    return;
                case NVME_FEAT_TEMP_THRESH:
                case NVME_FEAT_WR_ATOMIC:
                case NVME_FEAT_IRQ_COALESC:
                case NVME_FEAT_IRQ_VECTOR:
                    nvme_complete_cmd_cs(nvme, cmd, NVME_SC_SUCCESS, 0);
                    return;
                default:
                    // TODO: What error should be reported here?
                    nvme_complete_cmd(nvme, cmd, NVME_SC_BAD_FIELD);
                    return;
            }
            return;
        case NVME_ABORT:
            // Ignore
            nvme_complete_cmd(nvme, cmd, NVME_SC_SUCCESS);
            return;
        default:
            rvvm_debug("Unknown NVMe admin cmd %02x", opcode);
            nvme_complete_cmd(nvme, cmd, NVME_SC_BAD_OPCODE);
            return;
    }
}

static void nvme_io_cmd(nvme_dev_t* nvme, nvme_cmd_t* cmd)
{
    uint8_t  opcode = cmd->ptr[0];
    uint64_t pos    = read_uint64_le(cmd->ptr + 40) << NVME_LBAS;
    uint8_t* buffer;
    size_t   size, tmp;

    switch (opcode) {
        case NVME_IO_READ:
        case NVME_IO_WRITE:
            while (cmd->prp.cur < cmd->prp.size) {
                buffer = nvme_get_prp_chunk(nvme, cmd, &size);
                if (buffer == NULL) {
                    return;
                }
                if (opcode == NVME_IO_WRITE) {
                    tmp = rvvm_blk_write(nvme->blk, buffer, size, pos);
                } else {
                    tmp = rvvm_blk_read(nvme->blk, buffer, size, pos);
                }
                if (tmp != size) {
                    nvme_complete_cmd(nvme, cmd, NVME_SC_DATA_ERR);
                    return;
                }
                pos += size;
            }
            nvme_complete_cmd(nvme, cmd, NVME_SC_SUCCESS);
            break;
        case NVME_IO_FLUSH:
            rvvm_blk_sync(nvme->blk);
            nvme_complete_cmd(nvme, cmd, NVME_SC_SUCCESS);
            break;
        case NVME_IO_WRITEZ:
            rvvm_blk_trim(nvme->blk, pos, cmd->prp.size);
            nvme_complete_cmd(nvme, cmd, NVME_SC_SUCCESS);
            break;
        case NVME_IO_DTSM:
            if (cmd->ptr[44] & 0x4) {
                // Deallocate (TRIM)
                cmd->prp.size = (((size_t)cmd->ptr[40]) + 1) << 4;
                while (cmd->prp.cur < cmd->prp.size) {
                    buffer = nvme_get_prp_chunk(nvme, cmd, &size);
                    if (!buffer) {
                        return;
                    }
                    for (size_t i = 0; i < size; i += 16) {
                        uint64_t trim_len = ((uint64_t)read_uint32_le(buffer + i + 4)) << NVME_LBAS;
                        uint64_t trim_pos = read_uint64_le(buffer + i + 8) << NVME_LBAS;
                        rvvm_blk_trim(nvme->blk, trim_pos, trim_len);
                    }
                }
            }
            nvme_complete_cmd(nvme, cmd, NVME_SC_SUCCESS);
            break;
        default:
            rvvm_debug("Unknown NVMe IO cmd %02x", opcode);
            nvme_complete_cmd(nvme, cmd, NVME_SC_BAD_OPCODE);
            break;
    }
}

static void nvme_run_cmd(nvme_dev_t* nvme, size_t sq_id, uint32_t sq_head)
{
    nvme_queue_t* sq      = &nvme->queues[sq_id << 1];
    rvvm_addr_t   sq_addr = nvme_queue_addr(sq);
    uint8_t*      ptr     = pci_get_dma_ptr(nvme->pci_func, sq_addr + (sq_head << 6), 64);

    if (likely(ptr)) {
        nvme_cmd_t cmd = {
            .ptr = ptr,
            .prp = {
                .prp1 = read_uint64_le(ptr + 24),
                .prp2 = read_uint64_le(ptr + 32),
                .size = (((size_t)read_uint16_le(ptr + 48)) + 1) << NVME_LBAS,
            },
            .cmd_id = read_uint16_le(ptr + 2),
            .sq_head_id = sq_head | (sq_id << 16),
            .cq_id = sq->cq_id,
        };

        if (sq_id) {
            nvme_io_cmd(nvme, &cmd);
        } else {
            nvme_admin_cmd(nvme, &cmd);
        }
    }
}

static void* nvme_cmd_worker(void** data)
{
    nvme_dev_t* nvme = data[0];
    nvme_run_cmd(nvme, (size_t)data[1], (size_t)data[2]);
    atomic_sub_uint32(&nvme->threads, 1);
    return NULL;
}

static void nvme_drain_sq(nvme_dev_t* nvme, size_t sq_id)
{
    nvme_queue_t* queue      = &nvme->queues[sq_id << 1];
    uint32_t      queue_size = atomic_load_uint32_relax(&queue->size);
    uint32_t      queue_tail = atomic_load_uint32_relax(&queue->tail);

    while (atomic_load_uint32_relax(&queue->head) != queue_tail) {
        uint32_t queue_head = nvme_queue_next(&queue->head, queue_size);
        if (sq_id) {
            void* args[3] = {
                nvme,
                (void*)sq_id,
                (void*)(size_t)queue_head,
            };
            atomic_add_uint32(&nvme->threads, 1);
            thread_create_task_va(nvme_cmd_worker, args, 3);
        } else {
            nvme_run_cmd(nvme, sq_id, queue_head);
        }
    }
}

static void nvme_doorbell(nvme_dev_t* nvme, size_t queue_id, uint16_t val)
{
    nvme_queue_t* queue      = &nvme->queues[queue_id];
    uint32_t      queue_size = atomic_load_uint32_relax(&queue->size);

    // Ignore attempts to overrun queue
    if (likely(val <= queue_size)) {
        if (queue_id & 1) {
            // Update completion queue head
            atomic_store_uint32_relax(&queue->head, val);
            if (atomic_load_uint32_relax(&queue->tail) == val) {
                nvme_queue_lower_irq(nvme, queue);
            }
        } else {
            // Update submission queue tail
            atomic_store_uint32_relax(&queue->tail, val);
            nvme_drain_sq(nvme, queue_id >> 1);
        }
    }
}

static bool nvme_pci_read(rvvm_mmio_dev_t* dev, void* data, size_t offset, uint8_t size)
{
    nvme_dev_t* nvme = dev->data;
    uint32_t    val  = 0;
    UNUSED(size);

    switch (offset) {
        case NVME_REG_CAP1:
            val = NVME_CAP1_MQES | NVME_CAP1_CQR | NVME_CAP1_TO;
            break;
        case NVME_REG_CAP2:
            val = NVME_CAP2_CSS;
            break;
        case NVME_REG_VS:
            val = NVME_VS_VERSION;
            break;
        case NVME_REG_INTMS:
        case NVME_REG_INTMC:
            val = atomic_load_uint32_relax(&nvme->irq_mask);
            break;
        case NVME_REG_CC: {
            uint32_t conf = atomic_load_uint32_relax(&nvme->conf);
            val           = (conf & NVME_CC_EN) | NVME_CC_IOQES;
            break;
        }
        case NVME_REG_CSTS: {
            uint32_t conf = atomic_load_uint32_relax(&nvme->conf);
            // CC.EN  -> CSTS.EN
            // CC.SHN -> CSTS.SHST
            val = (conf & NVME_CSTS_RDY);
            if (conf & NVME_CC_SHN) {
                // CC.SHN -> CSTS.SHST
                val |= NVME_CSTS_SHST;
            }
            break;
        }
        case NVME_REG_AQA:
            val  = atomic_load_uint32_relax(&nvme->queues[NVME_ADMIN_SQ].size);
            val |= atomic_load_uint32_relax(&nvme->queues[NVME_ADMIN_CQ].size) << 16;
            break;
        case NVME_REG_ASQ1:
            val = atomic_load_uint32_relax(&nvme->queues[NVME_ADMIN_SQ].addr_l);
            break;
        case NVME_REG_ASQ2:
            val = atomic_load_uint32_relax(&nvme->queues[NVME_ADMIN_SQ].addr_h);
            break;
        case NVME_REG_ACQ1:
            val = atomic_load_uint32_relax(&nvme->queues[NVME_ADMIN_CQ].addr_l);
            break;
        case NVME_REG_ACQ2:
            val = atomic_load_uint32_relax(&nvme->queues[NVME_ADMIN_CQ].addr_h);
            break;
    }

    write_uint32_le(data, val);
    return true;
}

static bool nvme_pci_write(rvvm_mmio_dev_t* dev, void* data, size_t offset, uint8_t size)
{
    nvme_dev_t* nvme = dev->data;
    uint32_t    val  = read_uint32_le(data);
    UNUSED(size);

    if (likely(offset >= 0x1000)) {
        // Doorbell write
        size_t queue_id = (offset - 0x1000) >> 2;
        if (queue_id < NVME_NQUEUES) {
            nvme_doorbell(nvme, queue_id, val);
        }
        return true;
    }

    switch (offset) {
        case NVME_REG_INTMS:
            atomic_or_uint32(&nvme->irq_mask, val);
            break;
        case NVME_REG_INTMC:
            atomic_and_uint32(&nvme->irq_mask, ~val);
            break;
        case NVME_REG_CC:
            atomic_store_uint32_relax(&nvme->conf, val);
            if ((val & NVME_CC_SHN) || !(val & NVME_CC_EN)) {
                // Shutdown the controller
                nvme_reset(nvme);
            }
            break;
        case NVME_REG_AQA:
            atomic_store_uint32_relax(&nvme->queues[NVME_ADMIN_SQ].size, bit_cut(val, 0, 12));
            atomic_store_uint32_relax(&nvme->queues[NVME_ADMIN_CQ].size, bit_cut(val, 16, 12));
            break;
        case NVME_REG_ASQ1:
            atomic_store_uint32_relax(&nvme->queues[NVME_ADMIN_SQ].addr_l, val & ~NVME_PAGE_MASK);
            break;
        case NVME_REG_ASQ2:
            atomic_store_uint32_relax(&nvme->queues[NVME_ADMIN_SQ].addr_h, val);
            break;
        case NVME_REG_ACQ1:
            atomic_store_uint32_relax(&nvme->queues[NVME_ADMIN_CQ].addr_l, val & ~NVME_PAGE_MASK);
            break;
        case NVME_REG_ACQ2:
            atomic_store_uint32_relax(&nvme->queues[NVME_ADMIN_CQ].addr_h, val);
            break;
    }

    return true;
}

pci_dev_t* nvme_init_blk(pci_bus_t* pci_bus, rvvm_blk_dev_t* blk)
{
    nvme_dev_t* nvme = safe_new_obj(nvme_dev_t);

    // Enable IEN on Admin Completion Queue
    nvme->queues[NVME_ADMIN_CQ].irq = NVME_CQ_FLAGS_IEN;

    nvme->blk = blk;
    rvvm_randomserial(nvme->serial, sizeof(nvme->serial));

    pci_func_desc_t nvme_desc = {
        .vendor_id  = 0x1F31, // Nextorage
        .device_id  = 0x4512, // Nextorage NE1N NVMe SSD
        .class_code = 0x0108, // Mass Storage, Non-Volatile memory controller
        .prog_if    = 0x02,   // NVMe
        .irq_pin    = PCI_IRQ_PIN_INTA,
        .bar[0] = {
            .size        = 0x4000,
            .data        = nvme,
            .type        = &nvme_type,
            .read        = nvme_pci_read,
            .write       = nvme_pci_write,
            .min_op_size = 4,
            .max_op_size = 4,
        },
    };

    pci_dev_t* pci_dev = pci_attach_func(pci_bus, &nvme_desc);
    if (pci_dev) {
        // Successfully plugged in
        nvme->pci_func = pci_get_device_func(pci_dev, 0);
    }
    return pci_dev;
}

pci_dev_t* nvme_init(pci_bus_t* pci_bus, const char* image, bool rw)
{
    rvvm_blk_dev_t* blk = rvvm_blk_open(image, NULL, rw ? RVVM_BLK_RW : RVVM_BLK_READ);
    if (blk) {
        return nvme_init_blk(pci_bus, blk);
    }
    return NULL;
}

pci_dev_t* nvme_init_auto(rvvm_machine_t* machine, const char* image, bool rw)
{
    return nvme_init(rvvm_get_pci_bus(machine), image, rw);
}

POP_OPTIMIZATION_SIZE

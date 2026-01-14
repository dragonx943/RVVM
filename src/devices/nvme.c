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
#define NVME_REG_CAP1            0x00 // Controller Capabilities
#define NVME_REG_CAP2            0x04 // Controller Capabilities
#define NVME_REG_VS              0x08 // Version
#define NVME_REG_INTMS           0x0C // Interrupt Mask Set
#define NVME_REG_INTMC           0x10 // Interrupt Mask Clear
#define NVME_REG_CC              0x14 // Controller Configuration
#define NVME_REG_CSTS            0x1C // Controller Status
#define NVME_REG_AQA             0x24 // Admin Queue Attributes
#define NVME_REG_ASQ1            0x28 // Admin Submission Queue Base Address
#define NVME_REG_ASQ2            0x2C
#define NVME_REG_ACQ1            0x30 // Admin Completion Queue Base Address
#define NVME_REG_ACQ2            0x34

/*
 * NVMe Register constants
 */
#define NVME_CAP1_MQES           0x0000FFFF // Maximum Queue Entries Supported: 65536
#define NVME_CAP1_CQR            0x00010000 // Contiguous Queues Required
#define NVME_CAP1_TO             0xFF000000 // Timeout: Max

#define NVME_CAP2_DSTRD          0x00000000 // Doorbell Stride (0 means 2-bit shift)
#define NVME_CAP2_CSS            0x00000020 // Command Sets Supported (NVM Command Set)

#define NVME_VS_VERSION          0x00010400 // NVMe v1.4

#define NVME_CC_EN               0x00000001 // Enabled
#define NVME_CC_SHN              0x0000C000 // Shutdown Notification
#define NVME_CC_IOQES            0x00460000 // IO Queue Entry Sizes (16b:64b)

#define NVME_CSTS_RDY            0x00000001 // Ready
#define NVME_CSTS_SHST           0x00000008 // Shutdown complete

/*
 * NVMe Queue IDs
 */
#define NVME_ADMIN_SQ            0x00 // Admin Submission Queue
#define NVME_ADMIN_CQ            0x01 // Admin Completion Queue

/*
 * NVMe Submission Queue Entry definitions
 */
#define NVME_SQE_SIZE            0x40 // Completion Queue Entry Size
#define NVME_SQE_SIZE_SHIFT      0x06 // Completion Queue Entry Size Shift

#define NVME_SQE_CDW0            0x00 // Command Dword 0
#define NVME_SQE_CID             0x02 // Command Identifier
#define NVME_SQE_NSID            0x04 // Namespace Identifier
#define NVME_SQE_MPTR            0x10 // Metadata Pointer (Contiguous buffer or SGL descriptor)
#define NVME_SQE_DPTR            0x18 // Data Pointer (PRP1+PRP2 or SGL segment)
#define NVME_SQE_PRP1            0x18 // PRP1 (Pointer to first page)
#define NVME_SQE_PRP2            0x20 // PRP2 (Pointer to second page or PRP list)
#define NVME_SQE_CDW10           0x28 // Command Dword 10 (Command-Specific)
#define NVME_SQE_CDW11           0x2C // Command Dword 11 (Command-Specific)
#define NVME_SQE_CDW12           0x30 // Command Dword 12 (Command-Specific)
#define NVME_SQE_CDW13           0x34 // Command Dword 13 (Command-Specific)
#define NVME_SQE_CDW14           0x38 // Command Dword 14 (Command-Specific)
#define NVME_SQE_CDW15           0x3C // Command Dword 15 (Command-Specific)

#define NVME_CDW0_PRP            0x00000000 // PRPs are used for this transfer
#define NVME_CDW0_SGL_MPBUF      0x00004000 // SGLs are used for this transfer. MPTR contains a contiguous buffer.
#define NVME_CDW0_SGL_MPSGL      0x00008000 // PRPs are used for this transfer. MPTR contains an SGL descriptor.
#define NVME_CDW0_SGL_MASK       0x0000C000 // Mask to detect SGL usage

#define NVME_CDW0_CID_SHIFT      0x10 // Command Identifier (CID) shift from CDW0

/*
 * NVMe Completion Queue Entry definitions
 */
#define NVME_CQE_SIZE            0x10 // Completion Queue Entry Size
#define NVME_CQE_SIZE_SHIFT      0x04 // Completion Queue Entry Size Shift

#define NVME_CQE_CS              0x00 // Command Specific
#define NVME_CQE_RSVD            0x04 // Reserved
#define NVME_CQE_SQHD_SQID       0x08 // Submission Queue Head Pointer & Identifier
#define NVME_CQE_CID_PB_SF       0x0C // Command Identifier, Phase Bit, Status Field

#define NVME_CQE_PB_MASK         0x00010000 // Phase Bit
#define NVME_CQE_SF_SHIFT        0x11       // Status Shift

/*
 * NVMe Completion Queue Status Codes
 */
#define NVME_SC_SUCCESS          0x00 // Successful Completion
#define NVME_SC_BAD_OPCODE       0x01 // Invalid Command Opcode
#define NVME_SC_BAD_FIELD        0x02 // Invalid Field in Command
#define NVME_SC_DATA_ERR         0x04 // Data Transfer Error
#define NVME_SC_ABORT            0x07 // Command Abort Requested
#define NVME_SC_SQ_DELETED       0x08 // Command Aborted due to SQ Deletion
#define NVME_SC_FEAT_NSAVE       0x0D // Feature Identifier Not Saveable
#define NVME_SC_FEAT_NCHG        0x0E // Feature Identifier Not Changeable
#define NVME_SC_LBA_RANGE        0x80 // LBA Out of Range

/*
 * NVMe Command Specific Status
 */
#define NVME_CS_CQ_INVALID       0x00 // Completion Queue Invalid
#define NVME_CS_ID_INVALID       0x01 // Invalid Queue Identifier
#define NVME_CS_SIZE_INVALID     0x02 // Invalid Queue Size

/*
 * NVMe Admin Command Set
 */
#define NVME_ADM_DELETE_IO_SQ    0x00 // Delete IO Submission Queue
#define NVME_ADM_CREATE_IO_SQ    0x01 // Create IO Submission Queue
#define NVME_ADM_GET_LOG_PAGE    0x02 // Get Log Page
#define NVME_ADM_DELETE_IO_CQ    0x04 // Delete IO Completion Queue
#define NVME_ADM_CREATE_IO_CQ    0x05 // Create IO Completion Queue
#define NVME_ADM_IDENTIFY        0x06 // Identify
#define NVME_ADM_ABORT           0x08 // Abort Command
#define NVME_ADM_SET_FEATURE     0x09 // Set Features
#define NVME_ADM_GET_FEATURE     0x0A // Get Features
#define NVME_ADM_ASYNC_EVENT_REQ 0x0C // Asynchronous Event Request
#define NVME_ADM_NAMESPACE_MGMT  0x0C // Namespace Management
#define NVME_ADM_FIRMWARE_COMM   0x10 // Firmware Commit
#define NVME_ADM_FIRMWARE_DOWN   0x11 // Firmware Image Download
#define NVME_ADM_SELF_TEST       0x14 // Device Self-Test
#define NVME_ADM_NAMESPACE_ATCH  0x15 // Namespace Attachment
#define NVME_ADM_KEEP_ALIVE      0x18 // Keep Alive
#define NVME_ADM_DIRECTIVE_SEND  0x19 // Directive Send
#define NVME_ADM_DIRECTIVE_RECV  0x1A // Directive Receive
#define NVME_ADM_VIRT_MGMT       0x1C // Virtualization Management
#define NVME_ADM_NVME_MI_SEND    0x1D // NVMe-MI Send
#define NVME_ADM_NVME_MI_RECV    0x1E // NVMe-MI Receive
#define NVME_ADM_DBELL_BUFF_CFG  0x7C // Doorbell Buffer Config

/*
 * Create IO Completion Queue Dword 11 bits
 */
#define NVME_CQ_FLAGS_PC         0x01 // Physically Contiguous
#define NVME_CQ_FLAGS_IEN        0x02 // Interrupts Enabled

/*
 * NVMe Identify command fields
 */
#define NVME_IDENT_NAMESPACE     0x00 // Identify Namespace
#define NVME_IDENT_CONTROLLER    0x01 // Identify Controller
#define NVME_IDENT_NMSPC_LIST    0x02 // Identify Namespace List
#define NVME_IDENT_NMSPC_DESC    0x03 // Identify Namespace Descriptors

/*
 * NVMe Set Feature command fields
 */
#define NVME_FEAT_ARBITRATION    0x01 // Arbitration
#define NVME_FEAT_POWER_MGMT     0x02 // Power Management
#define NVME_FEAT_TEMP_THRESH    0x04 // Temperature Threshold
#define NVME_FEAT_ERROR_RECOV    0x05 // Error Recovery
#define NVME_FEAT_VOLATILE_WC    0x06 // Volatile Write Cache
#define NVME_FEAT_NUM_QUEUES     0x07 // Number of Queues
#define NVME_FEAT_IRQ_COALESC    0x08 // Interrupt Coalescing
#define NVME_FEAT_IRQ_VECTOR     0x09 // Interrupt Vector Configuration
#define NVME_FEAT_WR_ATOMIC      0x0A // Write Atomicity Normal
#define NVME_FEAT_ASYNC_EVENT    0x0B // Asynchronous Event Configuration

/*
 * NVMe IO Command Set
 */
#define NVME_IO_FLUSH            0x00 // Flush buffers
#define NVME_IO_WRITE            0x01 // Write
#define NVME_IO_READ             0x02 // Read
#define NVME_IO_WRITEZ           0x08 // Write Zeroes
#define NVME_IO_DTSM             0x09 // Dataset Management

/*
 * NVMe Implementation constants
 */

#define NVME_PAGE_SHIFT          0x0C // Page Size Shift (4kb pages)
#define NVME_LBA_SHIFT           0x09 // LBA Block Size Shift (512b logical blocks)
#define NVME_IO_QUEUES           0x10 // Max IO Queues (16)

#define NVME_PAGE_SIZE           (1ULL << NVME_PAGE_SHIFT)
#define NVME_PAGE_MASK           (NVME_PAGE_SIZE - 1ULL)
#define NVME_LBA_SIZE            (1ULL << NVME_LBA_SHIFT)
#define NVME_LBA_MASK            (NVME_LBA_SIZE - 1ULL)

#define NVME_FEAT_NQES           ((NVME_IO_QUEUES - 1) | ((NVME_IO_QUEUES - 1) << 16))

// IO + Admin SQ/CQ
#define NVME_NQUEUES             ((NVME_IO_QUEUES + 1) << 1)

typedef struct {
    uint32_t addr_l;
    uint32_t addr_h;
    uint32_t size;
    uint32_t head;
    uint32_t tail;

    // For submission queue: Completion queue ID
    // For completion queue: Interrupt configuration
    uint32_t data;
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
    size_t      size;
    size_t      cur;
} nvme_prp_ctx_t;

typedef struct {
    // Submission queue entry
    const uint8_t* sqe;

    // PRP parser context
    nvme_prp_ctx_t prp;

    // Command information
    uint32_t sqhd_sqid;
    uint32_t cq_id;
} nvme_cmd_t;

// Helper for head/tail manipulation on dequeue/enqueue
static inline uint32_t nvme_queue_next(uint32_t* head_or_tail, uint32_t queue_size)
{
    uint32_t entry = atomic_load_uint32_relax(head_or_tail);
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
    uint32_t irq_reg = atomic_load_uint32_relax(&queue->data);
    if (likely(irq_reg & NVME_CQ_FLAGS_IEN)) {
        uint32_t irq_vec = irq_reg >> 16;
        if (!(atomic_load_uint32_relax(&nvme->irq_mask) & bit_set32(irq_vec))) {
            pci_raise_irq(nvme->pci_func, irq_vec);
        }
    }
}

static void nvme_queue_lower_irq(nvme_dev_t* nvme, nvme_queue_t* queue)
{
    uint32_t irq_vec = atomic_load_uint32_relax(&queue->data) >> 16;
    pci_lower_irq(nvme->pci_func, irq_vec);
}

static inline rvvm_addr_t nvme_queue_addr(const nvme_queue_t* queue)
{
    uint32_t low  = atomic_load_uint32_relax(&queue->addr_l);
    uint32_t high = atomic_load_uint32_relax(&queue->addr_h);
    return low | (((uint64_t)high) << 32);
}

static void nvme_queue_setup(nvme_dev_t* nvme, nvme_queue_t* queue, //
                             rvvm_addr_t addr, uint32_t size, uint32_t data)
{
    atomic_store_uint32_relax(&queue->head, 0);
    atomic_store_uint32_relax(&queue->tail, 0);
    atomic_store_uint32_relax(&queue->addr_l, addr & ~NVME_PAGE_MASK);
    atomic_store_uint32_relax(&queue->addr_h, addr >> 32);
    atomic_store_uint32_relax(&queue->size, size);
    atomic_store_uint32_relax(&queue->data, data);
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
        uint32_t size = 0, data = 0;
        if (i == NVME_ADMIN_SQ || i == NVME_ADMIN_CQ) {
            // Keep Admin queues intact
            addr = nvme_queue_addr(&nvme->queues[i]);
            size = atomic_load_uint32_relax(&nvme->queues[i].size);
            data = atomic_load_uint32_relax(&nvme->queues[i].data);
        }
        nvme_queue_setup(nvme, &nvme->queues[i], addr, size, data);
    }
}

static void nvme_complete_cmd_cs(nvme_dev_t* nvme, nvme_cmd_t* cmd, uint32_t status, uint32_t cs)
{
    nvme_queue_t* queue      = &nvme->queues[(cmd->cq_id << 1) + 1];
    uint32_t      queue_size = atomic_load_uint32_relax(&queue->size);
    uint32_t      queue_tail = nvme_queue_next(&queue->tail, queue_size);
    rvvm_addr_t   addr       = nvme_queue_addr(queue) + (queue_tail << NVME_CQE_SIZE_SHIFT);
    uint8_t*      cqe        = pci_get_dma_ptr(nvme->pci_func, addr, NVME_CQE_SIZE);
    if (likely(cqe)) {
        uint32_t cmd_id = read_uint16_le(cmd->sqe + NVME_SQE_CID);
        uint32_t phase  = (~read_uint32_le(cqe + NVME_CQE_CID_PB_SF)) & NVME_CQE_PB_MASK;
        uint32_t cid_ps = cmd_id | phase | (status << NVME_CQE_SF_SHIFT);
        write_uint32_le(cqe + NVME_CQE_CS, cs);
        write_uint32_le(cqe + NVME_CQE_SQHD_SQID, cmd->sqhd_sqid);
        atomic_store_uint32_le(cqe + NVME_CQE_CID_PB_SF, cid_ps);
    }
    nvme_queue_raise_irq(nvme, queue);
}

static inline void nvme_complete_cmd(nvme_dev_t* nvme, nvme_cmd_t* cmd, uint32_t status)
{
    nvme_complete_cmd_cs(nvme, cmd, status, 0);
}

static inline rvvm_addr_t nvme_prepare_prp(nvme_cmd_t* cmd, size_t size)
{
    cmd->prp.prp1 = read_uint64_le(cmd->sqe + NVME_SQE_PRP1);
    cmd->prp.prp2 = read_uint64_le(cmd->sqe + NVME_SQE_PRP2);
    cmd->prp.size = size;
    cmd->prp.cur  = 0;
    return cmd->prp.prp1;
}

static inline size_t nvme_prp_avail(const nvme_cmd_t* cmd)
{
    return cmd->prp.size - cmd->prp.cur;
}

static size_t nvme_parse_prp_region(nvme_dev_t* nvme, nvme_cmd_t* cmd)
{
    nvme_prp_ctx_t* prp = &cmd->prp;
    size_t          len = NVME_PAGE_SIZE;

    if (prp->cur == 0) {
        // Consume the first page from PRP1, may be misaligned
        len = NVME_PAGE_SIZE - (prp->prp1 & NVME_PAGE_MASK);
        if (len >= prp->size) {
            // Single-page region
            return prp->size;
        } else if (prp->size <= len + NVME_PAGE_SIZE) {
            // PRP2 encodes second page address
            rvvm_addr_t page = prp->prp2 & ~NVME_PAGE_MASK;
            if (page == prp->prp1 + len) {
                // Contiguous two-page region
                return prp->size;
            } else {
                // Scattered two-page region
                prp->prp1 = page;
                return len;
            }
        }
    }

    // Process PRP list entries until we reach end of transfer
    rvvm_addr_t prp2 = prp->prp2 & ~7ULL;
    uint8_t*    dma  = NULL;
    while (nvme_prp_avail(cmd) > len) {
        if (!dma) {
            // Obtain DMA mapping of the PRP list
            dma = pci_get_dma_ptr(nvme->pci_func, prp2 & ~NVME_PAGE_MASK, NVME_PAGE_SIZE);
            if (!dma) {
                // PRP list DMA error
                rvvm_debug("NVMe PRP list DMA error at %#llx", (long long)prp2);
                break;
            }
        }
        if (!((prp2 + 8) & NVME_PAGE_MASK) && nvme_prp_avail(cmd) > len + NVME_PAGE_SIZE) {
            // Fetch next PRP list in the chain
            prp2 = read_uint64_le_m(dma + NVME_PAGE_SIZE - 8) & ~NVME_PAGE_MASK;
            dma  = NULL;
        } else {
            // Fetch next PRP list entry
            rvvm_addr_t page = read_uint64_le_m(dma + (prp2 & NVME_PAGE_MASK));
            // Advance pointers
            prp2 += 8;
            if (page != (prp->prp1 + len)) {
                // Scattered region
                prp->prp1 = page;
                prp->prp2 = prp2;
                break;
            }
            len += NVME_PAGE_SIZE;
        }
    }

    return EVAL_MIN(len, nvme_prp_avail(cmd));
}

static void* nvme_get_prp_region(nvme_dev_t* nvme, nvme_cmd_t* cmd, size_t* size)
{
    rvvm_addr_t reg_addr = cmd->prp.prp1;
    size_t      reg_size = nvme_parse_prp_region(nvme, cmd);
    if (reg_size) {
        void* region  = pci_get_dma_ptr(nvme->pci_func, reg_addr, reg_size);
        cmd->prp.cur += reg_size;
        if (region) {
            *size = reg_size;
            return region;
        }
    }
    *size = 0;
    rvvm_debug("NVMe PRP region DMA error at %#llx", (long long)reg_addr);
    return NULL;
}

static void nvme_copy_to_prp(nvme_dev_t* nvme, nvme_cmd_t* cmd, const void* data, size_t size)
{
    while (size) {
        size_t reg_size = 0;
        void*  region   = nvme_get_prp_region(nvme, cmd, &reg_size);
        if (region) {
            reg_size = EVAL_MIN(reg_size, size);
            memcpy(region, data, reg_size);
            data  = (const void*)(((const uint8_t*)data) + reg_size);
            size -= reg_size;
        } else {
            break;
        }
    }
}

static void nvme_identify(nvme_dev_t* nvme, nvme_cmd_t* cmd)
{
    uint8_t* buf = safe_new_arr(uint8_t, NVME_PAGE_SIZE);
    uint8_t  idt = cmd->sqe[NVME_SQE_CDW10];
    switch (idt) {
        case NVME_IDENT_NAMESPACE: {
            uint64_t lbas = rvvm_blk_get_size(nvme->blk) >> NVME_LBA_SHIFT;
            write_uint64_le(buf, lbas);
            write_uint64_le(buf + 8, lbas);
            write_uint64_le(buf + 16, lbas);
            buf[130] = NVME_LBA_SHIFT;
            break;
        }
        case NVME_IDENT_CONTROLLER: {
            // Controller identification
            memcpy(buf + 4, nvme->serial, sizeof(nvme->serial)); // Serial Number
            rvvm_strlcpy((char*)buf + 24, "NVMe Storage", 40);   // Model
            rvvm_strlcpy((char*)buf + 64, "R2570", 8);           // Firmware Revision
            write_uint32_le(buf + 80, NVME_VS_VERSION);          // Version

            // Controller features
            buf[111] = 1;    // Controller Type: I/O Controller
            buf[512] = 0x66; // Submission Queue Max/Cur Entry Size
            buf[513] = 0x44; // Completion Queue Max/Cur Entry Size
            buf[516] = 1;    // Number of Namespaces
            buf[520] = 0x4;  // Supports Dataset Management (TRIM)

            // NVMe Qualified Name (Includes serial to distinguish targets)
            size_t nqn_off = rvvm_strlcpy(((char*)buf) + 768, "nqn.2022-04.lekkit:nvme:", 256);
            memcpy(buf + 768 + nqn_off, nvme->serial, sizeof(nvme->serial));
            break;
        }
        case NVME_IDENT_NMSPC_LIST:
            write_uint32_le(buf, 0x1); // Namespace #1
            break;
        case NVME_IDENT_NMSPC_DESC:
            buf[0] = 3;  // Namespace uses UUID
            buf[1] = 16; // UUID length
            break;
        default:
            nvme_complete_cmd(nvme, cmd, NVME_SC_BAD_FIELD);
            safe_free(buf);
            return;
    }
    nvme_prepare_prp(cmd, NVME_PAGE_SIZE);
    nvme_copy_to_prp(nvme, cmd, buf, NVME_PAGE_SIZE);
    nvme_complete_cmd(nvme, cmd, NVME_SC_SUCCESS);
    safe_free(buf);
}

static void nvme_create_io_sq(nvme_dev_t* nvme, nvme_cmd_t* cmd)
{
    rvvm_addr_t sq_addr = nvme_prepare_prp(cmd, 0);
    uint32_t    sq_id   = read_uint16_le(cmd->sqe + NVME_SQE_CDW10);
    uint32_t    sq_size = read_uint16_le(cmd->sqe + NVME_SQE_CDW10 + 2);
    uint32_t    cq_flag = read_uint16_le(cmd->sqe + NVME_SQE_CDW11);
    uint32_t    cq_id   = read_uint16_le(cmd->sqe + NVME_SQE_CDW11 + 2);
    if (!sq_id || sq_id > NVME_IO_QUEUES) {
        // Queue ID invalid
        nvme_complete_cmd_cs(nvme, cmd, NVME_SC_BAD_FIELD, NVME_CS_ID_INVALID);
    } else if (!cq_id || cq_id > NVME_IO_QUEUES) {
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
        nvme_queue_setup(nvme, queue, sq_addr, sq_size, cq_id);
        nvme_complete_cmd(nvme, cmd, NVME_SC_SUCCESS);
    }
}

static void nvme_create_io_cq(nvme_dev_t* nvme, nvme_cmd_t* cmd)
{
    rvvm_addr_t cq_addr = nvme_prepare_prp(cmd, 0);
    uint32_t    cq_id   = read_uint16_le(cmd->sqe + NVME_SQE_CDW10);
    uint32_t    cq_size = read_uint16_le(cmd->sqe + NVME_SQE_CDW10 + 2);
    uint32_t    cq_flag = read_uint32_le(cmd->sqe + NVME_SQE_CDW11);
    if (!cq_id || cq_id > NVME_IO_QUEUES) {
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
        nvme_queue_setup(nvme, queue, cq_addr, cq_size, cq_flag);
        nvme_complete_cmd(nvme, cmd, NVME_SC_SUCCESS);
    }
}

static void nvme_delete_io_queue(nvme_dev_t* nvme, nvme_cmd_t* cmd, bool is_cq)
{
    size_t queue_id = read_uint16_le(cmd->sqe + NVME_SQE_CDW10);
    if (!queue_id || queue_id > NVME_IO_QUEUES) {
        // Queue ID invalid
        nvme_complete_cmd_cs(nvme, cmd, NVME_SC_BAD_FIELD, NVME_CS_ID_INVALID);
    } else {
        nvme_queue_setup(nvme, &nvme->queues[(queue_id << 1) + is_cq], 0, 0, 0);
        nvme_complete_cmd(nvme, cmd, NVME_SC_SUCCESS);
    }
}

static void nvme_handle_feature(nvme_dev_t* nvme, nvme_cmd_t* cmd, bool set)
{
    uint8_t feature_id = cmd->sqe[NVME_SQE_CDW10];
    UNUSED(set);
    switch (feature_id) {
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
}

static void nvme_admin_cmd(nvme_dev_t* nvme, nvme_cmd_t* cmd)
{
    uint8_t opcode = cmd->sqe[NVME_SQE_CDW0];
    switch (opcode) {
        case NVME_ADM_IDENTIFY:
            nvme_identify(nvme, cmd);
            return;
        case NVME_ADM_CREATE_IO_SQ:
            nvme_create_io_sq(nvme, cmd);
            return;
        case NVME_ADM_CREATE_IO_CQ:
            nvme_create_io_cq(nvme, cmd);
            return;
        case NVME_ADM_DELETE_IO_SQ:
        case NVME_ADM_DELETE_IO_CQ:
            nvme_delete_io_queue(nvme, cmd, opcode == NVME_ADM_DELETE_IO_CQ);
            return;
        case NVME_ADM_SET_FEATURE:
        case NVME_ADM_GET_FEATURE:
            nvme_handle_feature(nvme, cmd, opcode == NVME_ADM_DELETE_IO_CQ);
            return;
        case NVME_ADM_ABORT:
            // Ignore
            nvme_complete_cmd(nvme, cmd, NVME_SC_SUCCESS);
            return;
        default:
            rvvm_debug("Unknown NVMe admin cmd %#02x", opcode);
            nvme_complete_cmd(nvme, cmd, NVME_SC_BAD_OPCODE);
            return;
    }
}

static void nvme_io_cmd(nvme_dev_t* nvme, nvme_cmd_t* cmd)
{
    uint8_t opcode = cmd->sqe[NVME_SQE_CDW0];
    switch (opcode) {
        case NVME_IO_READ:
        case NVME_IO_WRITE: {
            uint64_t pos = read_uint64_le(cmd->sqe + NVME_SQE_CDW10) << NVME_LBA_SHIFT;
            size_t   nlb = read_uint16_le(cmd->sqe + NVME_SQE_CDW12);
            nvme_prepare_prp(cmd, (nlb + 1) << NVME_LBA_SHIFT);
            while (nvme_prp_avail(cmd)) {
                size_t size = 0, tmp = 0;
                void*  buffer = nvme_get_prp_region(nvme, cmd, &size);
                if (buffer) {
                    if (opcode == NVME_IO_WRITE) {
                        tmp = rvvm_blk_write(nvme->blk, buffer, size, pos);
                    } else {
                        tmp = rvvm_blk_read(nvme->blk, buffer, size, pos);
                    }
                }
                if (tmp != size) {
                    nvme_complete_cmd(nvme, cmd, NVME_SC_DATA_ERR);
                    return;
                }
                pos += size;
            }
            nvme_complete_cmd(nvme, cmd, NVME_SC_SUCCESS);
            break;
        }
        case NVME_IO_FLUSH:
            rvvm_blk_sync(nvme->blk);
            nvme_complete_cmd(nvme, cmd, NVME_SC_SUCCESS);
            break;
        case NVME_IO_DTSM:
            if (cmd->sqe[NVME_SQE_CDW11] & 0x4) {
                // Deallocate (TRIM)
                size_t nr = cmd->sqe[NVME_SQE_CDW10];
                nvme_prepare_prp(cmd, (nr + 1) << 4);
                while (nvme_prp_avail(cmd)) {
                    size_t   size   = 0;
                    uint8_t* buffer = nvme_get_prp_region(nvme, cmd, &size);
                    if (buffer) {
                        for (size_t i = 0; i < size; i += 16) {
                            uint64_t trim_lba = read_uint32_le(buffer + i + 4);
                            uint64_t trim_pos = read_uint64_le(buffer + i + 8) << NVME_LBA_SHIFT;
                            rvvm_blk_trim(nvme->blk, trim_pos, trim_lba << NVME_LBA_SHIFT);
                        }
                    }
                }
            }
            nvme_complete_cmd(nvme, cmd, NVME_SC_SUCCESS);
            break;
        default:
            rvvm_debug("Unknown NVMe IO cmd %#02x", opcode);
            nvme_complete_cmd(nvme, cmd, NVME_SC_BAD_OPCODE);
            break;
    }
}

static void nvme_run_cmd(nvme_dev_t* nvme, size_t sq_id, uint32_t sq_head)
{
    nvme_queue_t* sq       = &nvme->queues[sq_id << 1];
    rvvm_addr_t   sqe_addr = nvme_queue_addr(sq) + (sq_head << NVME_SQE_SIZE_SHIFT);
    uint8_t*      sqe_ptr  = pci_get_dma_ptr(nvme->pci_func, sqe_addr, NVME_SQE_SIZE);

    if (likely(sqe_ptr)) {
        nvme_cmd_t cmd = {
            .sqe       = sqe_ptr,
            .sqhd_sqid = sq_head | (sq_id << 16),
            .cq_id     = sq->data,
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

pci_dev_t* nvme_init_blk(pci_bus_t* pci_bus, rvvm_blk_dev_t* blk)
{
    nvme_dev_t* nvme = safe_new_obj(nvme_dev_t);

    // Enable IEN on Admin Completion Queue
    nvme->queues[NVME_ADMIN_CQ].data = NVME_CQ_FLAGS_IEN;

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

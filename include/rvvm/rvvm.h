/*
<rvvm/utils.h> - RVVM Public API
Copyright (C) 2020-2025 LekKit <github.com/LekKit>
                        cerg2010cerg2010 <github.com/cerg2010cerg2010>
                        Mr0maks <mr.maks0443@gmail.com>
                        KotB <github.com/0xCatPKG>
                        X512 <github.com/X547>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _RVVM_PUBLIC_API_H
#define _RVVM_PUBLIC_API_H

#if defined(__cplusplus)
#define RVVM_EXTERN_C_BEGIN extern "C" {
#define RVVM_EXTERN_C_END   }
#else
#define RVVM_EXTERN_C_BEGIN
#define RVVM_EXTERN_C_END
#endif

RVVM_EXTERN_C_BEGIN

/* For size_t, NULL */
#include <stddef.h>

#if defined(__GNUC__) || defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
/* For bool, uint32_t, etc */
#if __STDC_VERSION__ < 202311L && !defined(__cplusplus)
#include <stdbool.h>
#endif
#include <stdint.h>
#else
/* Pre-C99 translation unit, expose C99 definitions for public headers */
#if defined(_MSC_VER) && _MSC_VER <= 1500
typedef int _Bool;
#endif
typedef signed char    __int8_t;
typedef unsigned char  __uint8_t;
typedef signed short   __int16_t;
typedef unsigned short __uint16_t;
typedef signed int     __int32_t;
typedef unsigned int   __uint32_t;
#if defined(__LP64__)
typedef signed long int   __int64_t;
typedef unsigned long int __uint64_t;
#else
typedef signed long long   __int64_t;
typedef unsigned long long __uint64_t;
#endif
/* Replace <stdbool.h> */
#if !defined(__bool_true_false_are_defined) && !defined(__cplusplus)
#define __bool_true_false_are_defined 1
#undef bool
#define bool _Bool
#undef false
#define false 0
#undef true
#define true 0
#endif
/* Replace <stdint.h> */
#undef int8_t
#define int8_t __int8_t
#undef uint8_t
#define uint8_t __uint8_t
#undef int16_t
#define int16_t __int16_t
#undef uint16_t
#define uint16_t __uint16_t
#undef int32_t
#define int32_t __int32_t
#undef uint32_t
#define uint32_t __uint32_t
#undef int64_t
#define int64_t __int64_t
#undef uint64_t
#define uint64_t __uint64_t
#endif

/* Expose inline attribute */
#undef inline
#if !(defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L) && defined(_MSC_VER)
#define inline __inline
#elif !(defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L) && defined(__GNUC__)
#define inline __inline__
#elif !(defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L)
#define inline
#endif

#if !defined(RVVM_STATIC) && (defined(_WIN32) || defined(__CYGWIN__)) && defined(USE_LIB)
/* Mark symbols as dllexport when building shared librvvm */
#define RVVM_PUBLIC __declspec(dllexport)
#elif !defined(RVVM_STATIC) && (defined(_WIN32) || defined(__CYGWIN__)) && !defined(RVVM_VERSION)
/* Mark symbols as dllimport when linking to shared librvvm */
#define RVVM_PUBLIC __declspec(dllimport)
#elif !defined(RVVM_STATIC) && defined(__GNUC__) && __GNUC__ >= 4 && (!defined(RVVM_VERSION) || defined(USE_LIB))
/* Mark symbols as visible when building/linking to shared librvvm */
#define RVVM_PUBLIC __attribute__((__visibility__("default")))
#else
#define RVVM_PUBLIC
#endif

#if !defined(RVVM_VERSION)
/**
 * Version string
 */
#define RVVM_VERSION "0.7-git"
#endif

/**
 * Increments on each API/ABI breakage, defined to -1 in staging versions with unstable API/ABI
 */
#define RVVM_ABI_VERSION -1

/**
 * Check librvvm ABI compatibility via rvvm_check_abi(RVVM_ABI_VERSION)
 * \return True if librvvm supports the requested ABI
 */
RVVM_PUBLIC bool rvvm_check_abi(int abi);

/**
 * @defgroup rvvm_types RVVM Type definitions
 * @addtogroup rvvm_types
 * @{
 */

/**
 * Physical memory address
 */
typedef uint64_t rvvm_addr_t;

/**
 * Interrupt index
 */
typedef uint32_t rvvm_irq_t;

/**
 * Machine handle
 */
typedef struct rvvm_machine_t rvvm_machine_t;

/**
 * CPU handle
 */
typedef struct rvvm_hart_t rvvm_cpu_t;

/**
 * MMIO device handle
 */
typedef struct rvvm_mmio_dev_t rvvm_mmio_dev_t;

/**
 * MMIO access handler
 * \param dev    Device handle
 * \param dest   Pointer to load/store data
 * \param offset Offset within device region, always aligned to size
 * \param size   Access size
 * \return True if access was handled by this device
 */
typedef bool (*rvvm_mmio_handler_t)(rvvm_mmio_dev_t* dev, void* dest, size_t offset, uint8_t size);

/**
 * Interrupt controller handle
 */
typedef struct rvvm_intc rvvm_intc_t;

/**
 * PCI Bus (PCI Express Root Complex) handle
 */
typedef struct rvvm_pci_bus pci_bus_t;

/**
 * I2C Bus handle
 */
typedef struct rvvm_i2c_bus i2c_bus_t;

/**
 * Flattened Device Tree node
 */
typedef struct fdt_node rvvm_fdt_node_t;

/** @}*/

/**
 * @defgroup rvvm_machine_api RVVM Machine API
 * @addtogroup rvvm_machine_api
 * @{
 */

/*
 * Machine options (Settable)
 */

#define RVVM_OPT_NONE         0x00 /**< Invalid option                                         */
#define RVVM_OPT_RESET_PC     0x01 /**< Physical jump address at reset, defaults to 0x80000000 */
#define RVVM_OPT_FDT_ADDR     0x02 /**< Physical FDT address at reset for manual FDT loading   */
#define RVVM_OPT_TIME_FREQ    0x03 /**< Machine timer frequency, 10Mhz by default              */
#define RVVM_OPT_HW_IMITATE   0x04 /**< Imitate traits or identity of physical hardware        */
#define RVVM_OPT_MAX_CPU_CENT 0x05 /**< Maximum CPU load % per guest/host CPUs                 */
#define RVVM_OPT_JIT          0x06 /**< Enable JIT                                             */
#define RVVM_OPT_JIT_CACHE    0x07 /**< Amount of per-cpu JIT cache (In bytes)                 */
#define RVVM_OPT_JIT_HARVARD  0x08 /**< No dirty code tracking, explicit ifence, slower        */

/*
 * Machine options (Special or read-only)
 */

#define RVVM_OPT_MEM_BASE     0x80000001U /**< Physical RAM base address, Default: 0x80000000 */
#define RVVM_OPT_MEM_SIZE     0x80000002U /**< Physical RAM size                              */
#define RVVM_OPT_CPU_COUNT    0x80000003U /**< Amount of CPUs                                 */

/**
 * Creates a new virtual machine
 *
 * \param mem_size   Amount of memory (in bytes), should be page-aligned
 * \param hart_count Amount of HARTs (cores), should be >=1
 * \param isa        String describing the CPU instruction set, or NULL to pick rv64
 * \return Valid machine handle, or NULL on failure
 */
RVVM_PUBLIC rvvm_machine_t* rvvm_create_machine(size_t mem_size, size_t hart_count, const char* isa);

/**
 * Set a new kernel cmdline passed to the guest kernel
 */
RVVM_PUBLIC void rvvm_set_cmdline(rvvm_machine_t* machine, const char* str);

/**
 * Append to the kernel cmdline
 */
RVVM_PUBLIC void rvvm_append_cmdline(rvvm_machine_t* machine, const char* str);

/**
 * Load machine firmware, which is executed from RVVM_OPT_RESET_PC
 */
RVVM_PUBLIC bool rvvm_load_firmware(rvvm_machine_t* machine, const char* path);

/**
 * Load kernel payload, which is usually the next stage after the firmware
 */
RVVM_PUBLIC bool rvvm_load_kernel(rvvm_machine_t* machine, const char* path);

/**
 * Load a custom Flattened Device Tree, which is passed to guest at reset
 */
RVVM_PUBLIC bool rvvm_load_fdt(rvvm_machine_t* machine, const char* path);

/**
 * Dump generated Flattened Device Tree to file
 */
RVVM_PUBLIC bool rvvm_dump_fdt(rvvm_machine_t* machine, const char* path);

/**
 * Get machine option value
 */
RVVM_PUBLIC rvvm_addr_t rvvm_get_opt(rvvm_machine_t* machine, uint32_t opt);

/**
 * Set machine option
 */
RVVM_PUBLIC bool rvvm_set_opt(rvvm_machine_t* machine, uint32_t opt, rvvm_addr_t val);

/**
 * Powers up or resumes a paused machine, returns immediately
 *
 * \return Machine start success, false if it was already running
 */
RVVM_PUBLIC bool rvvm_start_machine(rvvm_machine_t* machine);

/**
 * Pauses the machine (Stops the vCPUs)
 *
 * \return Machine pause success, false if it wasn't running yet
 */
RVVM_PUBLIC bool rvvm_pause_machine(rvvm_machine_t* machine);

/**
 * Reset the machine (Continues running if it was powered)
 */
RVVM_PUBLIC void rvvm_reset_machine(rvvm_machine_t* machine, bool reset);

/**
 * Check whether machine is currently running
 */
RVVM_PUBLIC bool rvvm_machine_running(rvvm_machine_t* machine);

/**
 * Check whether machine is powered on (Even when it's paused)
 */
RVVM_PUBLIC bool rvvm_machine_powered(rvvm_machine_t* machine);

/**
 * Full machine state cleanup (Frees memory, attached devices, internal structures)
 *
 * \warning After this call, none of the handles previously attached to this machine are valid
 */
RVVM_PUBLIC void rvvm_free_machine(rvvm_machine_t* machine);

/**
 * Run the event loop in the calling thread, returns when any machine is paused or powered off
 */
RVVM_PUBLIC void rvvm_run_eventloop(void);

/** @}*/

/**
 * @defgroup rvvm_device_api RVVM Device API
 * @addtogroup rvvm_device_api
 * @{
 */

/**
 * Dummy MMIO handler: Reads zeros, ignores writes, never faults
 */
RVVM_PUBLIC bool rvvm_mmio_none(rvvm_mmio_dev_t* dev, void* dest, size_t offset, uint8_t size);

/**
 * MMIO device class information and handlers (Cleanup, reset, serialize)
 */
typedef struct {
    /** Device identifier name */
    const char* name;

    /** Called to free device state (LIFO order), dev->data is simply freed if this is NULL */
    void (*remove)(rvvm_mmio_dev_t* dev);

    /** Called periodically from event thread */
    void (*update)(rvvm_mmio_dev_t* dev);

    /** Called on machine reset */
    void (*reset)(rvvm_mmio_dev_t* dev);

    /*
     * TODO
     * void (*suspend)(rvvm_mmio_dev_t* dev, rvvm_state_t* state);
     * void (*resume)(rvvm_mmio_dev_t* dev, rvvm_state_t* state);
     */

} rvvm_mmio_type_t;

/**
 * MMIO device region description
 */
struct rvvm_mmio_dev_t {
    /** MMIO region address in machine physical memory */
    rvvm_addr_t addr;

    /** MMIO region size, zero means a placeholder */
    size_t size;

    /** Private device data, free()'d on removal if dev->type->remove is NULL */
    void* data;

    /**
     * Directly mapped host memory region.
     * If this is non-NULL, read/write handlers are only called on page dirtying
     */
    void* mapping;

    /** Owner machine handle, filled on attach */
    rvvm_machine_t* machine;

    /** Device class description */
    const rvvm_mmio_type_t* type;

    /** Called on MMIO region read if non-NULL */
    rvvm_mmio_handler_t read;

    /** Called on MMIO region write if non-NULL */
    rvvm_mmio_handler_t write;

    /** Minimum MMIO operation size allowed */
    uint8_t min_op_size;

    /** Maximum MMIO operation size allowed */
    uint8_t max_op_size;
};

/**
 * Writes data to machine physical memory
 */
RVVM_PUBLIC bool rvvm_write_ram(rvvm_machine_t* machine, rvvm_addr_t dest, const void* src, size_t size);

/**
 * Reads data from machine physical memory
 */
RVVM_PUBLIC bool rvvm_read_ram(rvvm_machine_t* machine, void* dest, rvvm_addr_t src, size_t size);

/**
 * Directly access machine physical memory (DMA)
 *
 * \return Pointer to machine DMA region, or NULL on failure
 */
RVVM_PUBLIC void* rvvm_get_dma_ptr(rvvm_machine_t* machine, rvvm_addr_t addr, size_t size);

/**
 * Get usable address for a MMIO region
 *
 * \return Usable physical memory address, which is equal to addr if the requested region is free
 */
RVVM_PUBLIC rvvm_addr_t rvvm_mmio_zone_auto(rvvm_machine_t* machine, rvvm_addr_t addr, size_t size);

/**
 * Attach MMIO device to the machine by it's description, free it's state on failure
 *
 * \param   machine   Machine to attach the device to
 * \param   mmio_desc MMIO region description, gets copied internally
 * \return  Attached MMIO region handle, or NULL on failure
 * \warning Dereferencing an attached rvvm_mmio_dev_t* should be done thread-safely using atomics/fences
 */
RVVM_PUBLIC rvvm_mmio_dev_t* rvvm_attach_mmio(rvvm_machine_t* machine, const rvvm_mmio_dev_t* mmio_desc);

/**
 * Detach (pull out) MMIO device from the owning machine, free it's state
 */
RVVM_PUBLIC void rvvm_remove_mmio(rvvm_mmio_dev_t* mmio_dev);

/**
 * Clean up MMIO device which is not attached to any machine
 */
RVVM_PUBLIC void rvvm_cleanup_mmio_desc(const rvvm_mmio_dev_t* mmio_desc);

/**
 * Get wired interrupt controller handle of this machine or NULL
 */
RVVM_PUBLIC rvvm_intc_t* rvvm_get_intc(rvvm_machine_t* machine);
RVVM_PUBLIC void         rvvm_set_intc(rvvm_machine_t* machine, rvvm_intc_t* intc);

/**
 * Get PCI rooot complex handle of this machine or NULL
 */
RVVM_PUBLIC pci_bus_t* rvvm_get_pci_bus(rvvm_machine_t* machine);
RVVM_PUBLIC void       rvvm_set_pci_bus(rvvm_machine_t* machine, pci_bus_t* pci_bus);

/**
 * Get I2C bus handle of this machine, or NULL
 */
RVVM_PUBLIC i2c_bus_t* rvvm_get_i2c_bus(rvvm_machine_t* machine);
RVVM_PUBLIC void       rvvm_set_i2c_bus(rvvm_machine_t* machine, i2c_bus_t* i2c_bus);

/**
 * Get root FDT node (For FDT device description generation)
 */
RVVM_PUBLIC rvvm_fdt_node_t* rvvm_get_fdt_root(rvvm_machine_t* machine);

/**
 * Get /soc FDT node (For FDT device description generation)
 */
RVVM_PUBLIC rvvm_fdt_node_t* rvvm_get_fdt_soc(rvvm_machine_t* machine);

/** @}*/

/**
 * @defgroup rvvm_irq_api RVVM Interrupt API
 * @addtogroup rvvm_irq_api
 * @{
 */

/**
 * Sends an MSI IRQ by issuing a 32-bit little-endian memory write
 */
RVVM_PUBLIC bool rvvm_send_msi_irq(rvvm_machine_t* machine, rvvm_addr_t addr, uint32_t val);

/**
 * Attach an MSI IRQ controller, semantically almost same as rvvm_attach_mmio(), not removable
 */
RVVM_PUBLIC bool rvvm_attach_msi_target(rvvm_machine_t* machine, const rvvm_mmio_dev_t* mmio_desc);

/**
 * Interrupt controller description
 */
struct rvvm_intc {
    void*    data;
    uint32_t last_irq;

    bool       (*send_irq)(rvvm_intc_t* intc, rvvm_irq_t irq);
    bool       (*raise_irq)(rvvm_intc_t* intc, rvvm_irq_t irq);
    bool       (*lower_irq)(rvvm_intc_t* intc, rvvm_irq_t irq);
    rvvm_irq_t (*alloc_irq)(rvvm_intc_t* intc);
    uint32_t   (*fdt_phandle)(rvvm_intc_t* intc);
    size_t     (*fdt_irq_cells)(rvvm_intc_t* intc, rvvm_irq_t irq, uint32_t* cells, size_t size);
};

/**
 * Allocate a new IRQ pin on the IRQ controller
 */
RVVM_PUBLIC rvvm_irq_t rvvm_alloc_irq(rvvm_intc_t* intc);

/**
 * Send an edge-triggered IRQ through IRQ controller pin
 */
RVVM_PUBLIC bool rvvm_send_irq(rvvm_intc_t* intc, rvvm_irq_t irq);

/**
 * Assert/lower an IRQ pin on the IRQ controller
 */
RVVM_PUBLIC bool rvvm_raise_irq(rvvm_intc_t* intc, rvvm_irq_t irq);
RVVM_PUBLIC bool rvvm_lower_irq(rvvm_intc_t* intc, rvvm_irq_t irq);

/**
 * Add interrupt-parent & interrupts fields to device FDT node
 */
RVVM_PUBLIC bool rvvm_fdt_describe_irq(struct fdt_node* node, rvvm_intc_t* intc, rvvm_irq_t irq);

/**
 * Get interrupt-parent FDT phandle of an interrupt controller
 */
RVVM_PUBLIC uint32_t rvvm_fdt_intc_phandle(rvvm_intc_t* intc);

/**
 * Get interrupts-extended FDT cells for an IRQ
 */
RVVM_PUBLIC size_t rvvm_fdt_irq_cells(rvvm_intc_t* intc, rvvm_irq_t irq, uint32_t* cells, size_t size);

/** @}*/

/**
 * @defgroup rvvm_userland_api RVVM Userland Emulation API
 * @addtogroup rvvm_userland_api
 * @{
 */

#define RVVM_REGID_X0    0
#define RVVM_REGID_F0    32
#define RVVM_REGID_PC    1024
#define RVVM_REGID_CAUSE 1025
#define RVVM_REGID_TVAL  1026

/**
 * Create a userland process context
 */
RVVM_PUBLIC rvvm_machine_t* rvvm_create_userland(const char* isa);

/**
 * Flush instruction cache for a specified memory range
 */
RVVM_PUBLIC void rvvm_flush_icache(rvvm_machine_t* machine, rvvm_addr_t addr, size_t size);

/**
 * Create userland process thread
 */
RVVM_PUBLIC rvvm_cpu_t* rvvm_create_user_thread(rvvm_machine_t* machine);

/**
 * Destroy userland process thread
 */
RVVM_PUBLIC void rvvm_free_user_thread(rvvm_cpu_t* thread);

/**
 * Run a userland thread until a trap happens
 *
 * \return Returns trap cause. PC points to faulty instruction upon return
 */
RVVM_PUBLIC rvvm_addr_t rvvm_run_user_thread(rvvm_cpu_t* thread);

/**
 * Read thread context register
 */
RVVM_PUBLIC rvvm_addr_t rvvm_read_cpu_reg(rvvm_cpu_t* thread, size_t reg_id);

/**
 * Write thread context register
 */
RVVM_PUBLIC void rvvm_write_cpu_reg(rvvm_cpu_t* thread, size_t reg_id, rvvm_addr_t reg);

/** @}*/

RVVM_EXTERN_C_END

/*
 * Deprecated definitions
 * TODO: Get rid of this!
 */

#undef PUBLIC
#define PUBLIC RVVM_PUBLIC

typedef struct rvvm_hart_t rvvm_hart_t;

#endif

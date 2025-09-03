/*
pci-vfio.c - VFIO PCI Passthrough
Copyright (C) 2022  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

// Expose pread()/pwrite()/readlink(), O_CLOEXEC
#include "feature_test.h"

#include "blk_io.h"
#include "mem_ops.h"
#include "pci-vfio.h"
#include "threading.h"
#include "utils.h"
#include "vector.h"

PUSH_OPTIMIZATION_SIZE

// Check that <linux/vfio.h> include is available
#if defined(__linux__) && defined(USE_VFIO) && !CHECK_INCLUDE(linux/vfio.h, 0)
#warning Disabling USE_VFIO as <linux/vfio.h> is unavailable
#undef USE_VFIO
#endif

#if defined(__linux__) && defined(USE_VFIO)

#include <errno.h>       // For errno
#include <fcntl.h>       // For open(), O_RDONLY, O_RDWR
#include <string.h>      // For strerror()
#include <sys/eventfd.h> // For eventfd()
#include <sys/ioctl.h>   // For ioctl()
#include <sys/mman.h>    // For mmap(), munmap(), MAP_*, PROT_*
#include <unistd.h>      // For close(), read(), write(), lseek(), pread(), pwrite(), readlink()

#include <linux/vfio.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#ifndef MAP_ANON
#define MAP_ANON MAP_ANONYMOUS
#endif

/*
 * Helper code to unbind a device from host driver and bind to vfio_pci driver
 */

static size_t vfio_rw_file(const char* path, void* rd, const void* wr, size_t size)
{
    rvfile_t* file = rvopen(path, wr ? RVFILE_WRITE : RVFILE_READ);
    size_t    ret  = rvfilesize(file);
    if (size && wr) {
        ret = rvwrite(file, wr, size, 0);
    } else if (size) {
        ret = rvread(file, rd, size, 0);
    }
    rvclose(file);
    return ret;
}

static bool vfio_unbind_driver(const char* pci_id)
{
    char path[256] = ZERO_INIT;
    rvvm_snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/driver/unbind", pci_id);
    return !!vfio_rw_file(path, NULL, pci_id, rvvm_strlen(pci_id));
}

static bool vfio_bind_vfio(const char* pci_id)
{
    char path[256]    = ZERO_INIT;
    char ven_dev[256] = ZERO_INIT;
    rvvm_snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/vendor", pci_id);
    vfio_rw_file(path, ven_dev, NULL, sizeof(ven_dev) - 1);
    if (rvvm_strfind(ven_dev, "\n")) {
        size_t len = ((size_t)rvvm_strfind(ven_dev, "\n")) - (size_t)ven_dev;
        if (len > 0 && len < 16) {
            len += rvvm_strlcpy(ven_dev + len, " ", sizeof(ven_dev) - len - 1);
            rvvm_snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/device", pci_id);
            len += vfio_rw_file(path, ven_dev + len, NULL, sizeof(ven_dev) - len - 1);
            if (rvvm_strfind(ven_dev, "\n")) {
                len          = ((size_t)rvvm_strfind(ven_dev, "\n")) - (size_t)ven_dev;
                ven_dev[len] = 0;
            }
            vfio_rw_file("/sys/bus/pci/drivers/vfio-pci/new_id", NULL, ven_dev, len);
            return !!vfio_rw_file("/sys/bus/pci/drivers/vfio-pci/bind", NULL, pci_id, rvvm_strlen(pci_id));
        }
    }
    return false;
}

static bool vfio_needs_rebind(const char* pci_id)
{
    char path[256]        = ZERO_INIT;
    char driver_path[256] = ZERO_INIT;
    rvvm_snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/driver", pci_id);
    if (readlink(path, driver_path, sizeof(driver_path)) < 0) {
        return true;
    }
    return !rvvm_strfind(driver_path, "vfio-pci");
}

static bool vfio_bind(const char* pci_id)
{
    if (vfio_needs_rebind(pci_id)) {
        rvvm_info("Unbinding the device from it's original driver");
        vfio_unbind_driver(pci_id);
        vfio_bind_vfio(pci_id);
    }
    rvvm_info("Host PCI device %s should now be bound to vfio-pci", pci_id);
    return !vfio_needs_rebind(pci_id);
}

static uint32_t vfio_get_iommu_group(const char* pci_id)
{
    char path[256]       = ZERO_INIT;
    char group_path[256] = ZERO_INIT;
    rvvm_snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/iommu_group", pci_id);
    if (readlink(path, group_path, sizeof(group_path)) < 0) {
        return -1;
    }
    const char* iommu_path = rvvm_strfind(group_path, "/kernel/iommu_groups/");
    if (iommu_path) {
        return str_to_int_dec(iommu_path + rvvm_strlen("/kernel/iommu_groups/"));
    }
    rvvm_error("Invalid VFIO IOMMU group path!");
    return -1;
}

static int vfio_open_group(const char* pci_id)
{
    char path[256] = ZERO_INIT;
    rvvm_snprintf(path, sizeof(path), "/dev/vfio/%u", vfio_get_iommu_group(pci_id));
    return open(path, O_RDWR | O_CLOEXEC);
}

static size_t vfio_read_rom(const char* pci_id, void* buffer, size_t size)
{
    char   path[256] = ZERO_INIT;
    size_t ret       = 0;
    rvvm_snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/rom", pci_id);
    vfio_rw_file(path, NULL, "1", 1);
    ret = vfio_rw_file(path, buffer, NULL, size);
    vfio_rw_file(path, NULL, "0", 1);
    return ret;
}

/*
 * VFIO attachment
 */

typedef struct {
    pci_func_t*   pci_func;
    thread_ctx_t* thread;
    uint32_t      running;
    int           eventfd;
    int           id;
} vfio_irq_t;

typedef struct {
    vector_t(vfio_irq_t*) irqs;
    pci_func_desc_t       func_desc;
    pci_func_t*           pci_func;
    int                   container;
    int                   group;
    int                   device;
    bool                  valid;
} vfio_func_t;

static void vfio_bar_unmap(rvvm_mmio_dev_t* dev)
{
    if (dev->mapping && dev->size) {
        // This BAR holds a host PCI MMIO mapping
        munmap(dev->mapping, dev->size);
    }
}

static void vfio_func_free(vfio_func_t* vfio)
{
    // Shutdown VFIO IRQ eventfd threads
    uint64_t val = 1;
    vector_foreach_back (vfio->irqs, i) {
        vfio_irq_t* irq = vector_at(vfio->irqs, i);
        atomic_store_uint32(&irq->running, false);
        UNUSED(!write(irq->eventfd, &val, sizeof(val)));
        thread_join(irq->thread);
        free(irq);
    }
    vector_free(vfio->irqs);

    if (!vfio->valid) {
        // Unmap non-attached BAR mappings
        for (size_t i = 0; i < PCI_FUNC_BARS; ++i) {
            vfio_bar_unmap(&vfio->func_desc.bar[i]);
        }
        vfio_bar_unmap(&vfio->func_desc.expansion_rom);
    }

    // Close VFIO descriptors
    if (vfio->device > 0) {
        close(vfio->device);
    }
    if (vfio->group > 0) {
        close(vfio->group);
    }
    if (vfio->container > 0) {
        close(vfio->container);
    }

    free(vfio);
}

static void vfio_bar_remove(rvvm_mmio_dev_t* dev)
{
    vfio_bar_unmap(dev);
    if (dev->data) {
        // This BAR is responsible for VFIO cleanup
        vfio_func_free(dev->data);
    }
}

static const rvvm_mmio_type_t vfio_bar_type = {
    .name   = "vfio_bar",
    .remove = vfio_bar_remove,
};

static void* vfio_irq_thread(void* data)
{
    vfio_irq_t* irq = data;
    uint64_t    val = 0;
    while (true) {
        UNUSED(!read(irq->eventfd, &val, sizeof(val)));
        if (atomic_load_uint32_relax(&irq->running)) {
            pci_send_irq(irq->pci_func, irq->id);
        } else {
            break;
        }
    }
    return NULL;
}

static void vfio_enable_irqs(vfio_func_t* vfio)
{
    vector_foreach_back (vfio->irqs, i) {
        vfio_irq_t* irq = vector_at(vfio->irqs, i);
        irq->pci_func   = vfio->pci_func;
        irq->running    = true;
        irq->thread     = thread_create(vfio_irq_thread, irq);
    }
}

static bool vfio_map_dma(vfio_func_t* vfio, rvvm_machine_t* machine, rvvm_addr_t mem_base, size_t mem_size)
{
    struct vfio_iommu_type1_dma_map dma_map = {
        .argsz = sizeof(struct vfio_iommu_type1_dma_map),
        .flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
        .iova  = mem_base,
        .size  = mem_size,
        .vaddr = (size_t)rvvm_get_dma_ptr(machine, mem_base, mem_size),
    };
    return ioctl(vfio->container, VFIO_IOMMU_MAP_DMA, &dma_map) == 0;
}

static bool vfio_setup_irqs(vfio_func_t* vfio, int irq_type)
{
    struct vfio_irq_info irq_info = {
        .argsz = sizeof(struct vfio_irq_info),
        .index = irq_type,
    };
    if (ioctl(vfio->device, VFIO_DEVICE_GET_IRQ_INFO, &irq_info)) {
        return false;
    }

    if (!irq_info.count && irq_type == VFIO_PCI_MSI_IRQ_INDEX) {
        // This device doesn't have any IRQs
        rvvm_info("VFIO device doesn't have any IRQ vectors");
        return true;
    }

    size_t               irq_count = irq_info.count;
    size_t               irq_size  = sizeof(struct vfio_irq_set) + (irq_count * sizeof(int));
    struct vfio_irq_set* irq_set   = safe_calloc(1, irq_size);
    irq_set->argsz                 = irq_size;
    irq_set->flags                 = VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER;
    irq_set->index                 = irq_type;
    irq_set->start                 = 0;
    irq_set->count                 = irq_count;

    for (size_t i = 0; i < irq_count; ++i) {
        vfio_irq_t* irq = safe_new_obj(vfio_irq_t);
        irq->eventfd    = eventfd(0, 0);
        irq->id         = i;

        // NOTE: vfio_irq_set data buffer is treated as an int array
        memcpy(((uint8_t*)irq_set->data) + (i * sizeof(int)), &irq->eventfd, sizeof(int));
        vector_push_back(vfio->irqs, irq);

        if (irq->eventfd < 0) {
            rvvm_error("Failed to create VFIO IRQ eventfd: %s", strerror(errno));
            free(irq_set);
            return false;
        }
    }

    if (ioctl(vfio->device, VFIO_DEVICE_SET_IRQS, irq_set)) {
        free(irq_set);
        return false;
    }

    free(irq_set);
    return true;
}

static bool vfio_try_attach(vfio_func_t* vfio, rvvm_machine_t* machine, const char* pci_id)
{
    vfio->container = open("/dev/vfio/vfio", O_RDWR | O_CLOEXEC);
    if (vfio->container == -1) {
        rvvm_error("Could not open /dev/vfio/vfio: %s", strerror(errno));
        return false;
    }
    vfio->group = vfio_open_group(pci_id);
    if (vfio->group == -1) {
        rvvm_error("Failed to open VFIO group: %s", strerror(errno));
        return false;
    }
    struct vfio_group_status group_status = {
        .argsz = sizeof(struct vfio_group_status),
    };
    if (ioctl(vfio->group, VFIO_GROUP_GET_STATUS, &group_status) || !(group_status.flags & VFIO_GROUP_FLAGS_VIABLE)) {
        rvvm_error("VFIO group not viable, are all group devices attached to vfio_pci module?");
        return false;
    }
    if (ioctl(vfio->group, VFIO_GROUP_SET_CONTAINER, &vfio->container)) {
        rvvm_error("Failed to set VFIO container group: %s", strerror(errno));
        return false;
    }
    if (ioctl(vfio->container, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU)) {
        rvvm_error("Failed to set up VFIO IOMMU: %s", strerror(errno));
        return false;
    }

    // Set up DMA to guest RAM
    rvvm_addr_t mem_base = rvvm_get_opt(machine, RVVM_OPT_MEM_BASE);
    rvvm_addr_t mem_size = rvvm_get_opt(machine, RVVM_OPT_MEM_SIZE);
    if (!vfio_map_dma(vfio, machine, mem_base, mem_size)) {
        // This *kinda* works around a DMA conflict with x86 MSI IRQ vector reserved region
        // More info: https://lore.kernel.org/linux-iommu/20191211082304.2d4fab45@x1.home/
        // cat /sys/kernel/iommu_groups/[iommu group]/reserved_regions

        // LAPIC MSI registers are usually placed on address 0xFEE00000, I/O APIC on address 0xFEС00000
        const rvvm_addr_t msi_x86_low = 0xFEC00000;
        const rvvm_addr_t msi_x86_end = 0xFEF00000;
        rvvm_info("Workaround reserved x86 MSI IRQ vector by splitting DMA region");
        if (mem_base < msi_x86_low) {
            size_t low_size = EVAL_MIN(mem_size, msi_x86_low - mem_base);
            if (!vfio_map_dma(vfio, machine, mem_base, low_size)) {
                rvvm_error("Failed to set up VFIO DMA: %s", strerror(errno));
                rvvm_error("This is likely caused by reserved mappings on your host overlapping guest RAM");
                return false;
            }
        }
        if (mem_base + mem_size > msi_x86_end) {
            size_t high_size = (mem_base + mem_size) - msi_x86_end;
            if (!vfio_map_dma(vfio, machine, msi_x86_end, high_size)) {
                rvvm_error("Failed to set up VFIO DMA: %s", strerror(errno));
                rvvm_error("This is likely caused by reserved mappings on your host overlapping guest RAM");
                return false;
            }
        }
    }

    vfio->device = ioctl(vfio->group, VFIO_GROUP_GET_DEVICE_FD, pci_id);
    if (vfio->device < 0) {
        rvvm_error("Failed to get VFIO device fd: %s", strerror(errno));
        return false;
    }
    struct vfio_device_info device_info = {
        .argsz = sizeof(struct vfio_device_info),
    };
    if (ioctl(vfio->device, VFIO_DEVICE_GET_INFO, &device_info)) {
        rvvm_error("Failed to get VFIO device info: %s", strerror(errno));
        return false;
    }

    // Read PCI config space of the device
    struct vfio_region_info pci_cfg_info = {
        .argsz = sizeof(struct vfio_region_info),
        .index = VFIO_PCI_CONFIG_REGION_INDEX,
    };
    if (ioctl(vfio->device, VFIO_DEVICE_GET_REGION_INFO, &pci_cfg_info)) {
        rvvm_error("Failed to get VFIO PCI config space info: %s", strerror(errno));
        return false;
    }
    uint8_t pci_config[64] = ZERO_INIT;
    if (pread(vfio->device, pci_config, 64, pci_cfg_info.offset) != 64) {
        rvvm_error("Failed to read PCI config space: %s", strerror(errno));
        return false;
    }

    vfio->func_desc.vendor_id  = read_uint16_le(pci_config);
    vfio->func_desc.device_id  = read_uint16_le(pci_config + 0x2);
    vfio->func_desc.class_code = read_uint16_le(pci_config + 0xA);
    vfio->func_desc.prog_if    = pci_config[0x9];
    vfio->func_desc.irq_pin    = pci_config[0x3D];

    // Disable INTx interrupts, Enable Bus Mastering & MMIO BAR access
    write_uint32_le(pci_config + 4, 0x406);
    if (pwrite(vfio->device, pci_config + 4, 4, pci_cfg_info.offset + 4) != 4) {
        rvvm_error("Failed to write PCI config space: %s", strerror(errno));
        return false;
    }

    // Setup IRQs
    if (!vfio_setup_irqs(vfio, VFIO_PCI_MSIX_IRQ_INDEX) && !vfio_setup_irqs(vfio, VFIO_PCI_MSI_IRQ_INDEX)) {
        rvvm_error("Failed to setup VFIO IRQs!");
        return false;
    }

    // Setup device BAR mappings
    void* data = vfio;
    for (uint32_t i = 0; i < device_info.num_regions && i <= VFIO_PCI_BAR5_REGION_INDEX; ++i) {
        struct vfio_region_info region_info = {
            .argsz = sizeof(struct vfio_region_info),
            .index = i,
        };
        if (ioctl(vfio->device, VFIO_DEVICE_GET_REGION_INFO, &region_info)) {
            rvvm_error("Failed to get VFIO BAR info: %s", strerror(errno));
            return false;
        }
        bool region_valid = region_info.size && (region_info.flags & VFIO_REGION_INFO_FLAG_MMAP);
        if ((region_info.flags & VFIO_REGION_INFO_FLAG_CAPS) && i >= 4) {
            rvvm_info("Skipping MSI-X BAR %u", i);
            region_valid = false;
        }
        if (region_valid) {
            void* bar = mmap(NULL, region_info.size, PROT_READ | PROT_WRITE, MAP_SHARED, //
                             vfio->device, region_info.offset);
            rvvm_info("VFIO PCI BAR %u: size %#.8llx, offset %#.8llx, flags %#x", i, //
                      (unsigned long long)region_info.size,                          //
                      (unsigned long long)region_info.offset, (uint32_t)region_info.flags);
            vfio->func_desc.bar[i].type = &vfio_bar_type;
            vfio->func_desc.bar[i].data = data;

            if (bar == MAP_FAILED) {
                rvvm_error("VFIO BAR mmap() failed: %s", strerror(errno));
                return false;
            }
            vfio->func_desc.bar[i].mapping = bar;
            vfio->func_desc.bar[i].size    = region_info.size;

            // Only one BAR is responsible for VFIO cleanup
            data = NULL;
        }
    }

    // Set up expansion ROM
    size_t rom_size = vfio_read_rom(pci_id, NULL, 0);
    if (rom_size) {
        void* rom = mmap(NULL, rom_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (rom != MAP_FAILED) {
            size_t tmp = 0;
            for (size_t j = 0; j < 10; ++j) {
                tmp = vfio_read_rom(pci_id, rom, rom_size);
                if (tmp) {
                    break;
                }
            }
            if (tmp) {
                vfio->func_desc.expansion_rom.data    = data;
                vfio->func_desc.expansion_rom.mapping = rom;
                vfio->func_desc.expansion_rom.size    = tmp;
                vfio->func_desc.expansion_rom.type    = &vfio_bar_type;
            } else {
                rvvm_error("Failed to read PCI ROM! Check whether CSM is disabled in host BIOS");
                munmap(rom, rom_size);
            }
        } else {
            rvvm_error("VFIO ROM BAR mmap() failed: %s", strerror(errno));
        }
    }

    // Graceful device reset, all OK
    ioctl(vfio->device, VFIO_DEVICE_RESET);
    return true;
}

PUBLIC bool pci_vfio_init_auto(rvvm_machine_t* machine, const char* pci_id)
{
    pci_bus_t* pci_bus          = rvvm_get_pci_bus(machine);
    char       long_pci_id[256] = "0000:";
    if (rvvm_strlen(pci_id) < 12) {
        // This is a shorthand pci id, like in lspci (00:01.0) - extend it to sysfs format
        rvvm_strlcpy(long_pci_id + 5, pci_id, sizeof(long_pci_id) - 5);
        pci_id = long_pci_id;
    }

    UNUSED(!system("modprobe vfio_pci")); // Just in case

    if (!vfio_bind(pci_id)) {
        rvvm_error("Failed to bind PCI device to vfio_pci kernel module");
        return false;
    }

    vfio_func_t* vfio = safe_new_obj(vfio_func_t);
    if (!vfio_try_attach(vfio, machine, pci_id)) {
        vfio_func_free(vfio);
        return false;
    }

    vfio->valid = true;

    pci_dev_t* pci_dev = pci_attach_func(pci_bus, &vfio->func_desc);
    if (pci_dev) {
        // Successfully plugged in
        vfio->pci_func = pci_get_device_func(pci_dev, 0);
        vfio_enable_irqs(vfio);
    }

    return !!pci_dev;
}

#else

#include "utils.h"

PUBLIC bool pci_vfio_init_auto(rvvm_machine_t* machine, const char* pci_id)
{
    rvvm_error("VFIO isn't available");
    UNUSED(machine);
    UNUSED(pci_id);
    return false;
}

#endif

POP_OPTIMIZATION_SIZE

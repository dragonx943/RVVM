/*
i2c-hid.c - i2c HID driver
Copyright (C) 2022  X512 <github.com/X547>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "i2c-hid.h"
#include "bit_ops.h"
#include "fdtlib.h"
#include "hid_api.h"
#include "i2c-oc.h"
#include "spinlock.h"
#include "utils.h"

PUSH_OPTIMIZATION_SIZE

#define I2C_HID_DESC_REG             1
#define I2C_HID_REPORT_REG           2
#define I2C_HID_INPUT_REG            3
#define I2C_HID_OUTPUT_REG           4
#define I2C_HID_COMMAND_REG          5
#define I2C_HID_DATA_REG             6

#define I2C_HID_COMMAND_RESET        1
#define I2C_HID_COMMAND_GET_REPORT   2
#define I2C_HID_COMMAND_SET_REPORT   3
#define I2C_HID_COMMAND_GET_IDLE     4
#define I2C_HID_COMMAND_SET_IDLE     5
#define I2C_HID_COMMAND_GET_PROTOCOL 6
#define I2C_HID_COMMAND_SET_PROTOCOL 7
#define I2C_HID_COMMAND_SET_POWER    8

enum {
    wHIDDescLength,
    bcdVersion,
    wReportDescLength,
    wReportDescRegister,
    wInputRegister,
    wMaxInputLength,
    wOutputRegister,
    wMaxOutputLength,
    wCommandRegister,
    wDataRegister,
    wVendorID,
    wProductID,
    wVersionID,
};

struct report_id_queue {
    int16_t first;
    int16_t last;
    int16_t list[256];
};

typedef struct {
    hid_dev_t* hid_dev;

    spinlock_t lock;

    // IRQ data
    rvvm_intc_t* intc;
    rvvm_irq_t   irq;

    struct report_id_queue report_id_queue;

    // i2c IO state
    bool     is_write;
    int32_t  io_offset;
    uint16_t reg;
    uint8_t  command;
    uint8_t  report_type;
    uint8_t  report_id;
    uint16_t data_size;
    uint16_t data_val;
    bool     is_reset;
} i2c_hid_t;

static void report_id_queue_init(struct report_id_queue* queue)
{
    queue->first = -1;
    queue->last  = -1;
    for (int16_t i = 0; i < 256; i++) {
        queue->list[i] = -1;
    }
}

static void report_id_queue_insert(struct report_id_queue* queue, uint8_t report_id)
{
    if (report_id == queue->last || queue->list[report_id] >= 0) {
        return;
    }
    if (queue->first < 0) {
        queue->first = report_id;
    } else {
        queue->list[queue->last] = report_id;
    }
    queue->last = report_id;
}

static int16_t report_id_queue_get(struct report_id_queue* queue)
{
    return queue->first;
}

static void report_id_queue_remove_at(struct report_id_queue* queue, uint8_t report_id)
{
    if (queue->first < 0) {
        return;
    }
    if (report_id == queue->first) {
        queue->first = queue->list[report_id];
        if (queue->first < 0) {
            queue->last = -1;
        }
    } else {
        int16_t prev = queue->first;
        while (prev >= 0 && queue->list[prev] != report_id) {
            prev = queue->list[prev];
        }
        if (prev < 0) {
            return;
        }
        queue->list[prev] = queue->list[report_id];
    }
    queue->list[report_id] = -1;
}

static void i2c_hid_reset(i2c_hid_t* i2c_hid, bool is_init)
{
    report_id_queue_init(&i2c_hid->report_id_queue);
    i2c_hid->reg         = I2C_HID_INPUT_REG;
    i2c_hid->command     = 0;
    i2c_hid->report_type = 0;
    i2c_hid->report_id   = 0;
    i2c_hid->is_reset    = !is_init;

    if (i2c_hid->hid_dev->reset) {
        i2c_hid->hid_dev->reset(i2c_hid->hid_dev->dev);
    }

    if (!is_init) {
        rvvm_raise_irq(i2c_hid->intc, i2c_hid->irq);
    }
}

static void i2c_hid_input_available(void* host, uint8_t report_id)
{
    i2c_hid_t* i2c_hid = (i2c_hid_t*)host;
    spin_lock(&i2c_hid->lock);
    if (!i2c_hid->is_reset) {
        report_id_queue_insert(&i2c_hid->report_id_queue, report_id);
        rvvm_raise_irq(i2c_hid->intc, i2c_hid->irq);
    }
    spin_unlock(&i2c_hid->lock);
}

static bool i2c_hid_read_data_size(i2c_hid_t* i2c_hid, uint32_t offset, uint8_t val)
{
    if (offset < 2) {
        i2c_hid->data_size = bit_replace(i2c_hid->data_size, offset * 8, 8, val);
    }
    if (offset >= 1 && offset >= i2c_hid->data_size) {
        return false;
    }
    return true;
}

static void i2c_hid_read_report(i2c_hid_t* i2c_hid, uint8_t report_type, uint8_t report_id, uint32_t offset,
                                uint8_t* val)
{
    i2c_hid->hid_dev->read_report(i2c_hid->hid_dev->dev, report_type, report_id, offset, val);
    if (offset < 2) {
        i2c_hid->data_size = bit_replace(i2c_hid->data_size, offset * 8, 8, *val);
    }
    if (report_type == REPORT_TYPE_INPUT && offset >= 1
        && offset == (uint32_t)(i2c_hid->data_size > 2 ? i2c_hid->data_size - 1 : 1)) {
        report_id_queue_remove_at(&i2c_hid->report_id_queue, report_id);
        if (report_id_queue_get(&i2c_hid->report_id_queue) >= 0) {
            rvvm_raise_irq(i2c_hid->intc, i2c_hid->irq);
        } else {
            rvvm_lower_irq(i2c_hid->intc, i2c_hid->irq);
        }
    }
}

static bool i2c_hid_write_report(i2c_hid_t* i2c_hid, uint8_t report_type, uint8_t report_id, uint32_t offset,
                                 uint8_t val)
{
    if (!i2c_hid_read_data_size(i2c_hid, offset, val)) {
        return false;
    }
    i2c_hid->hid_dev->write_report(i2c_hid->hid_dev->dev, report_type, report_id, offset, val);
    return true;
}

static uint8_t i2c_hid_read_reg(i2c_hid_t* i2c_hid, uint16_t reg, uint32_t offset)
{
    switch (reg) {
        case I2C_HID_DESC_REG: {
            uint16_t field_val;
            switch (offset / 2) {
                case wHIDDescLength:
                    field_val = 0x1e;
                    break;
                case bcdVersion:
                    field_val = 0x0100;
                    break;
                case wReportDescLength:
                    field_val = i2c_hid->hid_dev->report_desc_size;
                    break;
                case wReportDescRegister:
                    field_val = I2C_HID_REPORT_REG;
                    break;
                case wInputRegister:
                    field_val = I2C_HID_INPUT_REG;
                    break;
                case wMaxInputLength:
                    field_val = i2c_hid->hid_dev->max_input_size;
                    break;
                case wOutputRegister:
                    field_val = I2C_HID_OUTPUT_REG;
                    break;
                case wMaxOutputLength:
                    field_val = i2c_hid->hid_dev->max_output_size;
                    break;
                case wCommandRegister:
                    field_val = I2C_HID_COMMAND_REG;
                    break;
                case wDataRegister:
                    field_val = I2C_HID_DATA_REG;
                    break;
                case wVendorID:
                    field_val = i2c_hid->hid_dev->vendor_id;
                    break;
                case wProductID:
                    field_val = i2c_hid->hid_dev->product_id;
                    break;
                case wVersionID:
                    field_val = i2c_hid->hid_dev->version_id;
                    break;
                default:
                    field_val = 0;
                    break;
            }
            return bit_cut(field_val, 8 * (offset % 2), 8);
        }
        case I2C_HID_REPORT_REG:
            if (offset < i2c_hid->hid_dev->report_desc_size) {
                return i2c_hid->hid_dev->report_desc[offset];
            }
            break;
        case I2C_HID_INPUT_REG: {
            int16_t report_id = report_id_queue_get(&i2c_hid->report_id_queue);
            if (report_id < 0) {
                rvvm_lower_irq(i2c_hid->intc, i2c_hid->irq);
                return 0;
            }
            uint8_t val = 0;
            i2c_hid_read_report(i2c_hid, REPORT_TYPE_INPUT, report_id, offset, &val);
            return val;
        }
        case I2C_HID_DATA_REG:
            switch (i2c_hid->command) {
                case I2C_HID_COMMAND_GET_REPORT: {
                    uint8_t val = 0;
                    i2c_hid_read_report(i2c_hid, i2c_hid->report_type, i2c_hid->report_id, offset, &val);
                    return val;
                }
                case I2C_HID_COMMAND_GET_IDLE: {
                    uint16_t field_val = 0;
                    switch (offset / 2) {
                        case 0:
                            field_val = 4;
                            break;
                        case 1:
                            if (i2c_hid->hid_dev->get_idle) {
                                i2c_hid->hid_dev->get_idle(i2c_hid->hid_dev->dev, i2c_hid->report_id, &field_val);
                            }
                            break;
                    }
                    return bit_cut(field_val, 8 * (offset % 2), 8);
                }
                case I2C_HID_COMMAND_GET_PROTOCOL: {
                    uint16_t field_val = 0;
                    switch (offset / 2) {
                        case 0:
                            field_val = 4;
                            break;
                        case 1:
                            if (i2c_hid->hid_dev->get_protocol) {
                                i2c_hid->hid_dev->get_protocol(i2c_hid->hid_dev->dev, &field_val);
                            }
                            break;
                    }
                    return bit_cut(field_val, 8 * (offset % 2), 8);
                }
            }
            break;
    }
    return 0;
}

static bool i2c_hid_write_reg(i2c_hid_t* i2c_hid, uint16_t reg, uint32_t offset, uint8_t val)
{
    switch (reg) {
        case I2C_HID_OUTPUT_REG:
            return i2c_hid_write_report(i2c_hid, REPORT_TYPE_OUTPUT, 0, offset, val);
        case I2C_HID_COMMAND_REG:
            switch (offset) {
                case 0:
                    i2c_hid->report_id   = bit_cut(val, 0, 4);
                    i2c_hid->report_type = bit_cut(val, 4, 2);
                    return true;
                case 1:
                    i2c_hid->command = bit_cut(val, 0, 4);
                    // fprintf(stderr, "  command: %u\n", i2c_hid->command);
                    if (i2c_hid->report_id == 0xF) {
                        return true;
                    }
                    break;
                case 2:
                    i2c_hid->report_id = val;
                    break;
            }
            switch (i2c_hid->command) {
                case I2C_HID_COMMAND_SET_IDLE:
                    if (i2c_hid->data_size == 4 && i2c_hid->hid_dev->set_idle) {
                        i2c_hid->hid_dev->set_idle(i2c_hid->hid_dev->dev, i2c_hid->report_id, i2c_hid->data_val);
                    }
                    break;
                case I2C_HID_COMMAND_SET_PROTOCOL:
                    if (i2c_hid->data_size == 4 && i2c_hid->hid_dev->set_protocol) {
                        i2c_hid->hid_dev->set_protocol(i2c_hid->hid_dev->dev, i2c_hid->data_val);
                    }
                    break;
                case I2C_HID_COMMAND_SET_POWER:
                    if (i2c_hid->hid_dev->set_power) {
                        i2c_hid->hid_dev->set_power(i2c_hid->hid_dev->dev, i2c_hid->report_id % 4);
                    }
                    break;
            }
            break;
        case I2C_HID_DATA_REG:
            if (i2c_hid->command == I2C_HID_COMMAND_SET_REPORT) {
                return i2c_hid_write_report(i2c_hid, i2c_hid->report_type, i2c_hid->report_id, offset, val);
            } else {
                if (!i2c_hid_read_data_size(i2c_hid, offset, val)) {
                    return false;
                }
                if (offset / 2 == 1) {
                    i2c_hid->data_val = bit_replace(i2c_hid->data_val, offset * 8, 8, val);
                }
                return true;
            }
            break;
    }
    return false;
}

static bool i2c_hid_start(void* dev, bool is_write)
{
    i2c_hid_t* i2c_hid = (i2c_hid_t*)dev;
    spin_lock(&i2c_hid->lock);
    // fprintf(stderr, "i2c_hid_start(is_write: %d)\n", is_write);
    i2c_hid->is_write  = is_write;
    i2c_hid->io_offset = 0;
    spin_unlock(&i2c_hid->lock);
    return true;
}

static bool i2c_hid_write(void* dev, uint8_t byte)
{
    i2c_hid_t* i2c_hid = (i2c_hid_t*)dev;
    spin_lock(&i2c_hid->lock);
    // fprintf(stderr, "i2c_hid_write, io_offset: %u, val: %#02x\n", i2c_hid->io_offset, byte);
    switch (i2c_hid->io_offset) {
        case 0:
        case 1:
            i2c_hid->reg = bit_replace(i2c_hid->reg, i2c_hid->io_offset * 8, 8, byte);
            i2c_hid->io_offset++;
            // if (i2c_hid->io_offset == 2) {
            //   fprintf(stderr, "  reg: %u\n", i2c_hid->reg);
            // }
            break;
        default:
            if (i2c_hid_write_reg(i2c_hid, i2c_hid->reg, i2c_hid->io_offset - 2, byte)) {
                i2c_hid->io_offset++;
            } else {
                i2c_hid->io_offset = 0;
            }
            break;
    }
    spin_unlock(&i2c_hid->lock);
    return true;
}

static bool i2c_hid_read(void* dev, uint8_t* byte)
{
    i2c_hid_t* i2c_hid = (i2c_hid_t*)dev;
    spin_lock(&i2c_hid->lock);
    // fprintf(stderr, "i2c_hid_read, io_offset: %u\n", i2c_hid->io_offset);
    *byte = i2c_hid_read_reg(i2c_hid, i2c_hid->reg, i2c_hid->io_offset++);
    // fprintf(stderr, "  val: %#02x\n", *byte);
    spin_unlock(&i2c_hid->lock);
    return true;
}

static void i2c_hid_stop(void* dev)
{
    i2c_hid_t* i2c_hid = (i2c_hid_t*)dev;
    spin_lock(&i2c_hid->lock);
    // fprintf(stderr, "i2c_hid_stop\n");
    i2c_hid->is_reset = false;
    if (i2c_hid->command == I2C_HID_COMMAND_RESET) {
        i2c_hid_reset(i2c_hid, false);
    }
    i2c_hid->reg       = I2C_HID_INPUT_REG;
    i2c_hid->command   = 0;
    i2c_hid->data_size = 0;
    spin_unlock(&i2c_hid->lock);
}

static void i2c_hid_remove(void* dev)
{
    i2c_hid_t* i2c_hid = (i2c_hid_t*)dev;
    if (i2c_hid->hid_dev->remove) {
        i2c_hid->hid_dev->remove(i2c_hid->hid_dev->dev);
    }
    free(i2c_hid);
}

static void i2c_hid_init(rvvm_machine_t* machine, i2c_bus_t* bus, uint16_t addr, rvvm_intc_t* intc, rvvm_irq_t irq,
                         hid_dev_t* hid_dev)
{
    UNUSED(machine);
    i2c_hid_t* i2c_hid = safe_new_obj(i2c_hid_t);

    i2c_dev_t i2c_dev = {.addr   = addr,
                         .data   = i2c_hid,
                         .start  = i2c_hid_start,
                         .write  = i2c_hid_write,
                         .read   = i2c_hid_read,
                         .stop   = i2c_hid_stop,
                         .remove = i2c_hid_remove};
    i2c_dev.addr      = i2c_attach_dev(bus, &i2c_dev);

    i2c_hid->intc = intc;
    i2c_hid->irq  = irq;

    i2c_hid->hid_dev         = hid_dev;
    hid_dev->host            = i2c_hid;
    hid_dev->input_available = i2c_hid_input_available;

    i2c_hid_reset(i2c_hid, true);

#ifdef USE_FDT
    struct fdt_node* i2c_vdd = fdt_node_find(rvvm_get_fdt_root(machine), "i2c_vdd");
    if (i2c_vdd == NULL) {
        i2c_vdd = fdt_node_create("i2c_vdd");
        fdt_node_add_prop_str(i2c_vdd, "compatible", "regulator-fixed");
        fdt_node_add_prop_str(i2c_vdd, "regulator-name", "i2c_vdd");
        fdt_node_add_child(rvvm_get_fdt_root(machine), i2c_vdd);
    }

    struct fdt_node* i2c_fdt = fdt_node_create_reg("i2c", i2c_dev.addr);
    fdt_node_add_prop_u32(i2c_fdt, "reg", i2c_dev.addr);
    fdt_node_add_prop_str(i2c_fdt, "compatible", "hid-over-i2c");
    fdt_node_add_prop_u32(i2c_fdt, "hid-descr-addr", I2C_HID_DESC_REG);
    fdt_node_add_prop_u32(i2c_fdt, "vdd-supply", fdt_node_get_phandle(i2c_vdd));
    fdt_node_add_prop_u32(i2c_fdt, "vddl-supply", fdt_node_get_phandle(i2c_vdd));
    fdt_node_add_prop(i2c_fdt, "wakeup-source", NULL, 0);
    rvvm_fdt_describe_irq(i2c_fdt, intc, irq);
    fdt_node_add_child(i2c_bus_fdt_node(bus), i2c_fdt);
#endif
}

PUBLIC void i2c_hid_init_auto(rvvm_machine_t* machine, hid_dev_t* hid_dev)
{
    i2c_bus_t*   bus  = rvvm_get_i2c_bus(machine);
    rvvm_intc_t* intc = rvvm_get_intc(machine);
    i2c_hid_init(machine, bus, I2C_AUTO_ADDR, intc, rvvm_alloc_irq(intc), hid_dev);
}

POP_OPTIMIZATION_SIZE

/*
rtl8169.c - Realtek RTL8169 (8168B) NIC
Copyright (C) 2022  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifdef USE_NET

#include "rtl8169.h"
#include "tap_api.h"
#include "pci-bus.h"
#include "mem_ops.h"
#include "bit_ops.h"
#include "spinlock.h"
#include "utils.h"

/*
 * See https://people.freebsd.org/~wpaul/RealTek/RTL8111B_8168B_Registers_DataSheet_1.0.pdf
 */

/*
 * RTL8169 Registers
 */

#define RTL8169_REG_IDR0  0x00 // ID Register 0-3 (For MAC Address)
#define RTL8169_REG_IDR4  0x04 // ID Register 4-5
#define RTL8169_REG_MAR0  0x08 // Multicast Address Register 0-3
#define RTL8169_REG_MAR4  0x0C // Multicast Address Register 4-7
#define RTL8169_REG_DTCR1 0x10 // Dump Tally Counter Command Register (64-byte alignment)
#define RTL8169_REG_DTCR2 0x14
#define RTL8169_REG_TXDA1 0x20 // Transmit Descriptors Address (64-bit, 256-byte alignment)
#define RTL8169_REG_TXDA2 0x24
#define RTL8169_REG_TXHA1 0x28 // Transmit High Priority Descriptors Address (64-bit, 256-byte alignment)
#define RTL8169_REG_TXHA2 0x2C
#define RTL8169_REG_CR32  0x34 // Command Register (Aligned to 32-bit boundary for ease)
#define RTL8169_REG_CR    0x37 // Command Register (Actual offset in spec)
#define RTL8169_REG_TPOLL 0x38 // Transmit Priority Polling
#define RTL8169_REG_IMR   0x3C // Interrupt Mask
#define RTL8169_REG_ISR   0x3E // Interrupt Status
#define RTL8169_REG_TCR   0x40 // Transmit Configuration Register
#define RTL8169_REG_RCR   0x44 // Receive Configuration Register
#define RTL8169_REG_TCTR  0x48 // Timer Counter Register
#define RTL8169_REG_MPC   0x4C // Missed Packet Counter
#define RTL8169_REG_9346  0x50 // 93C46 Command Register, CFG 0-2
#define RTL8169_REG_CFG3  0x54 // Configuration Register 3-5
#define RTL8169_REG_TINT  0x58 // Timer Interrupt Register
#define RTL8169_REG_PHYAR 0x60 // PHY Access Register
#define RTL8169_REG_TBIR0 0x64 // TBI Control and Status Register
#define RTL8169_REG_TBANR 0x68 // TBI Auto-Negotiation Advertisement Register
#define RTL8169_REG_PHYS  0x6C // PHY Status Register
#define RTL8169_REG_ERIDR 0x70 // ERI GPHY Data Register, rtl8168b specific
#define RTL8169_REG_ERIAR 0x74 // ERI GPHY Access Register, rtl8168b specific
#define RTL8169_REG_EPHAR 0x80 // EPHY GPHY Data Register, rtl816cp specific
#define RTL8169_REG_OCPDR 0xB0 // OCP GPHY Data Register, rtl8168dp specific
#define RTL8169_REG_OCPAR 0xB4 // OCP GPHY Access Register, rtl8168dp specific
#define RTL8169_REG_RMS32 0xD8 // RX Packet Maximum Size (Aligned to 32-bit boundary for ease)
#define RTL8169_REG_RMS   0xDA // RX Packet Maximum Size (Actual offset in spec)
#define RTL8169_REG_CPCR  0xE0 // C+ Command Register
#define RTL8169_REG_RXDA1 0xE4 // Receive Descriptor Address (64-bit, 256-byte alignment)
#define RTL8169_REG_RXDA2 0xE8
#define RTL8169_REG_MTPS  0xEC // TX Packet Maximum Size

/*
 * RTL8169 Register Values
 */

// Command Register bits
#define RTL8169_CR_TE  0x04 // Transmitter Enable
#define RTL8169_CR_RE  0x08 // Receiver Enable
#define RTL8169_CR_RW  0x0C // R/W Register bits mask
#define RTL8169_CR_RST 0x10 // Reset

// Transmit Polling bits
#define RTL8169_TPOLL_FSW 0x01 // Forced Software Interrupt
#define RTL8169_TPOLL_NPQ 0x40 // Normal Priority Queue Polling
#define RTL8169_TPOLL_HPQ 0x80 // High Priority Queue Polling

// Interrupt Status bits
#define RTL8169_IRQ_ROK  0x00 // Receive OK
#define RTL8169_IRQ_RER  0x01 // Receiver Error
#define RTL8169_IRQ_TOK  0x02 // Transmit OK
#define RTL8169_IRQ_TER  0x03 // Transmitter Error
#define RTL8169_IRQ_RDU  0x04 // RX Descriptor Unavailable
#define RTL8169_IRQ_LCG  0x05 // Link Change
#define RTL8169_IRQ_FOVW 0x06 // RX FIFO Overflow (For RX ring overflow, use RDU)
#define RTL8169_IRQ_TDU  0x07 // TX Descriptor Unavailable
#define RTL8169_IRQ_SWI  0x10 // Software Interrupt

// Transmit Configuration bits
#define RTL8169_TCR_IFG       0x03000000 // 96ns for 1Gbit
#define RTL8169_TCR_NOCRC     0x00010000 // No CRC applied for TX
#define RTL8169_TCR_MXDMA     0x00000700 // Unlimited DMA burst
#define RTL8169_TCR_DEFAULT   (RTL8169_TCR_IFG | RTL8169_TCR_MXDMA)

// RTL8169 Family Models (XID)
#define RTL8169_XID_RTL8169S  0x00800000 // RTL_GIGA_MAC_VER_02
#define RTL8169_XID_RTL8168B  0x38000000 // RTL_GIGA_MAC_VER_17
#define RTL8169_XID_RTL8168CP 0x3C800000 // RTL_GIGA_MAC_VER_24
#define RTL8169_XID_RTL8168DP 0x28B00000 // RTL_GIGA_MAC_VER_31
#define RTL8169_XID_RTL8168EP 0x50200000 // RTL_GIGA_MAC_VER_51
#define RTL8169_XID_RTL8117   0x54B00000 // RTL_GIGA_MAC_VER_53

// Receive Configuration bits
#define RTL8169_RCR_AAP   0x0001 // Accept All Packets with Destination Address
#define RTL8169_RCR_APM   0x0002 // Accept Physical Match Packets
#define RTL8169_RCR_AMP   0x0004 // Accept Multicast Packets
#define RTL8169_RCR_ABP   0x0008 // Accept Broadcast Packets
#define RTL8169_RCR_9356  0x0040 // EEPROM is 9356
#define RTL8169_RCR_MXDMA 0x0700 // Unlimited DMA Burst
#define RTL8169_RCR_RXFTH 0xE000 // No Rx threshold
#define RTL8169_RCR_DEFAULT 0xE70F // Default RX config

// PHY Access bits
#define RTL8169_PHY_DATA 0xFFFF

// PHY Status bits
#define RTL8169_PHY_STATUS 0x73 // Link up, full duplex, 1Gbit, flow control ON

// RX Packet Maximum Size
#define RTL8169_RMS  0x1FFF // RX Packet Maximum Size: 8191

// TX Packet Maximum Size
#define RTL8169_MTPS 0x3B // TX Packet Maximum Size: 7552

// C+ Command bits
#define RTL8169_CPCR_RXCSUM 0x20 // Receive Checksum Offload Enabled
#define RTL8169_CPCR_RXVLAN 0x40 // Receive VLAN De-tagging Enabled

/*
 * Descriptor flags
 */

// Common TX/RX flags
#define RTL8169_DESC_OWN 0x80000000 // Descriptor owned by RTL8169
#define RTL8169_DESC_EOR 0x40000000 // End of Descriptor Ring
#define RTL8169_DESC_FS  0x20000000 // First Segment Descriptor
#define RTL8169_DESC_LS  0x10000000 // Last Segment Descriptor

// TX Descriptor flags
#define RTL8169_DESC_LGSEN 0x08000000 // Enable Large Send Offload

// TX Status flags mask
#define RTL8169_DESC_TXSTA (RTL8169_DESC_EOR | RTL8169_DESC_FS | RTL8169_DESC_LS)

// RX Descriptor flags
#define RTL8169_DESC_PAM  0x04000000 // Physical Address Matched
#define RTL8169_DESC_BAR  0x02000000 // Broadcast Address Received
#define RTL8169_DESC_RSV1 0x00800000 // Reserved (Always 1)
#define RTL8169_DESC_UDP  0x00040000 // UDP/IP Received
#define RTL8169_DESC_TCP  0x00020000 // TCP/IP Received

// Generic RX packet flags (FS & LS, PAM, TCP/IP received)
#define RTL8169_DESC_RXSTA 0x34820000

/*
 * PHY registers
 */

#define RTL8169_PHY_BMCR  0x00
#define RTL8169_PHY_BMSR  0x01
#define RTL8169_PHY_ID1   0x02
#define RTL8169_PHY_ID2   0x03
#define RTL8169_PHY_GBCR  0x09
#define RTL8169_PHY_GBSR  0x0A
#define RTL8169_PHY_GBESR 0x0F

/*
 * EEPROM pins
 */

#define RTL8169_EEPROM_DOU 0x01 // EEPROM Data out
#define RTL8169_EEPROM_DIN 0x02 // EEPROM Data in
#define RTL8169_EEPROM_CLK 0x04 // EEPROM Clock
#define RTL8169_EEPROM_SEL 0x08 // EEPROM Chip select
#define RTL8169_EEMODE_PRG 0x80 // EEPROM Programming mode

/*
 * Size constants
 */

#define RTL8169_MAX_FIFO_SIZE 1024
#define RTL8169_MAC_SIZE 6
#define RTL8169_MAX_PKT_SIZE 0x4000

typedef struct {
    spinlock_t lock;
    uint32_t   addr;
    uint32_t   addr_h;
    uint32_t   index;
} rtl8169_ring_t;

typedef struct {
    uint8_t  pins;
    uint8_t  addr;
    uint16_t word;
    bitcnt_t cur_bit;
    bool     addr_ok;
} at93c56_state_t;

typedef struct {
    pci_func_t* pci_func;
    tap_dev_t*  tap;

    // RTL8169 EEPROM (Used to retreive MAC address)
    spinlock_t mac_lock;
    at93c56_state_t eeprom;

    // RX / TX / High priority TX queues
    rtl8169_ring_t rx;
    rtl8169_ring_t tx;
    rtl8169_ring_t txp;

    uint32_t cr;
    uint32_t imr;
    uint32_t isr;
    uint32_t phydr;
    uint32_t phyar;

    uint8_t  mac[RTL8169_MAC_SIZE];

    // Descriptor segmentation reassembly buffer
    uint8_t  seg_buff[RTL8169_MAX_PKT_SIZE];
    size_t   seg_size;
} rtl8169_dev_t;

static void rtl8169_update_irqs(rtl8169_dev_t* rtl8169)
{
    uint32_t isr = atomic_load_uint32_relax(&rtl8169->isr);
    uint32_t imr = atomic_load_uint32_relax(&rtl8169->imr);
    if (isr & imr) {
        pci_raise_irq(rtl8169->pci_func, 0);
    } else {
        pci_lower_irq(rtl8169->pci_func, 0);
    }
}

static void rtl8169_interrupt(rtl8169_dev_t* rtl8169, size_t irq)
{
    uint32_t irqs = 1U << irq;
    atomic_or_uint32(&rtl8169->isr, irqs);
    if (irqs & atomic_load_uint32_relax(&rtl8169->imr)) {
        pci_raise_irq(rtl8169->pci_func, 0);
    }
}

static inline rvvm_addr_t rtl8169_ring_addr(const rtl8169_ring_t* ring)
{
    return atomic_load_uint32_relax(&ring->addr)
    | (((uint64_t)atomic_load_uint32_relax(&ring->addr_h)) << 32);
}

static void rtl8169_reset_ring(rtl8169_ring_t* ring)
{
    atomic_store_uint32_relax(&ring->addr, 0);
    atomic_store_uint32_relax(&ring->addr_h, 0);
    atomic_store_uint32_relax(&ring->index, 0);
}

static void rtl8169_reset(rvvm_mmio_dev_t* dev)
{
    rtl8169_dev_t* rtl8169 = dev->data;

    // Reset EEPROM
    spin_lock(&rtl8169->mac_lock);
    memset(&rtl8169->eeprom, 0, sizeof(at93c56_state_t));
    spin_unlock(&rtl8169->mac_lock);

    // Reset rings
    rtl8169_reset_ring(&rtl8169->rx);
    rtl8169_reset_ring(&rtl8169->tx);
    rtl8169_reset_ring(&rtl8169->txp);

    // Reset registers
    atomic_store_uint32_relax(&rtl8169->cr, 0);
    atomic_store_uint32_relax(&rtl8169->imr, 0);
    atomic_store_uint32_relax(&rtl8169->isr, 0);
    atomic_store_uint32_relax(&rtl8169->phydr, 0);
    atomic_store_uint32_relax(&rtl8169->phyar, 0);

    rtl8169_update_irqs(rtl8169);
}

static uint16_t rtl8169_read_phy(uint32_t reg)
{
    switch (reg) {
        case RTL8169_PHY_BMCR:
            return 0x1140; // Full-duplex 1Gbps, Auto-Negotiation Enabled
        case RTL8169_PHY_BMSR:
            return 0x796D; // Link is up; Supports GBESR
        case RTL8169_PHY_ID1:
            return 0x001C; // Realtek
        case RTL8169_PHY_ID2:
            return 0xC910; // Generic 1 GBps PHY
        case RTL8169_PHY_GBCR:
            return 0x0200; // Advertise 1000BASE-T Full duplex
        case RTL8169_PHY_GBSR:
            return 0x3800; // Link partner is capable of 1000BASE-T Full duplex
        case RTL8169_PHY_GBESR:
            return 0xA000; // 1000BASE-T Full duplex capable
    }
    return 0;
}

static void rtl8169_handle_phy(rtl8169_dev_t* rtl8169, uint32_t cmd)
{
    uint32_t reg = (cmd >> 16) & 0x1F;
    uint32_t val = ((cmd & 0xFFFF0000) ^ 0x80000000) | rtl8169_read_phy(reg);
    atomic_store_uint32_relax(&rtl8169->phyar, val);
}

static void rtl8169_handle_eri_phy(rtl8169_dev_t* rtl8169, uint32_t cmd)
{
    uint32_t reg = cmd & 0xFFF;
    uint32_t val = ((cmd & 0xFFFF0000) ^ 0x80000000);
    atomic_store_uint32_relax(&rtl8169->phydr, rtl8169_read_phy(reg));
    atomic_store_uint32_relax(&rtl8169->phyar, val);
}

static void rtl8169_handle_ocp_phy(rtl8169_dev_t* rtl8169, uint32_t cmd)
{
    uint32_t reg = (cmd >> 16) & 0x1F;
    uint32_t val = ((cmd & 0xFFFF0000) ^ 0x80000000);
    atomic_store_uint32_relax(&rtl8169->phydr, rtl8169_read_phy(reg));
    atomic_store_uint32_relax(&rtl8169->phyar, val);
}

static uint16_t rtl8169_93c56_read_word(rtl8169_dev_t* rtl8169, uint8_t addr)
{
    switch (addr) {
        case 0x0: // Device ID
            return 0x8129;
        case 0x7: // MAC words
        case 0x8:
        case 0x9:
            tap_get_mac(rtl8169->tap, rtl8169->mac);
            return read_uint16_le_m(rtl8169->mac + ((addr - 7) << 1));
    }
    return 0;
}

static void rtl8169_93c56_write_pins(rtl8169_dev_t* rtl8169, uint8_t pins)
{
    spin_lock(&rtl8169->mac_lock);
    if (pins & RTL8169_EEMODE_PRG) {
        if ((pins & RTL8169_EEPROM_CLK) && !(rtl8169->eeprom.pins & RTL8169_EEPROM_CLK)) {
            // Clock pulled high
            if (rtl8169->eeprom.addr_ok) {
                // Push data bits
                if (rtl8169->eeprom.cur_bit == 0) {
                    rtl8169->eeprom.word = rtl8169_93c56_read_word(rtl8169, rtl8169->eeprom.addr);
                }
                if (rtl8169->eeprom.word & (0x8000 >> rtl8169->eeprom.cur_bit)) {
                    pins |= RTL8169_EEPROM_DOU;
                } else {
                    pins &= ~RTL8169_EEPROM_DOU;
                }
                if (rtl8169->eeprom.cur_bit++ >= 15) {
                    rtl8169->eeprom.cur_bit = 0;
                    rtl8169->eeprom.addr++;
                }
            } else {
                // Get starting addr, ignore command (Act as readonly eeprom)
                if (rtl8169->eeprom.cur_bit >= 3) {
                    rtl8169->eeprom.addr <<= 1;
                    if (pins & RTL8169_EEPROM_DIN) rtl8169->eeprom.addr |= 1;
                }
                if (rtl8169->eeprom.cur_bit++ >= 11) {
                    rtl8169->eeprom.cur_bit = 0;
                    rtl8169->eeprom.addr_ok = true;
                }
            }
        }
        if (!(pins & RTL8169_EEPROM_SEL)) {
            // End of transfer, request addr next time
            rtl8169->eeprom.addr_ok = false;
            rtl8169->eeprom.addr = 0;
            rtl8169->eeprom.cur_bit = 0;
        }
    }
    rtl8169->eeprom.pins = pins;
    spin_unlock(&rtl8169->mac_lock);
}

static uint8_t rtl8169_93c56_read_pins(rtl8169_dev_t* rtl8169)
{
    spin_lock(&rtl8169->mac_lock);
    uint8_t ret = rtl8169->eeprom.pins;
    spin_unlock(&rtl8169->mac_lock);
    return ret;
}

static bool rtl8169_feed_rx(void* net_dev, const void* data, size_t size)
{
    rtl8169_dev_t* rtl8169 = net_dev;
    if (likely(atomic_load_uint32_relax(&rtl8169->cr) & RTL8169_CR_RE)) {
        // Receiver enabled
        rvvm_addr_t ring_addr = rtl8169_ring_addr(&rtl8169->rx);
        spin_lock(&rtl8169->rx.lock);
        uint8_t* desc = pci_get_dma_ptr(rtl8169->pci_func, ring_addr + (rtl8169->rx.index << 4), 0x10);
        if (unlikely(!desc)) {
            // RX descriptor DMA error
            spin_unlock(&rtl8169->rx.lock);
            rvvm_debug("rtl8169 RX descriptor DMA error");
            return false;
        }

        uint32_t flags = read_uint32_le(desc);
        if (unlikely(!(flags & RTL8169_DESC_OWN))) {
            // RX descriptor unavailable
            spin_unlock(&rtl8169->rx.lock);
            rtl8169_interrupt(rtl8169, RTL8169_IRQ_RDU);
            return false;
        }

        rvvm_addr_t packet_addr = read_uint64_le(desc + 8);
        size_t packet_size = flags & 0x3FFF;
        uint8_t* packet_ptr = pci_get_dma_ptr(rtl8169->pci_func, packet_addr, packet_size);
        if (likely(packet_ptr && packet_size >= size + 4)) {
            memcpy(packet_ptr, data, size);
            memset(packet_ptr + size, 0, 4); // Append fake CRC32
        } else {
            // Keep going as if nothing happened, maybe next descriptor will be OK
            rvvm_debug("rtl8169 RX packet DMA error");
        }

        rtl8169->rx.index++;
        if ((flags & RTL8169_DESC_EOR) || rtl8169->rx.index >= RTL8169_MAX_FIFO_SIZE) {
            rtl8169->rx.index = 0;
        }

        atomic_store_uint32_le(desc, (flags & RTL8169_DESC_EOR) | RTL8169_DESC_RXSTA | (size + 4));
        spin_unlock(&rtl8169->rx.lock);
        rtl8169_interrupt(rtl8169, RTL8169_IRQ_ROK);
        return true;
    }
    return false;
}

static void rtl8169_tx_doorbell(rtl8169_dev_t* rtl8169, rtl8169_ring_t* ring)
{
    if (likely(atomic_load_uint32_relax(&rtl8169->cr) & RTL8169_CR_TE)) {
        // Transmitter enabled
        rvvm_addr_t ring_addr = rtl8169_ring_addr(ring);
        bool tx_irq = false;
        spin_lock(&ring->lock);
        while (true) {
            uint8_t* desc = pci_get_dma_ptr(rtl8169->pci_func, ring_addr + (ring->index << 4), 0x10);
            if (unlikely(!desc)) {
                // TX descriptor DMA error
                rvvm_debug("rtl8169 TX descriptor DMA error");
                break;
            }
            uint32_t flags = read_uint32_le(desc);
            if (!(flags & RTL8169_DESC_OWN)) {
                // Nothing more to transmit
                break;
            }

            rvvm_addr_t addr = read_uint64_le(desc + 8);
            size_t size = flags & 0x3FFF;
            const void* ptr = pci_get_dma_ptr(rtl8169->pci_func, addr, size);

            if (likely(ptr)) {
                if ((flags & RTL8169_DESC_FS) && (flags & RTL8169_DESC_LS)) {
                    // This is a non-segmented packet, just send directly
                    tap_send(rtl8169->tap, ptr, size);
                } else {
                    // Reassemble segmented packet from descriptors
                    if (flags & RTL8169_DESC_FS) {
                        // Start assembling a new packet
                        rtl8169->seg_size = 0;
                    }
                    if (rtl8169->seg_size < RTL8169_MAX_PKT_SIZE - size) {
                        memcpy(rtl8169->seg_buff + rtl8169->seg_size, ptr, size);
                        rtl8169->seg_size += size;
                        if (flags & RTL8169_DESC_LS) {
                            // Last segment found
                            tap_send(rtl8169->tap, rtl8169->seg_buff, rtl8169->seg_size);
                            rtl8169->seg_size = 0;
                        }
                    } else {
                        // Transmit error
                        rtl8169_interrupt(rtl8169, RTL8169_IRQ_TER);
                        rtl8169->seg_size = -1;
                    }
                }
            } else {
                // Keep going as if nothing happened, maybe next descriptor will be OK
                rvvm_debug("rtl8169 TX packet DMA error");
                rtl8169->seg_size = -1;
            }

            ring->index++;
            if ((flags & RTL8169_DESC_EOR) || (ring->index >= RTL8169_MAX_FIFO_SIZE)) {
                ring->index = 0;
            }

            atomic_store_uint32_le(desc, flags & RTL8169_DESC_TXSTA);
            tx_irq = true;
        }
        spin_unlock(&ring->lock);

        if (tx_irq) {
            rtl8169_interrupt(rtl8169, RTL8169_IRQ_TOK);
        }
    }
}

static bool rtl8169_pci_read(rvvm_mmio_dev_t* dev, void* data, size_t offset, uint8_t size)
{
    rtl8169_dev_t* rtl8169 = dev->data;
    uint32_t val = 0;

    switch (offset & (~0x3)) {
        case RTL8169_REG_IDR0:
            spin_lock(&rtl8169->mac_lock);
            tap_get_mac(rtl8169->tap, rtl8169->mac);
            val = read_uint32_le(rtl8169->mac);
            spin_unlock(&rtl8169->mac_lock);
            break;
        case RTL8169_REG_IDR4:
            spin_lock(&rtl8169->mac_lock);
            tap_get_mac(rtl8169->tap, rtl8169->mac);
            val = read_uint16_le(rtl8169->mac + 4);
            spin_unlock(&rtl8169->mac_lock);
            break;
        case RTL8169_REG_IMR:
            val = atomic_load_uint32_relax(&rtl8169->imr);
            val |= atomic_load_uint32_relax(&rtl8169->isr) << 16;
            break;
        case RTL8169_REG_CR32:
            val = atomic_load_uint32_relax(&rtl8169->cr) << 24;
            break;
        case RTL8169_REG_TCR:
            val = RTL8169_TCR_DEFAULT | RTL8169_XID_RTL8168B;
            break;
        case RTL8169_REG_RCR:
            val = RTL8169_RCR_DEFAULT;
            break;
        case RTL8169_REG_9346:
            val = rtl8169_93c56_read_pins(rtl8169);
            break;
        case RTL8169_REG_ERIDR:
        case RTL8169_REG_OCPDR:
            val = atomic_load_uint32_relax(&rtl8169->phydr);
            break;
        case RTL8169_REG_PHYAR:
        case RTL8169_REG_ERIAR:
        case RTL8169_REG_EPHAR:
        case RTL8169_REG_OCPAR:
            val = atomic_load_uint32_relax(&rtl8169->phyar);
            break;
        case RTL8169_REG_PHYS:
            val = RTL8169_PHY_STATUS;
            break;
        case RTL8169_REG_TXDA1:
            val = atomic_load_uint32_relax(&rtl8169->tx.addr);
            break;
        case RTL8169_REG_TXDA2:
            val = atomic_load_uint32_relax(&rtl8169->tx.addr_h);
            break;
        case RTL8169_REG_TXHA1:
            val = atomic_load_uint32_relax(&rtl8169->txp.addr);
            break;
        case RTL8169_REG_TXHA2:
            val = atomic_load_uint32_relax(&rtl8169->txp.addr_h);
            break;
        case RTL8169_REG_CPCR:
            val = RTL8169_CPCR_RXCSUM | RTL8169_CPCR_RXVLAN;
            break;
        case RTL8169_REG_RXDA1:
            val = atomic_load_uint32_relax(&rtl8169->rx.addr);
            break;
        case RTL8169_REG_RXDA2:
            val = atomic_load_uint32_relax(&rtl8169->rx.addr_h);
            break;
        case RTL8169_REG_RMS32:
            val = RTL8169_RMS << 16;
            break;
        case RTL8169_REG_MTPS:
            val = RTL8169_MTPS;
            break;
    }

    write_uint32_le(&val, val);
    memcpy(data, ((uint8_t*)&val) + (offset & 0x3), size);
    return true;
}

static bool rtl8169_pci_write(rvvm_mmio_dev_t* dev, void* data, size_t offset, uint8_t size)
{
    rtl8169_dev_t* rtl8169 = dev->data;
    uint32_t val = 0;
    if (likely(size == 2)) {
        val = read_uint16_le(data);
    } else if (likely(size == 1)) {
        val = read_uint8(data);
    } else {
        val = read_uint32_le(data);
    }

    switch (offset) {
        case RTL8169_REG_IDR0:
            spin_lock(&rtl8169->mac_lock);
            memcpy(rtl8169->mac, data, size);
            tap_set_mac(rtl8169->tap, rtl8169->mac);
            spin_unlock(&rtl8169->mac_lock);
            break;
        case RTL8169_REG_IDR4:
            spin_lock(&rtl8169->mac_lock);
            memcpy(rtl8169->mac + 4, data, EVAL_MIN(size, 2));
            tap_set_mac(rtl8169->tap, rtl8169->mac);
            spin_unlock(&rtl8169->mac_lock);
            break;
        case RTL8169_REG_IMR:
            atomic_store_uint32_relax(&rtl8169->imr, (uint16_t)val);
            rtl8169_update_irqs(rtl8169);
            break;
        case RTL8169_REG_ISR:
            if (atomic_and_uint32(&rtl8169->isr, ~val) & val) {
                rtl8169_update_irqs(rtl8169);
            }
            break;
        case RTL8169_REG_CR:
            atomic_store_uint32_relax(&rtl8169->cr, val & RTL8169_CR_RW);
            if (val & RTL8169_CR_RST) {
                rtl8169_reset(dev);
            }
            break;
        case RTL8169_REG_TPOLL:
            if (val & RTL8169_TPOLL_HPQ) rtl8169_tx_doorbell(rtl8169, &rtl8169->txp);
            if (val & RTL8169_TPOLL_NPQ) rtl8169_tx_doorbell(rtl8169, &rtl8169->tx);
            if (val & RTL8169_TPOLL_FSW) rtl8169_interrupt(rtl8169, RTL8169_IRQ_SWI);
            break;
        case RTL8169_REG_9346:
            rtl8169_93c56_write_pins(rtl8169, val);
            break;
        case RTL8169_REG_TXDA1:
            atomic_store_uint32_relax(&rtl8169->tx.addr, val & ~0xFFU);
            break;
        case RTL8169_REG_TXDA2:
            atomic_store_uint32_relax(&rtl8169->tx.addr_h, val);
            break;
        case RTL8169_REG_TXHA1:
            atomic_store_uint32_relax(&rtl8169->txp.addr, val & ~0xFFU);
            break;
        case RTL8169_REG_TXHA2:
            atomic_store_uint32_relax(&rtl8169->txp.addr_h, val);
            break;
        case RTL8169_REG_RXDA1:
            atomic_store_uint32_relax(&rtl8169->rx.addr, val & ~0xFFU);
            break;
        case RTL8169_REG_RXDA2:
            atomic_store_uint32_relax(&rtl8169->rx.addr_h, val);
            break;
        case RTL8169_REG_PHYAR:
        case RTL8169_REG_EPHAR:
            rtl8169_handle_phy(rtl8169, val);
            break;
        case RTL8169_REG_ERIAR:
            rtl8169_handle_eri_phy(rtl8169, val);
            break;
        case RTL8169_REG_OCPAR:
            rtl8169_handle_ocp_phy(rtl8169, val);
            break;
    }

    return true;
}

static void rtl8169_remove(rvvm_mmio_dev_t* dev)
{
    rtl8169_dev_t* rtl8169 = dev->data;
    tap_close(rtl8169->tap);
    free(rtl8169);
}

static void rtl8169_remove_dummy(rvvm_mmio_dev_t* dev)
{
    UNUSED(dev);
}

static rvvm_mmio_type_t rtl8169_type = {
    .name = "rtl8169",
    .remove = rtl8169_remove,
    .reset = rtl8169_reset,
};

static rvvm_mmio_type_t rtl8169_type_dummy = {
    .name = "rtl8169",
    .remove = rtl8169_remove_dummy,
};

PUBLIC pci_dev_t* rtl8169_init(pci_bus_t* pci_bus, tap_dev_t* tap)
{
    rtl8169_dev_t* rtl8169 = safe_new_obj(rtl8169_dev_t);
    tap_net_dev_t nic = {
        .net_dev = rtl8169,
        .feed_rx = rtl8169_feed_rx,
    };

    rtl8169->tap = tap;
    tap_attach(tap, &nic);
    if (rtl8169->tap == NULL) {
        rvvm_error("Failed to create TAP device!");
        free(rtl8169);
        return NULL;
    }

    pci_func_desc_t rtl8169_desc = {
        .vendor_id = 0x10EC,  // Realtek
        .device_id = 0x8168,  // RTL8168 Gigabit NIC
        .class_code = 0x0200, // Ethernet
        .irq_pin = PCI_IRQ_PIN_INTA,
        .bar[0] = {
            .size = 0x100,
            .min_op_size = 1,
            .max_op_size = 4,
            .read = rtl8169_pci_read,
            .write = rtl8169_pci_write,
            .data = rtl8169,
            .type = &rtl8169_type_dummy,
        },
        .bar[2] = {
            .size = 0x1000,
            .min_op_size = 1,
            .max_op_size = 4,
            .read = rtl8169_pci_read,
            .write = rtl8169_pci_write,
            .data = rtl8169,
            .type = &rtl8169_type,
        },
    };

    pci_dev_t* pci_dev = pci_attach_func(pci_bus, &rtl8169_desc);
    if (pci_dev) {
        // Successfully plugged in
        rtl8169->pci_func = pci_get_device_func(pci_dev, 0);
    }
    return pci_dev;
}

PUBLIC pci_dev_t* rtl8169_init_auto(rvvm_machine_t* machine)
{
    tap_dev_t* tap = tap_open();
    if (tap == NULL) {
        rvvm_error("Failed to create TAP device!");
        return NULL;
    }
    return rtl8169_init(rvvm_get_pci_bus(machine), tap);
}

#endif

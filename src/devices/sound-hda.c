/*
sound-hda.h - HD Audio
Copyright (C) 2025  David Korenchuk <github.com/epoll-reactor-2>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "sound-hda.h"
#include "atomics.h"
#include "compiler.h"
#include "mem_ops.h"
#include "rvvm_types.h"
#include "threading.h"
#include "spinlock.h"
#include "utils.h"
#include <stdio.h>
#include <unistd.h>

#define SOUND_VENDOR_ID_CMEDIA        0x13f6 // C-Media
#define SOUND_DEVICE_ID_CMEDIA        0x5011 // CM8888 HDA Controller
#define SOUND_CLASS_CODE_CMEDIA       0x0403 // Audio device

#define SOUND_HDA_FIFO_SIZE           0x100

// CORB - Command Outbound Ring Buffer
// CORB verbs: include/sound/hda_verbs.h
// https://github.com/VendelinSlezak/BleskOS/blob/master/source/drivers/sound/hda.c
// https://github.com/freebsd/freebsd-src/blob/main/sys/dev/sound/pci/hda/hdac.c
// https://intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf

#define SOUND_HDA_GCAP                0x00 // Global capabilities
#define SOUND_HDA_VS                  0x02 // 0x02 minor, 0x03 major
#define SOUND_HDA_OUTPAY              0x04 // Output payload capability
#define SOUND_HDA_INPAY               0x06 // Input payload capability
#define SOUND_HDA_GLOBAL_CTRL         0x08 // Global Control
#define SOUND_HDA_WAKEEN              0x0C // Wake enable
#define SOUND_HDA_STATESTS            0x0E // State Change Status
#define SOUND_HDA_GTS                 0x10 // Global status
#define SOUND_HDA_OUTSTRMPAY          0x18 // Output stream payload capability
#define SOUND_HDA_INSTRMPAY           0x1A // Input stream payload capability
#define SOUND_HDA_INTR_CTRL           0x20 // Interrupt Control
#define SOUND_HDA_INTSTS              0x24 // Interrupt status
#define SOUND_HDA_WALL_CLOCK          0x30 // Wall clock counter
#define SOUND_HDA_STREAM_SYNC         0x38 // Stream Synchronization
#define SOUND_HDA_CORB_LO             0x40 // CORB Lower Base Address
#define SOUND_HDA_CORB_HI             0x44 // CORB Upper Base Address
#define SOUND_HDA_CORB_WP             0x48 // CORB Write Pointer
#define SOUND_HDA_CORB_RP             0x4A // CORB Read Pointer
#define SOUND_HDA_CORB_CTRL           0x4C // CORB Control
#define SOUND_HDA_CORB_STATUS         0x4D // CORB Status
#define SOUND_HDA_CORB_SIZE           0x4E // CORB Size
#define SOUND_HDA_RIRB_LO             0x50 // RIRB Lower Base Address
#define SOUND_HDA_RIRB_HI             0x54 // RIRB Upper Base Address
#define SOUND_HDA_RIRB_WP             0x58 // RIRB Write Pointer
#define SOUND_HDA_RIRB_INTR_CNT       0x5A // RIRB Response Interrupt Count
#define SOUND_HDA_RIRB_CTRL           0x5C // RIRB Control
#define SOUND_HDA_RIRB_STATUS         0x5D // RIRB Status
#define SOUND_HDA_RIRB_SIZE           0x5E // RIRB Size
#define SOUND_HDA_DMA_LO              0x70 // DMA Position Lower Base Address
#define SOUND_HDA_DMA_HI              0x74 // DMA Position Upper Base Address

// This implementation assumes 1 input and 1 output streams,
// so registers are hard coded there.
#define SOUND_HDA_ISD0                0x80
#define SOUND_HDA_ISD0CTL             SOUND_HDA_ISD0 + 0x00 // Input Stream Descriptor Control
#define SOUND_HDA_ISD0STS             SOUND_HDA_ISD0 + 0x03 // Input Stream Descriptor Status
#define SOUND_HDA_ISD0LPIB            SOUND_HDA_ISD0 + 0x04 // Input Stream Descriptor Link Position in Buffer
#define SOUND_HDA_ISD0CBL             SOUND_HDA_ISD0 + 0x08 // Input Stream Descriptor Cyclic Buffer Length
#define SOUND_HDA_ISD0LVI             SOUND_HDA_ISD0 + 0x0C // Input Stream Descriptor Last Valid Index
#define SOUND_HDA_ISD0FIFOS           SOUND_HDA_ISD0 + 0x10 // Input Stream Descriptor FIFO Size
#define SOUND_HDA_ISD0FMT             SOUND_HDA_ISD0 + 0x12 // Input Stream Descriptor Format
#define SOUND_HDA_ISD0BDPL            SOUND_HDA_ISD0 + 0x18 // Input Stream Descriptor BDL Pointer Lower Base Address
#define SOUND_HDA_ISD0BDPU            SOUND_HDA_ISD0 + 0x1C // Input Stream Descriptor BDL Pointer Upper Base Address

#define SOUND_HDA_OSD0                0xA0
#define SOUND_HDA_OSD0CTL             SOUND_HDA_OSD0 + 0x00 // Output Stream Descriptor Control
#define SOUND_HDA_OSD0STS             SOUND_HDA_OSD0 + 0x03 // Output Stream Descriptor Status
#define SOUND_HDA_OSD0LPIB            SOUND_HDA_OSD0 + 0x04 // Output Stream Descriptor Link Position in Buffer
#define SOUND_HDA_OSD0CBL             SOUND_HDA_OSD0 + 0x08 // Output Stream Descriptor Cyclic Buffer Length
#define SOUND_HDA_OSD0LVI             SOUND_HDA_OSD0 + 0x0C // Output Stream Descriptor Last Valid Index
#define SOUND_HDA_OSD0FIFOS           SOUND_HDA_OSD0 + 0x10 // Output Stream Descriptor FIFO Size
#define SOUND_HDA_OSD0FMT             SOUND_HDA_OSD0 + 0x12 // Output Stream Descriptor Format
#define SOUND_HDA_OSD0BDPL            SOUND_HDA_OSD0 + 0x18 // Output Stream Descriptor BDL Pointer Lower Base Address
#define SOUND_HDA_OSD0BDPU            SOUND_HDA_OSD0 + 0x1C // Output Stream Descriptor BDL Pointer Upper Base Address

#define SOUND_HDA_PARAM_V             0x103 // Version 1.03
#define SOUND_HDA_PARAM_NO_OUT        0x01  // Number of output streams supported
#define SOUND_HDA_PARAM_NO_IN         0x01  // Number of input streams supported
#define SOUND_HDA_PARAM_NO_BSS        0x00  // Number of bidirectional streams supported
#define SOUND_HDA_PARAM_NO_NSDO       0x00  // Number of serial data out signals
#define SOUND_HDA_PARAM_64_BIT        0x00  // 64-bit support

#define SOUND_HDA_PARAM_GCAP          ((SOUND_HDA_PARAM_NO_OUT  & 15) << 12) \
                                    | ((SOUND_HDA_PARAM_NO_IN   & 15) <<  8) \
                                    | ((SOUND_HDA_PARAM_NO_BSS  & 31) <<  3) \
                                    | ((SOUND_HDA_PARAM_NO_NSDO &  2) <<  1) \
                                    | ((SOUND_HDA_PARAM_64_BIT  &  1))

#define SOUND_HDA_PARAM_CORBSZCAP     0b0001 /*   8 bytes = 2 entries  */ \
                                    | 0b0010 /*  64 bytes = 16 entries */ \
                                    | 0b0100 /* 256 bytes = 32 entries */ \
                                    | 0b1000

#define SOUND_HDA_PARAM_RIRBSZCAP     0b0001 /*   16 bytes =   2 entries */ \
                                    | 0b0010 /*  128 bytes =  16 entries */ \
                                    | 0b0100 /* 2048 bytes = 256 entries */ \
                                    | 0b1000

#define SOUND_HDA_PARAM_CORBSIZE      0b10 // 256 bytes = 32 entries
#define SOUND_HDA_PARAM_RIRBSIZE      0b10 // 2048 bytes = 256 entries

// Parameters are described in 7.3.4 Parameters
#define VERB_GET_PARAMETER                               0xF00
#define VERB_GET_CONN_SELECT_CONTROL                     0xF01
#define VERB_SET_CONN_SELECT_CONTROL                     0x701
#define VERB_GET_CONN_LIST_ENTRY                         0xF02
#define VERB_GET_PROCESSING_STATE                        0xF03
#define VERB_SET_PROCESSING_STATE                        0x703
#define VERB_GET_COEFF_INDEX                             0xD
#define VERB_SET_COEFF_INDEX                             0x5
#define VERB_GET_PROCESSING_COEFF                        0xC
#define VERB_SET_PROCESSING_COEFF                        0x4

#define VERB_GET_AMP_GAIN_MUTE                           0xB
#define VERB_GET_AMP_GAIN_MUTE_INPUT                     0x0000
#define VERB_GET_AMP_GAIN_MUTE_OUTPUT                    0x8000
#define VERB_GET_AMP_GAIN_MUTE_RIGHT                     0x0000
#define VERB_GET_AMP_GAIN_MUTE_LEFT                      0x2000

#define VERB_SET_AMP_GAIN_MUTE                           0x3
#define VERB_SET_AMP_GAIN_MUTE_MUTE                      0x80
#define VERB_SET_AMP_GAIN_MUTE_GAIN_MASK                 0x7
#define VERB_SET_AMP_GAIN_MUTE_OUTPUT                    0x8000
#define VERB_SET_AMP_GAIN_MUTE_INPUT                     0x4000
#define VERB_SET_AMP_GAIN_MUTE_LEFT                      0x2000
#define VERB_SET_AMP_GAIN_MUTE_RIGHT                     0x1000

#define VERB_GET_CONV_FMT                                0xA
#define VERB_SET_CONV_FMT                                0x2
#define VERB_GET_DIGITAL_CONV_FMT1                       0xF0D
#define VERB_GET_DIGITAL_CONV_FMT2                       0xF0E
#define VERB_SET_DIGITAL_CONV_FMT1                       0x70D
#define VERB_SET_DIGITAL_CONV_FMT2                       0x70E
#define VERB_GET_POWER_STATE                             0xF05
#define VERB_SET_POWER_STATE                             0x705
#define VERB_GET_CONV_STREAM_CHAN                        0xF06
#define VERB_SET_CONV_STREAM_CHAN                        0x706
#define VERB_GET_INPUT_CONVERTER_SDI_SELECT              0xF04
#define VERB_SET_INPUT_CONVERTER_SDI_SELECT              0x704

#define VERB_GET_PIN_WIDGET_CTRL                         0xF07
#define VERB_GET_PIN_WIDGET_CTRL_HPHN_ENABLE             (1 << 7)
#define VERB_GET_PIN_WIDGET_CTRL_OUT_ENABLE              (1 << 6)
#define VERB_GET_PIN_WIDGET_CTRL_IN_ENABLE               (1 << 5)
#define VERB_GET_PIN_WIDGET_CTRL_VREF_ENABLE             (1 << 0)

#define VERB_SET_PIN_WIDGET_CTRL                         0x707
#define VERB_GET_UNSOLICITED_RESPONSE                    0xF08
#define VERB_SET_UNSOLICITED_RESPONSE                    0x708

#define VERB_GET_PIN_SENSE                               0xF09
#define VERB_GET_PIN_SENSE_PRESENSE_PLUGGED              (1 << 31)

#define VERB_SET_PIN_SENSE                               0x709
#define VERB_GET_EAPD_BTL_ENABLE                         0xF0C
#define VERB_SET_EAPD_BTL_ENABLE                         0x70C
#define VERB_GET_GPI_DATA                                0xF10
#define VERB_SET_GPI_DATA                                0x710
#define VERB_GET_GPI_WAKE_ENABLE_MASK                    0xF11
#define VERB_SET_GPI_WAKE_ENABLE_MASK                    0x711
#define VERB_GET_GPI_UNSOLICITED_ENABLE_MASK             0xF12
#define VERB_SET_GPI_UNSOLICITED_ENABLE_MASK             0x712
#define VERB_GET_GPI_STICKY_MASK                         0xF13
#define VERB_SET_GPI_STICKY_MASK                         0x713
#define VERB_GET_GPO_DATA                                0xF14
#define VERB_SET_GPO_DATA                                0x714
#define VERB_GET_GPIO_DATA                               0xF15
#define VERB_SET_GPIO_DATA                               0x715
#define VERB_GET_GPIO_ENABLE_MASK                        0xF16
#define VERB_SET_GPIO_ENABLE_MASK                        0x716
#define VERB_GET_GPIO_DIRECTION                          0xF17
#define VERB_SET_GPIO_DIRECTION                          0x717
#define VERB_GET_GPIO_WAKE_ENABLE_MASK                   0xF18
#define VERB_SET_GPIO_WAKE_ENABLE_MASK                   0x718
#define VERB_GET_GPIO_UNSOLICITED_ENABLE_MASK            0xF19
#define VERB_SET_GPIO_UNSOLICITED_ENABLE_MASK            0x719
#define VERB_GET_GPIO_STICKY_MASK                        0xF1A
#define VERB_SET_GPIO_STICKY_MASK                        0x71A
#define VERB_GET_BEEP_GENERATION                         0xF0A
#define VERB_SET_BEEP_GENERATION                         0x70A
#define VERB_GET_VOLUME_KNOB                             0xF0F
#define VERB_SET_VOLUME_KNOB                             0x70F
#define VERB_GET_SUBSYSTEM_ID                            0xF20
#define VERB_SET_SUSBYSTEM_ID1                           0x720
#define VERB_SET_SUBSYSTEM_ID2                           0x721
#define VERB_SET_SUBSYSTEM_ID3                           0x722
#define VERB_SET_SUBSYSTEM_ID4                           0x723

#define VERB_GET_CONFIG_DEFAULT                          0xF1C
#define VERB_GET_CONFIG_DEFAULT_DEVICE_LINE_OUT          ( 0 << 20)
#define VERB_GET_CONFIG_DEFAULT_DEVICE_SPEAKER           ( 1 << 20)
#define VERB_GET_CONFIG_DEFAULT_DEVICE_HP_OUT            ( 2 << 20)
#define VERB_GET_CONFIG_DEFAULT_DEVICE_CD                ( 3 << 20)
#define VERB_GET_CONFIG_DEFAULT_DEVICE_SPDIF_OUT         ( 4 << 20)
#define VERB_GET_CONFIG_DEFAULT_DEVICE_DIGITAL_OTHER_OUT ( 5 << 20)
#define VERB_GET_CONFIG_DEFAULT_DEVICE_MODEM_LINE        ( 6 << 20)
#define VERB_GET_CONFIG_DEFAULT_DEVICE_MODEM_HANDSET     ( 7 << 20)
#define VERB_GET_CONFIG_DEFAULT_DEVICE_LIVE_IN           ( 8 << 20)
#define VERB_GET_CONFIG_DEFAULT_DEVICE_AUX               ( 9 << 20)
#define VERB_GET_CONFIG_DEFAULT_DEVICE_MIC_IN            (10 << 20)
#define VERB_GET_CONFIG_DEFAULT_DEVICE_TELEPHONY         (11 << 20)
#define VERB_GET_CONFIG_DEFAULT_DEVICE_SPDIF_IN          (12 << 20)
#define VERB_GET_CONFIG_DEFAULT_DEVICE_DIGITAL_OTHER_IN  (13 << 20)
#define VERB_GET_CONFIG_DEFAULT_DEVICE_OTHER             (15 << 20)
#define VERB_GET_CONFIG_DEFAULT_CONNECTIVITY_JACK        ( 0 << 30)
#define VERB_GET_CONFIG_DEFAULT_CONNECTIVITY_NONE        ( 1 << 30)
#define VERB_GET_CONFIG_DEFAULT_CONNECTIVITY_FIXED       ( 2 << 30)
#define VERB_GET_CONFIG_DEFAULT_CONNECTIVITY_BOTH        ( 3 << 30)

#define VERB_SET_CONFIG_DEFAULT1                         0x71C
#define VERB_SET_CONFIG_DEFAULT2                         0x71D
#define VERB_SET_CONFIG_DEFAULT3                         0x71E
#define VERB_SET_CONFIG_DEFAULT4                         0x71F
#define VERB_GET_STRIPE_CONTROL                          0xF24
#define VERB_SET_STRIPE_CONTROL                          0x724
#define VERB_GET_CONV_CHAN_COUNT                         0xF2D
#define VERB_SET_CONV_CHAN_COUNT                         0x72D
#define VERB_FUNCTION_RESET                              0x7FF

// Applicable for VERB_GET_PARAMETER. We should assign static
// values to these parameter to emulate codec.
#define CODEC_PARAM_VENDOR_ID                            0x00
#define CODEC_PARAM_REVISION_ID                          0x02
#define CODEC_PARAM_SUB_NODE_COUNT                       0x04

#define CODEC_PARAM_FUNC_GROUP_TYPE                      0x05
#define CODEC_PARAM_FUNC_GROUP_TYPE_AUDIO                0x01
#define CODEC_PARAM_FUNC_GROUP_TYPE_MODEM                0x02

#define CODEC_PARAM_AUDIO_FUNC_GROUP_TYPE                0x08

#define CODEC_PARAM_AUDIO_WIDGET_CAPS                    0x09
#define CODEC_PARAM_AUDIO_WIDGET_CAPS_STEREO             (1 <<  0)
#define CODEC_PARAM_AUDIO_WIDGET_CAPS_AMP_IN             (1 <<  1)
#define CODEC_PARAM_AUDIO_WIDGET_CAPS_AMP_OUT            (1 <<  2)
#define CODEC_PARAM_AUDIO_WIDGET_CAPS_AMP_OVR            (1 <<  3)
#define CODEC_PARAM_AUDIO_WIDGET_CAPS_FORMAT_OVR         (1 <<  4)
#define CODEC_PARAM_AUDIO_WIDGET_CAPS_CONN_LIST          (1 <<  8)
#define CODEC_PARAM_AUDIO_WIDGET_CAPS_OUTPUT             (0 << 20)
#define CODEC_PARAM_AUDIO_WIDGET_CAPS_INPUT              (1 << 20)
#define CODEC_PARAM_AUDIO_WIDGET_CAPS_PIN                (4 << 20)

#define CODEC_PARAM_SUPP_PCM_SIZE_RATES                  0x0A

#define CODEC_PARAM_SUPP_STREAM_FMTS                     0x0B
#define CODEC_PARAM_SUPP_STREAM_FMTS_PCM                 (1 << 0)
#define CODEC_PARAM_SUPP_STREAM_FMTS_FLOAT32             (1 << 1)
#define CODEC_PARAM_SUPP_STREAM_FMTS_AC3                 (1 << 2)

#define CODEC_PARAM_PIN_CAPS                             0x0C
#define CODEC_PARAM_PIN_CAPS_IMP_SENSE                   (1 <<  0)
#define CODEC_PARAM_PIN_CAPS_TRIGGER_REQD                (1 <<  1)
#define CODEC_PARAM_PIN_CAPS_PRESENSE_DETECT             (1 <<  2)
#define CODEC_PARAM_PIN_CAPS_HEADPHONE                   (1 <<  3)
#define CODEC_PARAM_PIN_CAPS_OUTPUT                      (1 <<  4)
#define CODEC_PARAM_PIN_CAPS_INPUT                       (1 <<  5)
#define CODEC_PARAM_PIN_CAPS_BALANCED_IO                 (1 <<  6)
#define CODEC_PARAM_PIN_CAPS_HDMI                        (1 <<  7)
#define CODEC_PARAM_PIN_CAPS_VREF_CTRL_HIZ               (1 <<  8)
#define CODEC_PARAM_PIN_CAPS_VREF_CTRL_50                (1 <<  9)
#define CODEC_PARAM_PIN_CAPS_VREF_CTRL_GROUND            (1 << 10)
#define CODEC_PARAM_PIN_CAPS_VREF_CTRL_80                (1 << 12)
#define CODEC_PARAM_PIN_CAPS_VREF_CTRL_100               (1 << 13)

#define CODEC_PARAM_INPUT_AMP_CAPS                       0x0D

#define CODEC_PARAM_OUTPUT_AMP_CAPS                      0x12
#define CODEC_PARAM_OUTPUT_AMP_CAPS_MUTE_CAP             (   1 << 31)
#define CODEC_PARAM_OUTPUT_AMP_CAPS_STEPSIZE             (   3 << 16)
#define CODEC_PARAM_OUTPUT_AMP_CAPS_NUMSTEPS             (0x4a <<  8)
#define CODEC_PARAM_OUTPUT_AMP_CAPS_OFFSET               (0x4a <<  0) // Why 0x4a?

#define CODEC_PARAM_CONN_LIST_LEN                        0x0E
#define CODEC_PARAM_SUPP_POWER_STATES                    0x0F
#define CODEC_PARAM_PROCESSING_CAPS                      0x10
#define CODEC_PARAM_GPIO_CNT                             0x11
#define CODEC_PARAM_VOLUME_KNOB                          0x12

// For debugging purposes. Should I cut my fingers? I don't know.
static inline const char *sound_param_to_string(int param)
{
    switch (param) {
    case SOUND_HDA_GCAP:          return "SOUND_HDA_GCAP            ";
    case SOUND_HDA_VS:            return "SOUND_HDA_VS              ";
    case SOUND_HDA_OUTPAY:        return "SOUND_HDA_OUTPAY          ";
    case SOUND_HDA_INPAY:         return "SOUND_HDA_INPAY           ";
    case SOUND_HDA_GLOBAL_CTRL:   return "SOUND_HDA_GLOBAL_CTRL     ";
    case SOUND_HDA_WAKEEN:        return "SOUND_HDA_WAKEEN          ";
    case SOUND_HDA_STATESTS:      return "SOUND_HDA_STATESTS        ";
    case SOUND_HDA_GTS:           return "SOUND_HDA_GTS             ";
    case SOUND_HDA_OUTSTRMPAY:    return "SOUND_HDA_OUTSTRMPAY      ";
    case SOUND_HDA_INSTRMPAY:     return "SOUND_HDA_INSTRMPAY       ";
    case SOUND_HDA_INTR_CTRL:     return "SOUND_HDA_INTR_CTRL       ";
    case SOUND_HDA_INTSTS:        return "SOUND_HDA_INTSTS          ";
    case SOUND_HDA_WALL_CLOCK:    return "SOUND_HDA_WALL_CLOCK      ";
    case SOUND_HDA_STREAM_SYNC:   return "SOUND_HDA_STREAM_SYNC     ";
    case SOUND_HDA_CORB_LO:       return "SOUND_HDA_CORB_LO         ";
    case SOUND_HDA_CORB_HI:       return "SOUND_HDA_CORB_HI         ";
    case SOUND_HDA_CORB_WP:       return "SOUND_HDA_CORB_WP         ";
    case SOUND_HDA_CORB_RP:       return "SOUND_HDA_CORB_RP         ";
    case SOUND_HDA_CORB_CTRL:     return "SOUND_HDA_CORB_CTRL       ";
    case SOUND_HDA_CORB_SIZE:     return "SOUND_HDA_CORB_SIZE       ";
    case SOUND_HDA_RIRB_LO:       return "SOUND_HDA_RIRB_LO         ";
    case SOUND_HDA_RIRB_HI:       return "SOUND_HDA_RIRB_HI         ";
    case SOUND_HDA_RIRB_WP:       return "SOUND_HDA_RIRB_WP         ";
    case SOUND_HDA_RIRB_INTR_CNT: return "SOUND_HDA_RIRB_INTR_CNT   ";
    case SOUND_HDA_RIRB_CTRL:     return "SOUND_HDA_RIRB_CTRL       ";
    case SOUND_HDA_RIRB_STATUS:   return "SOUND_HDA_RIRB_STATUS     ";
    case SOUND_HDA_RIRB_SIZE:     return "SOUND_HDA_RIRB_SIZE       ";
    case SOUND_HDA_DMA_LO:        return "SOUND_HDA_DMA_LO          ";
    case SOUND_HDA_DMA_HI:        return "SOUND_HDA_DMA_HI          ";
    case SOUND_HDA_ISD0CTL:       return "SOUND_HDA_ISD0CTL         ";
    case SOUND_HDA_ISD0STS:       return "SOUND_HDA_ISD0STS         ";
    case SOUND_HDA_ISD0LPIB:      return "SOUND_HDA_ISD0LPIB        ";
    case SOUND_HDA_ISD0CBL:       return "SOUND_HDA_ISD0CBL         ";
    case SOUND_HDA_ISD0LVI:       return "SOUND_HDA_ISD0LVI         ";
    case SOUND_HDA_ISD0FIFOS:     return "SOUND_HDA_ISD0FIFOS       ";
    case SOUND_HDA_ISD0FMT:       return "SOUND_HDA_ISD0FMT         ";
    case SOUND_HDA_ISD0BDPL:      return "SOUND_HDA_ISD0BDPL        ";
    case SOUND_HDA_ISD0BDPU:      return "SOUND_HDA_ISD0BDPU        ";
    case SOUND_HDA_OSD0CTL:       return "SOUND_HDA_OSD0CTL         ";
    case SOUND_HDA_OSD0STS:       return "SOUND_HDA_OSD0STS         ";
    case SOUND_HDA_OSD0LPIB:      return "SOUND_HDA_OSD0LPIB        ";
    case SOUND_HDA_OSD0CBL:       return "SOUND_HDA_OSD0CBL         ";
    case SOUND_HDA_OSD0LVI:       return "SOUND_HDA_OSD0LVI         ";
    case SOUND_HDA_OSD0FIFOS:     return "SOUND_HDA_OSD0FIFOS       ";
    case SOUND_HDA_OSD0FMT:       return "SOUND_HDA_OSD0FMT         ";
    case SOUND_HDA_OSD0BDPL:      return "SOUND_HDA_OSD0BDPL        ";
    case SOUND_HDA_OSD0BDPU:      return "SOUND_HDA_OSD0BDPU        ";
    default: {
        static char buf[64];
        sprintf(buf, "<UNKNOWN> %08x        ", param);
        return buf;
    }
    }
}

#define hda_info(fmt, ...) rvvm_info("Sound HDA: " fmt, ##__VA_ARGS__)

typedef struct {
    uint32_t    bdl_lo;
    uint32_t    bdl_hi;
    uint32_t    bdl_len;
    uint32_t    lpib;
    uint8_t     ioce;
    uint8_t     stream;
    uint8_t     channel;
    atomic_bool running;
    uint8_t     fmt;
    uint8_t     status;
    uint8_t     left_gain;
    uint8_t     right_gain;
    uint8_t     left_mute;
    uint8_t     right_mute;
} sound_hda_stream_t;

typedef struct {
    pci_func_t* pci_func;
    spinlock_t  lock;
    uint32_t    gctl;
    uint32_t    gctl_accept_unsol;
    uint32_t    corb_lo;
    uint32_t    corb_hi;
    uint16_t    corb_rp;
    uint16_t    corb_wp;
    uint32_t    corb_size;
    uint32_t    rirb_lo;
    uint32_t    rirb_hi;
    uint32_t    rirb_rp;
    uint32_t    rirb_wp;
    uint32_t    rirb_size;
    uint32_t    rirb_cnt;
    uint32_t    rirb_status;
    uint32_t    power_state;

    sound_hda_stream_t stream_output;
} sound_hda_dev_t;

static void sound_hda_remove(rvvm_mmio_dev_t* dev)
{
    UNUSED(dev);
}

static rvvm_mmio_type_t sound_hda_type = {
    .name = "intel-hda",
    .remove = sound_hda_remove,
};

static bool sound_hda_mmio_read(rvvm_mmio_dev_t* dev, void* data, size_t offset, uint8_t size)
{
    UNUSED(size);

    sound_hda_dev_t *hda = dev->data;
    spin_lock(&hda->lock);

    hda_info("read at %08"PRIx64" %s", (uint64_t) offset, sound_param_to_string(offset));

    switch (offset) {
        case SOUND_HDA_GCAP:
            write_uint16_le(data, SOUND_HDA_PARAM_GCAP);
            break;
        case SOUND_HDA_VS:
            write_uint16_le(data, SOUND_HDA_PARAM_V);
            break;
        case SOUND_HDA_OUTPAY:
            write_uint16_le(data, 0x3C);
            break;
        case SOUND_HDA_INPAY:
            write_uint16_le(data, 0x1D);
            break;
        case SOUND_HDA_GLOBAL_CTRL:
            hda->gctl |= (1 << 8) | 1; // 1 << 8 is UNSOL
            write_uint32_le(data, hda->gctl);
            break;
        case SOUND_HDA_WAKEEN:
            break;
        case SOUND_HDA_STATESTS:
            break;
        case SOUND_HDA_GTS:
            break;
        case SOUND_HDA_OUTSTRMPAY:
            break;
        case SOUND_HDA_INSTRMPAY:
            break;
        case SOUND_HDA_INTR_CTRL: {
            // Is this repeats configuration in GCAP?
            uint32_t cmd = 0;
            cmd |= (0 << 31); // Global interrupt disabled
            cmd |= (0 << 30); // Controller interrupt disabled
            cmd |= (1 <<  0); // Input stream 1 enabled
            cmd |= (1 <<  1); // Input stream 2 enabled
            cmd |= (1 <<  2); // Output stream 1 enabled
            cmd |= (1 <<  3); // Output stream 2 enabled
            write_uint32_le(data, cmd);
            break;
        }
        case SOUND_HDA_INTSTS:
            break;
        case SOUND_HDA_WALL_CLOCK:
            break;
        case SOUND_HDA_STREAM_SYNC:
            break;
        case SOUND_HDA_CORB_LO:
            break;
        case SOUND_HDA_CORB_HI:
            break;
        case SOUND_HDA_CORB_WP:
            write_uint16_le(data, hda->corb_wp);
            break;
        case SOUND_HDA_CORB_RP:
            write_uint16_le(data, hda->corb_rp & 0b11111111);
            break;
        case SOUND_HDA_CORB_CTRL:
            break;
        case SOUND_HDA_CORB_STATUS:
            break;
        case SOUND_HDA_CORB_SIZE:
            write_uint8(data, SOUND_HDA_PARAM_CORBSZCAP << 4 | SOUND_HDA_PARAM_CORBSIZE);
            break;
        case SOUND_HDA_RIRB_LO:
            break;
        case SOUND_HDA_RIRB_HI:
            break;
        case SOUND_HDA_RIRB_WP: {
            write_uint16_le(data, hda->rirb_wp & 0b11111111);
            break;
        }
        case SOUND_HDA_RIRB_INTR_CNT:
            break;
        case SOUND_HDA_RIRB_CTRL:
            break;
        case SOUND_HDA_RIRB_STATUS:
            write_uint8(data, hda->rirb_status & 0xFF);
            // Should we reset RIRB status after read?
            break;
        case SOUND_HDA_RIRB_SIZE:
            write_uint8(data, SOUND_HDA_PARAM_RIRBSZCAP << 4 | SOUND_HDA_PARAM_RIRBSIZE);
            break;
        case SOUND_HDA_DMA_LO:
            break;
        case SOUND_HDA_DMA_HI:
            break;
        case SOUND_HDA_OSD0STS: {
            uint8_t cmd = 0;
            if (hda->stream_output.lpib < hda->stream_output.bdl_len)
                cmd |= 1 << 5;
            else
                cmd &= ~(1 << 5);
            cmd |= hda->stream_output.status;
            write_uint8(data, cmd);
            break;
        }
        case SOUND_HDA_OSD0LPIB:
            write_uint32_le(data, hda->stream_output.lpib);
            break;
        case SOUND_HDA_OSD0FIFOS:
            write_uint16_le(data, SOUND_HDA_FIFO_SIZE);
            break;

        default:
            spin_unlock(&hda->lock);
            return false;
    }
    spin_unlock(&hda->lock);

    return true;
}

static void write_response_to_rirb(sound_hda_dev_t *hda, uint32_t cad, uint32_t response)
{
    hda_info("write to RIRB { resp: %u, rp: %08x, wp: %08x}",
        response, hda->rirb_rp, hda->rirb_wp);

    ++hda->rirb_wp;
    hda->rirb_wp %= hda->rirb_size;

    uint32_t *rirb = pci_get_dma_ptr(
        hda->pci_func,
        (rvvm_addr_t) hda->rirb_lo + hda->rirb_wp * 4,
        4
    );

    uint32_t response_ex = cad;

    rirb[hda->rirb_wp] = response;
    rirb[hda->rirb_wp + 1] = response_ex;
    pci_send_irq(hda->pci_func, 0);
}

// NID = 0
static uint32_t process_codec_root_cmd(uint32_t payload)
{
    switch (payload) {
    case CODEC_PARAM_VENDOR_ID:
        return SOUND_DEVICE_ID_CMEDIA;

    case CODEC_PARAM_REVISION_ID:
        return 0xFFFF;

    case CODEC_PARAM_SUB_NODE_COUNT:
        return 0x00010001; // 1 Subnode, StartNid = 1

    default:
        return 0;
    }
}

// NID = 1
static uint32_t process_codec_fg_output_cmd(uint32_t payload)
{
    switch (payload) {
    case CODEC_PARAM_SUB_NODE_COUNT:
        return 0x00020002; // 2 Subnode, StartNid = 2

    case CODEC_PARAM_FUNC_GROUP_TYPE:
        return CODEC_PARAM_FUNC_GROUP_TYPE_AUDIO;

    case CODEC_PARAM_SUPP_PCM_SIZE_RATES:
        return (0x1F << 16) | 0x7FF; // B8 - B32, 8.0 - 192.0kHz

    case CODEC_PARAM_SUPP_STREAM_FMTS:
        return CODEC_PARAM_SUPP_STREAM_FMTS_PCM;

    case CODEC_PARAM_SUPP_POWER_STATES:
        return 0b1111; // D3, D2, D1, D0

    default:
        return 0;
    }
}

// NID = 2
static uint32_t process_codec_output_cmd(uint32_t payload)
{
    switch (payload) {
    case CODEC_PARAM_VENDOR_ID:
        return SOUND_DEVICE_ID_CMEDIA;

    case CODEC_PARAM_REVISION_ID:
        return 0xFFFF;

    case CODEC_PARAM_SUB_NODE_COUNT:
        return 0x00010001; // 1 Subnode, StartNid = 1

    case CODEC_PARAM_AUDIO_WIDGET_CAPS:
        return CODEC_PARAM_AUDIO_WIDGET_CAPS_OUTPUT
             | CODEC_PARAM_AUDIO_WIDGET_CAPS_FORMAT_OVR
             | CODEC_PARAM_AUDIO_WIDGET_CAPS_AMP_OVR
             | CODEC_PARAM_AUDIO_WIDGET_CAPS_AMP_OUT
             | CODEC_PARAM_AUDIO_WIDGET_CAPS_STEREO;

    case CODEC_PARAM_SUPP_PCM_SIZE_RATES:
        return (0x1F << 16) | 0x7FF; // B8 - B32, 8.0 - 192.0kHz

    case CODEC_PARAM_SUPP_STREAM_FMTS:
        return CODEC_PARAM_SUPP_STREAM_FMTS_PCM;

    case CODEC_PARAM_OUTPUT_AMP_CAPS:
        return CODEC_PARAM_OUTPUT_AMP_CAPS_MUTE_CAP
             | CODEC_PARAM_OUTPUT_AMP_CAPS_STEPSIZE
             | CODEC_PARAM_OUTPUT_AMP_CAPS_NUMSTEPS
             | CODEC_PARAM_OUTPUT_AMP_CAPS_OFFSET;

    default:
        return 0;
    }
}

// NID = 3
static uint32_t process_codec_pin_output_cmd(uint32_t payload)
{
    switch (payload) {
    case CODEC_PARAM_AUDIO_WIDGET_CAPS:
        return CODEC_PARAM_AUDIO_WIDGET_CAPS_PIN
             | CODEC_PARAM_AUDIO_WIDGET_CAPS_CONN_LIST
             | CODEC_PARAM_AUDIO_WIDGET_CAPS_STEREO;

    case CODEC_PARAM_PIN_CAPS:
        return CODEC_PARAM_PIN_CAPS_OUTPUT
             | CODEC_PARAM_PIN_CAPS_PRESENSE_DETECT;

    case CODEC_PARAM_CONN_LIST_LEN:
        return 1;

    default:
        return 0;
    }
}

#define NODE_ID_ROOT                0 // Root node
#define NODE_ID_FG_OUTPUT           1 // Output function group
#define NODE_ID_OUTPUT              2 // Output
#define NODE_ID_PIN_OUTPUT          3 // Pin output

static uint32_t process_codec_stream_cmd(sound_hda_stream_t *stream, uint32_t nid, uint32_t verb, uint32_t payload)
{
    uint32_t response = 0;
    uint8_t  mute = 0;
    uint8_t  gain = 0;

    hda_info("Stream verb: 0x%0X", verb);

    switch (verb) {
    case VERB_GET_CONV_FMT:
        response = stream->fmt;
        break;
    case VERB_SET_CONV_FMT:
        stream->fmt = payload;
        break;
    case VERB_GET_AMP_GAIN_MUTE:
        if (payload & VERB_GET_AMP_GAIN_MUTE_LEFT) {
            response = stream->left_gain | stream->left_mute;
        }

        if (payload & VERB_GET_AMP_GAIN_MUTE_RIGHT) {
            response = stream->right_gain | stream->right_mute;
        }

        break;
    case VERB_SET_AMP_GAIN_MUTE:
        mute = VERB_SET_AMP_GAIN_MUTE_MUTE;
        gain = VERB_SET_AMP_GAIN_MUTE_GAIN_MASK;

        if (payload & VERB_SET_AMP_GAIN_MUTE_LEFT) {
            stream->left_mute = mute;
            stream->left_gain = gain;
        }

        if (payload & VERB_SET_AMP_GAIN_MUTE_RIGHT) {
            stream->right_mute = mute;
            stream->right_gain = gain;
        }

        break;
    case VERB_GET_CONV_STREAM_CHAN:
        response = (stream->stream << 4) | stream->channel;
        break;
    case VERB_SET_CONV_STREAM_CHAN:
        stream->channel = payload & 0x0F;
        stream->stream = (payload >> 4) & 0x0F;
        hda_info("Stream set channel: %x, stream: %x", stream->channel, stream->stream);
        break;
    case VERB_GET_PIN_WIDGET_CTRL:
        switch (nid) {
        case NODE_ID_PIN_OUTPUT:
            response = VERB_GET_PIN_WIDGET_CTRL_OUT_ENABLE;
            break;
        }
        break;
    default:
        break;
    }

    return response;
}

#define VERB_PARAM_12BIT_SHIFT      8
#define VERB_PARAM_4BIT_SHIFT      16
#define VERB_PARAM_CAD_SHIFT       28
#define VERB_PARAM_NID_SHIFT       20

static void process_codec_cmd(sound_hda_dev_t *hda, uint32_t cmd)
{
    uint8_t cad = 0;
    uint8_t nid = 0;
    uint16_t verb = 0;
    uint16_t payload = 0;
    uint32_t response = 0;

    if (cmd == 0)
        return;

    cad = (cmd >> VERB_PARAM_CAD_SHIFT) & 0x0F;
    nid = (cmd >> VERB_PARAM_NID_SHIFT) & 0xFF;

    if ((cmd & 0x70000) == 0x70000) {
        verb = (cmd >> VERB_PARAM_12BIT_SHIFT) & 0x0FFF;
        payload = cmd & 0xFF;
    } else {
        verb = (cmd >> VERB_PARAM_4BIT_SHIFT) & 0x0F;
        payload = cmd & 0xFFFF;
    }

    switch (verb) {
    case VERB_GET_PARAMETER:
        // We define NID count and start indices in CODEC_PARAM_SUB_NODE_COUNT
        switch (nid) {
        case NODE_ID_ROOT:
            response = process_codec_root_cmd(payload);
            break;
        case NODE_ID_FG_OUTPUT:
            response = process_codec_fg_output_cmd(payload);
            break;
        case NODE_ID_OUTPUT:
            response = process_codec_output_cmd(payload);
            break;
        case NODE_ID_PIN_OUTPUT:
            response = process_codec_pin_output_cmd(payload);
            break;
        }

        break;
    case VERB_GET_SUBSYSTEM_ID:
        response = (SOUND_DEVICE_ID_CMEDIA << 16) | 1;
        break;
    case VERB_SET_POWER_STATE:
        hda->power_state = payload;
        response = 0;
        break;
    case VERB_GET_POWER_STATE:
        //         PS-Set              PS-Act
        response = hda->power_state | (hda->power_state << 4);
        break;
    case VERB_GET_PIN_WIDGET_CTRL:
        response |= VERB_GET_PIN_WIDGET_CTRL_OUT_ENABLE;
        break;
    case VERB_GET_PIN_SENSE:
        response = VERB_GET_PIN_SENSE_PRESENSE_PLUGGED;
        break;
    case VERB_GET_CONN_LIST_ENTRY:
        response = NODE_ID_OUTPUT;
        break;
    case VERB_GET_CONFIG_DEFAULT:
        response |= VERB_GET_CONFIG_DEFAULT_CONNECTIVITY_JACK
                 |  VERB_GET_CONFIG_DEFAULT_DEVICE_LINE_OUT
                 |  (1 <<  4)  // CONFIG_DEFAULT_ASSOCIATION_SHIFT. ???
                 |  (1 << 12); // CONFIG_DEFAULT_COLOR_SHIFT. ???
        break;
    default:
        switch (nid) {
        case NODE_ID_OUTPUT:
            response = process_codec_stream_cmd(&hda->stream_output, nid, verb, payload);
            break;
        }
        break;
    }

    write_response_to_rirb(hda, cad, response);
}

// Ugly.
static void* stream_worker(void *arg)
{
    sound_hda_dev_t *hda = (sound_hda_dev_t *) arg;
    sound_hda_stream_t *stream = &hda->stream_output;

    uint32_t should_generate_ioc = 0;

    while (atomic_load(&stream->running)) {
        uint64_t *bdl = pci_get_dma_ptr(
            hda->pci_func, (rvvm_addr_t) stream->bdl_lo, stream->bdl_len);

        uint32_t num_entries = hda->stream_output.bdl_len / 16;

        for (uint32_t i = 0; i < num_entries; ++i) {
            uint64_t addr =  bdl[i * 2 + 0];
            uint32_t len  =  bdl[i * 2 + 1] & 0xFFFFFFFF;
            uint8_t  ioc  = (bdl[i * 2 + 1] >> 32) & 1;

            if (addr == 0 || len == 0)
                break;

            if (ioc) {
                should_generate_ioc = 1;
            }

            uint8_t *pcm_data = pci_get_dma_ptr(hda->pci_func, (rvvm_addr_t) addr, len);
            hda_info("%2d: PCM %02x %02x %02x %02x %02x %02x %02x %02x",
                i,
                pcm_data[ 3], pcm_data[ 7], pcm_data[11], pcm_data[15],
                pcm_data[19], pcm_data[23], pcm_data[27], pcm_data[31]
            );

            stream->lpib += len;

            // Wrap LPIB if we reach the buffer limit (for streams longer that BDL length)
            if (stream->lpib >= stream->bdl_len) {
                stream->lpib = 0;
            }

            // Optionally if IOC enabled we could notify the guest

            // If this is a short stream, stop when we reach the total data length
            if (stream->bdl_len > 0 && stream->lpib >= stream->bdl_len) {
                atomic_store(&stream->running, 0);
            }
        }

        if (should_generate_ioc) {
            should_generate_ioc = 0;
            pci_send_irq(hda->pci_func, 0);
        }
    }

    return NULL;
}

static void process_output_stream_ctl(sound_hda_dev_t *hda, uint32_t cmd)
{
 // uint8_t strm   = (cmd >> 20) & 0b1111;
 // uint8_t dir    = (cmd >> 19) & 0b1;
 // uint8_t tp     = (cmd >> 18) & 0b1;
 // uint8_t stripe = (cmd >> 16) & 0b11;
 // uint8_t deie   = (cmd >>  4) & 0b1;
 // uint8_t feie   = (cmd >>  3) & 0b1;
    uint8_t ioce   = (cmd >>  2) & 0b1;
    uint8_t run    = (cmd >>  1) & 0b1; // Corresponding SSYNC bit?
 // uint8_t reset  = (cmd >>  0) & 0b1;

    sound_hda_stream_t *stream = &hda->stream_output;
    stream->ioce = ioce;

    if (run) {
        if (!atomic_swap_uint32_ex(&hda->stream_output.running, 1, ATOMIC_RELAXED)) {
            hda_info("Starting stream");
            thread_create_task(stream_worker, hda);
        }
    } else {
        if (!atomic_swap_uint32_ex(&hda->stream_output.running, 0, ATOMIC_RELAXED)) {
            hda_info("Stopping stream");
        }
    }
}

static bool sound_hda_mmio_write(rvvm_mmio_dev_t* dev, void* data, size_t offset, uint8_t size)
{
    UNUSED(dev);
    UNUSED(data);
    UNUSED(offset);
    UNUSED(size);

    sound_hda_dev_t *hda = dev->data;
    UNUSED(hda);
    spin_lock(&hda->lock);

    hda_info("write at %08"PRIx64" %s: %04x", (uint64_t) offset, sound_param_to_string(offset), read_uint32_le(data));

    switch (offset) {
        /* No SOUND_HDA_GCAP */
        /* No SOUND_HDA_VS */
        /* No SOUND_HDA_RIRB_SIZE */

        case SOUND_HDA_GLOBAL_CTRL:
            hda->gctl = read_uint32_le(data);

            if (hda->gctl & 0b00100000000)
                hda->gctl_accept_unsol = 1;

            break;
        case SOUND_HDA_WAKEEN:
            break;
        case SOUND_HDA_STATESTS:
            break;
        case SOUND_HDA_GTS:
            break;
        case SOUND_HDA_INTR_CTRL:
            break;
        case SOUND_HDA_STREAM_SYNC:
            break;
        case SOUND_HDA_CORB_LO:
            hda->corb_lo = read_uint32_le(data);
            break;
        case SOUND_HDA_CORB_HI:
            hda->corb_hi = read_uint32_le(data);
            break;
        case SOUND_HDA_CORB_WP: {
            hda->corb_wp = read_uint16_le(data) & 0b1111111;
            uint32_t *cmd = pci_get_dma_ptr(
                hda->pci_func,
                (rvvm_addr_t) hda->corb_lo + hda->corb_wp * 4,
                4
            );
            process_codec_cmd(hda, *cmd);
            break;
        }
        case SOUND_HDA_CORB_RP: {
            uint16_t cmd = read_uint16_le(data);
            if (cmd & 0x8000) {
                hda->corb_rp = 0;
                hda_info("CORB RP is reset");
            } else {
                hda->corb_rp = cmd & 0b1111111;
                hda_info("CORB RP set to %04x", hda->corb_rp);
            }
            break;
        }
        case SOUND_HDA_CORB_CTRL: {
            uint8_t cmd = read_uint8(data);
            if (cmd & 2)
                { } // CORB start
            else
                { } // CORB stop
            break;
        }
        case SOUND_HDA_CORB_STATUS:
            break;
        case SOUND_HDA_CORB_SIZE: {
            uint8_t cmd = read_uint8(data);
            switch (cmd & 0b11) {
            case 0b00: hda->corb_size =    8; break;
            case 0b01: hda->corb_size =   64; break;
            case 0b10: hda->corb_size = 1024; break;
            case 0b11: /* Reserved */         break;
            }
            break;
        }
        case SOUND_HDA_RIRB_LO:
            hda->rirb_lo = read_uint32_le(data);
            break;
        case SOUND_HDA_RIRB_HI:
            hda->rirb_hi = read_uint32_le(data);
            break;
        case SOUND_HDA_RIRB_WP: {
            uint16_t cmd = read_uint16_le(data);
            if (cmd & 0x8000)
                hda->rirb_wp = 0;
            else
                hda->rirb_wp = cmd & 0b11111111;
            break;
        }
        case SOUND_HDA_RIRB_INTR_CNT: {
            hda->rirb_cnt = read_uint16_le(data);
            break;
        }
        case SOUND_HDA_RIRB_CTRL: {
            uint8_t cmd = read_uint8(data);
            if (cmd & 1)
                hda_info("RIRB enable response interrupt");
            else
                hda_info("RIRB disable responce interrupt");

            if (cmd & 2)
                hda_info("RIRB DMA enable");
            else
                hda_info("RIRB DMA stop");

            break;
        }
        case SOUND_HDA_RIRB_STATUS: {
            uint8_t cmd = read_uint8(data);
            hda_info("RIRB status: %02x", cmd);

            if (cmd & 0x01)
                hda->rirb_status &= ~0x1;

            if (cmd & 0x02)
                hda->rirb_status &= ~0x2;

            break;
        }
        case SOUND_HDA_RIRB_SIZE: {
            uint8_t cmd = read_uint8(data);
            switch (cmd & 0b11) {
            case 0b00: hda->rirb_size =   16; break;
            case 0b01: hda->rirb_size =  128; break;
            case 0b10: hda->rirb_size = 2048; break;
            case 0b11: /* Reserved */         break;
            }
            hda_info("RIRB size: %d", hda->rirb_size);
            break;
        }
        case SOUND_HDA_DMA_LO:
            break;
        case SOUND_HDA_DMA_HI:
            break;
        case SOUND_HDA_OSD0CTL:
            process_output_stream_ctl(hda, read_uint32_le(data));
            break;
        case SOUND_HDA_OSD0STS: {
            uint8_t cmd = read_uint8(data);

            uint8_t bcis  = (cmd >> 2) & 0b1;
            uint8_t fifoe = (cmd >> 3) & 0b1;
            uint8_t dese  = (cmd >> 4) & 0b1;

            hda_info("Write SOUND_HDA_OSD0STS: %02x", cmd);

            if (bcis)
                hda->stream_output.status &= ~0x04;

            if (fifoe)
                hda->stream_output.status &= ~0x08;

            if (dese)
                hda->stream_output.status &= ~0x10;

            break;
        }
        case SOUND_HDA_OSD0LPIB:
            break;
        case SOUND_HDA_OSD0CBL: {
            uint32_t cmd = read_uint32_le(data);
            hda->stream_output.bdl_len = cmd;
            hda_info("BDL length set to: %08x", cmd);
            hda_info("BDL length set to: %08x", hda->stream_output.bdl_len);
            break;
        }
        case SOUND_HDA_OSD0LVI:
            break;
        case SOUND_HDA_OSD0FIFOS:
            break;
        case SOUND_HDA_OSD0FMT: {
            uint16_t cmd = read_uint16_le(data);
            hda_info("OSD0FMT: %04x", cmd);

            uint8_t base = (cmd >> 14) & 0b0001; // 14
            uint8_t mult = (cmd >> 11) & 0b0111; // 13:11
            uint8_t div  = (cmd >>  8) & 0b0111; // 10:8
            uint8_t bits = (cmd >>  4) & 0b0111; // 6:4
            uint8_t chan = (cmd >>  0) & 0b1111; // 3:0

            hda_info("OSD0FMT base: %04x", base);
            hda_info("OSD0FMT mult: %04x", mult);
            hda_info("OSD0FMT div:  %04x", div);
            hda_info("OSD0FMT bits: %04x", bits);
            hda_info("OSD0FMT chan: %04x", chan);
            break;
        }
        case SOUND_HDA_OSD0BDPL:
            hda->stream_output.bdl_lo = read_uint32_le(data);
            break;
        case SOUND_HDA_OSD0BDPU:
            hda->stream_output.bdl_hi = read_uint32_le(data);
            break;
        default:
            spin_unlock(&hda->lock);
            return false;
    }

    spin_unlock(&hda->lock);

    return true;
}

PUBLIC pci_dev_t* sound_hda_init(pci_bus_t* pci_bus)
{
    sound_hda_dev_t *sound_hda = safe_new_obj(sound_hda_dev_t);

#if 0 /* Intel HDA */
    pci_func_desc_t sound_hda_desc = {
        .vendor_id  = 0x8086, // Intel
        .device_id  = 0x2668, // HDA Controller
        .class_code = 0x0403, // Audio device
        .prog_if    = 0x00,
        .irq_pin    = PCI_IRQ_PIN_INTA,
        .bar[0] = {
            .size        = 0x1000,
            .min_op_size = 4,
            .max_op_size = 4,
            .read        = sound_hda_mmio_read,
            .write       = sound_hda_mmio_write,
            .data        = sound_hda,
            .type        = &sound_hda_type
        },
    };
#else /* C-Media */
    pci_func_desc_t sound_hda_desc = {
        .vendor_id  = SOUND_VENDOR_ID_CMEDIA,
        .device_id  = SOUND_DEVICE_ID_CMEDIA,
        .class_code = SOUND_CLASS_CODE_CMEDIA,
        .prog_if    = 0x00,
        .irq_pin    = PCI_IRQ_PIN_INTA,
        .bar[0] = {
            .size        = 0x4000,
            .min_op_size = 1,
            .max_op_size = 4,
            .read        = sound_hda_mmio_read,
            .write       = sound_hda_mmio_write,
            .data        = sound_hda,
            .type        = &sound_hda_type
        },
    };
#endif

    pci_dev_t* pci_dev = pci_attach_func(pci_bus, &sound_hda_desc);
    if (pci_dev) {
        // Successfully plugged in
        sound_hda->pci_func = pci_get_device_func(pci_dev, 0);
    }

    return pci_dev;
}

PUBLIC pci_dev_t* sound_hda_init_auto(rvvm_machine_t* machine)
{
    return sound_hda_init(rvvm_get_pci_bus(machine));
}
/*
sound-hda.c - HD Audio
Copyright (C) 2025  David Korenchuk <github.com/epoll-reactor-2>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "sound-hda.h"
#include "atomics.h"
#include "compiler.h"
#include "mem_ops.h"
#include "rvtimer.h"
#include "threading.h"
#include "spinlock.h"
#include "utils.h"

PUSH_OPTIMIZATION_SIZE

#define SOUND_VENDOR_ID_CMEDIA        0x13f6 // C-Media
#define SOUND_DEVICE_ID_CMEDIA        0x5011 // CM8888 HDA Controller
#define SOUND_CLASS_CODE_CMEDIA       0x0403 // Audio device

#define SOUND_HDA_FIFO_SIZE           0x100

#define SOUND_HDA_GCAP                0x00 // Global capabilities
#define SOUND_HDA_VS                  0x02 // 0x02 minor, 0x03 major
#define SOUND_HDA_OUTPAY              0x04 // Output payload capability
#define SOUND_HDA_INPAY               0x06 // Input payload capability
#define SOUND_HDA_GLOBAL_CTRL         0x08 // Global Control
#define SOUND_HDA_WAKEEN              0x0C // Wake enable
#define SOUND_HDA_STATESTS            0x0E // State Change Status
#define SOUND_HDA_GSTS                0x10 // Global status
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
#define SOUND_HDA_ICW                 0x60 // Immediate Command Output (§3.4.1)
#define SOUND_HDA_IRR                 0x64 // Immediate Response Input  (§3.4.2)
#define SOUND_HDA_ICS                 0x68 // Immediate Command Status  (§3.4.3)
#define SOUND_HDA_DMA_LO              0x70 // DMA Position Lower Base Address
#define SOUND_HDA_DMA_HI              0x74 // DMA Position Upper Base Address
#define SOUND_HDA_WALCLKA             0x2030 // Wall Clock Counter Alias (§3.3.44)

// Stream descriptor blocks (HDA spec §3.3.34, page 27): identical 0x20-byte
// register layout for every input, output, and bidirectional descriptor.
// Per-register offsets aren't enumerated here — the SD register table near
// the end of the typedef block is the single source of truth, and the
// MMIO dispatch resolves (offset → stream index, sub-offset → register)
// automatically.
//
// Today: NO_IN=1 → ISD0 at 0x80; NO_OUT=1 → OSD0 at 0xA0; NO_BSS=0.

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

#define SOUND_HDA_PARAM_CORBSZCAP     1 /*   8 bytes =  2 entries */ \
                                    | 2 /*  64 bytes = 16 entries */ \
                                    | 4 /* 256 bytes = 32 entries */ \
                                    | 8

#define SOUND_HDA_PARAM_RIRBSZCAP     1 /*   16 bytes =   2 entries */ \
                                    | 2 /*  128 bytes =  16 entries */ \
                                    | 4 /* 2048 bytes = 256 entries */ \
                                    | 8

#define SOUND_HDA_PARAM_CORBSIZE      2 /*  256 bytes =  32 entries */
#define SOUND_HDA_PARAM_RIRBSIZE      2 /* 2048 bytes = 256 entries */

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
#define VERB_SET_AMP_GAIN_MUTE_MUTE                      0x80   // Payload bit 7
#define VERB_SET_AMP_GAIN_MUTE_GAIN_MASK                 0x7F   // Payload bits 6:0
#define VERB_SET_AMP_GAIN_MUTE_OUTPUT                    0x8000 // Bit 15: Set Output Amp
#define VERB_SET_AMP_GAIN_MUTE_INPUT                     0x4000 // Bit 14: Set Input Amp
#define VERB_SET_AMP_GAIN_MUTE_LEFT                      0x2000 // Bit 13: Set Left  (channel 0)
#define VERB_SET_AMP_GAIN_MUTE_RIGHT                     0x1000 // Bit 12: Set Right (channel 1)

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
#define VERB_GET_CONFIG_DEFAULT_ASSOCIATION_DEFAULT      ( 1 <<  4)
#define VERB_GET_CONFIG_DEFAULT_COLOR_UNKNOWN            ( 0 << 12)
#define VERB_GET_CONFIG_DEFAULT_COLOR_BLACK              ( 1 << 12)
#define VERB_GET_CONFIG_DEFAULT_COLOR_GREY               ( 2 << 12)
#define VERB_GET_CONFIG_DEFAULT_COLOR_BLUE               ( 3 << 12)
#define VERB_GET_CONFIG_DEFAULT_COLOR_GREEN              ( 4 << 12)
#define VERB_GET_CONFIG_DEFAULT_COLOR_RED                ( 5 << 12)
#define VERB_GET_CONFIG_DEFAULT_COLOR_ORANGE             ( 6 << 12)
#define VERB_GET_CONFIG_DEFAULT_COLOR_YELLOW             ( 7 << 12)
#define VERB_GET_CONFIG_DEFAULT_COLOR_PURPLE             ( 8 << 12)
#define VERB_GET_CONFIG_DEFAULT_COLOR_PINK               ( 9 << 12)
#define VERB_GET_CONFIG_DEFAULT_COLOR_WHITE              (14 << 12)
#define VERB_GET_CONFIG_DEFAULT_COLOR_OTHER              (15 << 12)
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

// HDA stream rates. Single source of truth for both the
// CODEC_PARAM_SUPP_PCM_SIZE_RATES advertisement bitmask (parameter
// 0x0A, bits 0..10) and the stream format register encoding (HDA
// spec 7.3.3.10: base / multiplier / divisor in SDnFMT bits 14, 13:11,
// 10:8). Mirrors the autodetected portion of Linux's rate_bits[] in
// sound/hda/hdac_device.c. 384 kHz (bit 11) is reserved on real HW
// and intentionally omitted.
//
// X(hz, bit_pos, base_khz, mult, div):
//   hz       — sample rate in Hz
//   bit_pos  — bit position in SUPP_PCM_SIZE_RATES
//   base_khz — 44 or 48 (selects base-rate bit 14 in SDnFMT)
//   mult     — rate multiplier (1..4), encoded as (N-1) in bits 13:11
//   div      — rate divisor    (1..8), encoded as (N-1) in bits 10:8
#define HDA_RATE_TABLE(X)         \
    X(  8000,  0, 48, 1, 6)       \
    X( 11025,  1, 44, 1, 4)       \
    X( 16000,  2, 48, 1, 3)       \
    X( 22050,  3, 44, 1, 2)       \
    X( 32000,  4, 48, 2, 3)       \
    X( 44100,  5, 44, 1, 1)       \
    X( 48000,  6, 48, 1, 1)       \
    X( 88200,  7, 44, 2, 1)       \
    X( 96000,  8, 48, 2, 1)       \
    X(176400,  9, 44, 4, 1)       \
    X(192000, 10, 48, 4, 1)

enum {
#define HDA_RATE_BIT_ENUM(hz, bit, base, mult, div) HDA_RATE_BIT_POS_##hz = bit,
    HDA_RATE_TABLE(HDA_RATE_BIT_ENUM)
#undef HDA_RATE_BIT_ENUM
};

enum {
#define HDA_FMT_ENUM(hz, bit, base, mult, div) \
    HDA_FMT_RATE_##hz = (((base) == 44 ? 1u : 0u) << 14) \
                      | (((mult) - 1) << 11) \
                      | (((div)  - 1) <<  8),
    HDA_RATE_TABLE(HDA_FMT_ENUM)
#undef HDA_FMT_ENUM
};

// Bit in CODEC_PARAM_SUPP_PCM_SIZE_RATES advertising a given Hz rate.
// Compile-time constant; pass a literal Hz value present in
// HDA_RATE_TABLE (otherwise expands to an undeclared identifier).
#define HDA_RATE_BIT(hz)         (1u << HDA_RATE_BIT_POS_##hz)

// HDA bit-depth table. Single source of truth for both the SDnFMT BITS
// field (spec 3.3.41 / 3.7.1, bits 6:4) and the matching advertisement
// bits in CODEC_PARAM_SUPP_PCM_SIZE_RATES (spec 7.3.4.7, bits 16..20).
//
// Container size is *not* ceil(bits/8): the spec mandates 32-bit
// containers (4 bytes) for both 20-bit and 24-bit samples. Encoding the
// container size in the same table the worker reads is the whole point —
// the previous nested ternary in the worker derived bytes_per_sample
// from the bit count and got 20/24-bit wrong by 1 byte (33% pacing
// error), masked only because the codec advertised 16-bit only.
//
// X(bits, code, container_bytes, advert_bit):
//   bits            — sample width, used only for naming
//   code            — value of SDnFMT BITS field (0..4); 5..7 reserved
//   container_bytes — bytes per sample in memory (1, 2, or 4)
//   advert_bit      — bit position in SUPP_PCM_SIZE_RATES (16..20)
#define HDA_BITS_TABLE(X)        \
    X( 8, 0, 1, 16)              \
    X(16, 1, 2, 17)              \
    X(20, 2, 4, 18)              \
    X(24, 3, 4, 19)              \
    X(32, 4, 4, 20)

// Advertisement-bit constants HDA_PCM_SIZE_8..HDA_PCM_SIZE_32 generated
// from the bit-depth table; OR them into a SUPP_PCM_SIZE_RATES response.
enum {
#define HDA_PCM_SIZE_ENUM(bits, code, bytes, ad) HDA_PCM_SIZE_##bits = (1u << (ad)),
    HDA_BITS_TABLE(HDA_PCM_SIZE_ENUM)
#undef HDA_PCM_SIZE_ENUM
};

// Container size in bytes indexed by SDnFMT BITS field (0..7). Reserved
// codes 5..7 default-init to 0; callers treat 0 as an invalid format and
// bail (see sound_hda_stream_drain).
static const uint8_t hda_fmt_container_bytes[8] = {
#define HDA_BITS_BYTES_INIT(bits, code, bytes, ad) [code] = (bytes),
    HDA_BITS_TABLE(HDA_BITS_BYTES_INIT)
#undef HDA_BITS_BYTES_INIT
};

// Output amp caps: chosen to give a 16 dB range with 0.5 dB resolution,
// matching what consumer HDA codecs (Realtek ALC1220, IDT 92HD83, etc.)
// typically advertise. The wider 74 dB range we used to advertise made
// amixer's linear percent display feel useless because most of the
// slider sat in perceptually-inaudible territory.
//
//   STEPSIZE encoded value: actual_step_dB = (encoded + 1) * 0.25
//                           → encoded 1 = 0.5 dB per step
//   NUMSTEPS encoded value: maximum gain step value (0..NumSteps incl.)
//                           → 32 → 33 step values, range = 32 * 0.5 = 16 dB
//   OFFSET   encoded value: gain step that produces 0 dB
//                           → 32 → step 32 = 0 dB, step 0 = -16 dB
// Bit packing happens in the CODEC_PARAM_OUTPUT_AMP_CAPS_* macros below;
// these symbolic forms are kept up here so hda_gain_to_q15 and the
// amp-reset defaults can reference them by name.
#define HDA_AMP_NUMSTEPS         32
#define HDA_AMP_OFFSET           32
#define HDA_AMP_STEPSIZE_ENC      1   // → 0.5 dB per step

// HDA gain step → Q15 amplitude factor. Reads HDA_AMP_OFFSET and
// HDA_AMP_STEPSIZE_ENC so changing the advertised amp caps doesn't
// break the conversion math.
//
// Convert without libm: each 6 dB ≈ ÷2, so the factor decomposes into
// `shift` halvings plus a 12-entry half-dB mantissa for the 0..5.5 dB
// residue. Accuracy is within ~0.1 dB of `pow(10, -atten/20)` — well
// below the perceptual threshold (~1 dB) — and fixed-point so it works
// whether USE_FPU is enabled or not.
// Verified vs `round(pow(10, -dB/20) * 32768)`; max error ±0.0002 dB.
static const uint16_t hda_gain_db_mantissa_half[12] = {
    32768, // -0.0 dB → 1.000
    30935, // -0.5 dB → 0.944
    29205, // -1.0 dB → 0.891
    27571, // -1.5 dB → 0.841
    26029, // -2.0 dB → 0.794
    24573, // -2.5 dB → 0.750
    23198, // -3.0 dB → 0.708
    21900, // -3.5 dB → 0.668
    20675, // -4.0 dB → 0.631
    19519, // -4.5 dB → 0.596
    18427, // -5.0 dB → 0.562
    17396, // -5.5 dB → 0.531
};

static int32_t hda_gain_to_q15(uint8_t gain, uint8_t mute)
{
    if (mute)                     return 0;
    if (gain >= HDA_AMP_OFFSET)   return 32768;   // 0 dB or above (clamp)
    // attenuation in quarter-dB units = (offset - gain) * step_quarter_db,
    // where step_quarter_db = (encoded + 1).
    int atten_qdb = ((int)HDA_AMP_OFFSET - (int)gain) * (HDA_AMP_STEPSIZE_ENC + 1);
    int atten_hdb = atten_qdb / 2;        // half-dB units
    int shift     = atten_hdb / 12;       // 6 dB = 12 half-dB ≈ ÷2
    int rem       = atten_hdb % 12;
    return (int32_t)hda_gain_db_mantissa_half[rem] >> shift;
}

// Sized MMIO load/store. Use these in every register handler instead of
// bare read_uint{8,16,32}_le — they respect the access width the bus
// reports, so 1-byte writes to a 4-byte register don't pull garbage from
// the surrounding payload (which is uninitialised stack on the dispatch
// path), and reads return only the bytes the guest asked for. Static
// inline because they're 100% used inside this file; no point in
// promoting to a shared header until a second device wants them.
static inline uint32_t mmio_load(const void *data, uint8_t size)
{
    switch (size) {
        case 1:  return read_uint8(data);
        case 2:  return read_uint16_le(data);
        case 4:  return read_uint32_le(data);
        default: return 0;
    }
}

static inline void mmio_store(void *data, uint8_t size, uint32_t val)
{
    switch (size) {
        case 1: write_uint8(data, (uint8_t)val); break;
        case 2: write_uint16_le(data, (uint16_t)val); break;
        case 4: write_uint32_le(data, val); break;
    }
}

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
// Bit-packed forms of the amp caps for the GET_PARAMETER response.
// The numeric constants live up at the top of the file (HDA_AMP_*) so
// hda_gain_to_q15 and the reset paths can reference them by name.
#define CODEC_PARAM_OUTPUT_AMP_CAPS_STEPSIZE             (HDA_AMP_STEPSIZE_ENC << 16)
#define CODEC_PARAM_OUTPUT_AMP_CAPS_NUMSTEPS             (HDA_AMP_NUMSTEPS     <<  8)
#define CODEC_PARAM_OUTPUT_AMP_CAPS_OFFSET               (HDA_AMP_OFFSET       <<  0)

#define CODEC_PARAM_CONN_LIST_LEN                        0x0E
#define CODEC_PARAM_SUPP_POWER_STATES                    0x0F
#define CODEC_PARAM_PROCESSING_CAPS                      0x10
#define CODEC_PARAM_GPIO_CNT                             0x11
#define CODEC_PARAM_VOLUME_KNOB                          0x12

// Stream descriptor address layout (HDA spec §3.3 page 27):
//   ISDn at 0x80 + n*0x20                   for n in [0, ISS)
//   OSDn at 0x80 + (ISS + n)*0x20           for n in [0, OSS)
//   BSDn at 0x80 + (ISS + OSS + n)*0x20     for n in [0, BSS)
// Descriptor index — used as the SIE/SIS bit in INTCTL/INTSTS per
// §3.3.14 — increases sequentially in that order.
#define HDA_STREAM_BASE         0x80
#define HDA_STREAM_STRIDE       0x20
#define HDA_STREAMS_TOTAL       (SOUND_HDA_PARAM_NO_IN  \
                               + SOUND_HDA_PARAM_NO_OUT \
                               + SOUND_HDA_PARAM_NO_BSS)
#define HDA_STREAM_REGION_END   (HDA_STREAM_BASE + HDA_STREAMS_TOTAL * HDA_STREAM_STRIDE)

typedef enum {
    HDA_STREAM_DIR_INPUT  = 0,
    HDA_STREAM_DIR_OUTPUT = 1,
    HDA_STREAM_DIR_BIDIR  = 2,
} hda_stream_dir_t;

// Forward decl so sound_hda_stream_t can keep a back-pointer for the
// worker thread. Worker arg is a sound_hda_stream_t*; the dev pointer
// rides along inside the stream so we don't need a heap-allocated
// (dev, stream) tuple per stream.
struct sound_hda_dev_s;
typedef struct sound_hda_dev_s sound_hda_dev_t;

typedef struct {
    sound_hda_dev_t *hda;      // Back-ref to owning dev — set at init,
                               // never reseated. Lets the worker find
                               // pci_func / lock from a stream pointer.
    uint8_t     index;         // Descriptor index (0..HDA_STREAMS_TOTAL-1).
                               // Equals the SIE/SIS bit position per spec
                               // §3.3.14 — caches it as intsts_bit for the
                               // hot IRQ path even though they're identical.
    uint8_t     intsts_bit;
    uint8_t     dir;           // hda_stream_dir_t
    uint32_t    bdl_lvi;       // Spec stores 8 bits; widened to 32 here so
                               // the storage-register dispatch can pass a
                               // single offsetof()/sizeof() pair.
    uint32_t    bdl_lo;
    uint32_t    bdl_hi;
    uint32_t    bdl_len;
    uint32_t    lpib;
    uint8_t     ioce;
    uint8_t     feie;          // SDnCTL bit 3 — stored for RMW round-trip
    uint8_t     deie;          // SDnCTL bit 4 — stored for RMW round-trip
    uint8_t     srst;          // SDnCTL bit 0 — mirrored for the SRST 0→1 edge
    uint8_t     stripe;        // SDnCTL bits 17:16 — stored for round-trip
    uint8_t     tp;            // SDnCTL bit 18 — stored for round-trip
    uint8_t     ctl_strm;      // SDnCTL bits 23:20 — stream tag set by guest
                               // driver. Mirrors the codec-side `stream` field
                               // below (verb VERB_SET_CONV_STREAM_CHAN); the
                               // two are required to match per spec §7.3.3.8
                               // and the codec uses the verb-side one.
    uint8_t     stream;        // Codec-side stream tag from
                               // VERB_SET_CONV_STREAM_CHAN.
    uint8_t     channel;
    uint32_t    running;       // Guest intent: 1 = stream should be running
    uint32_t    worker_alive;  // Worker lifetime: 1 = worker thread exists
    uint16_t    fmt;           // SDnFMT register (HDA spec 7.3.3.10): full 16
                               // bits — channels(0:3), size(4:6), divisor(8:10),
                               // multiplier(11:13), base(14). Worker reads this
                               // to derive the pacing rate; truncating to 8 bits
                               // loses divisor / multiplier / base and leaves the
                               // worker pacing as 48 kHz 8-bit mono regardless of
                               // what the guest configured.
    uint8_t     status;
    uint8_t     left_gain;
    uint8_t     right_gain;
    uint8_t     left_mute;
    uint8_t     right_mute;
    int32_t     gain_q15;      // Cached amplitude factor for the worker.
                               // Q15 fixed point: 32768 = unity (0 dB),
                               // 0 = full mute. Recomputed in
                               // VERB_SET_AMP_GAIN_MUTE; default unity
                               // (codec power-on per HDA spec §7.3.3.7
                               // is "Gain field defaults to Offset" =
                               // 0 dB). Worker reads lockless — torn
                               // 32-bit reads are aligned/atomic on
                               // x86/arm64 and a stale value across one
                               // chunk is inaudible.
} sound_hda_stream_t;

struct sound_hda_dev_s {
    pci_func_t* pci_func;
    spinlock_t  lock;
    uint32_t    gctl;
    uint16_t    wakeen;        // §3.3.8  — preserved across CRST
    uint16_t    statests;      // §3.3.9  — codec presence; bit 0 = our codec
    uint16_t    gsts;          // §3.3.10 — flush status (we don't model flush)
    uint32_t    ssync;         // §3.3.17 — stream sync, round-trip only
    uint32_t    intr_ctrl;
    uint32_t    corb_lo;
    uint32_t    corb_hi;
    uint16_t    corb_rp;
    uint8_t     corb_rprst;    // CORBRP bit 15 (RST). RW handshake state:
                               // SW writes 1 → HW echoes 1 on read; SW writes
                               // 0 → echoes 0. Linux's azx_clear_corbrp polls
                               // for both transitions (§3.3.21). Stored
                               // separately from corb_rp so the walk can use
                               // corb_rp as a clean entry index.
    uint16_t    corb_wp;
    uint32_t    corb_size;
    uint8_t     corbctl;       // §3.3.22 — CORBRUN/CMEIE; round-trip only
    uint8_t     corbsts;       // §3.3.23 — CORB error status; never set by us
    uint32_t    rirb_lo;
    uint32_t    rirb_hi;
    uint32_t    rirb_rp;
    uint32_t    rirb_wp;
    uint32_t    rirb_size;
    uint32_t    rirb_cnt;
    uint32_t    rirb_status;
    uint8_t     rirbctl;       // §3.3.29 — RIRBDMAEN/RINTCTL; round-trip only
    uint32_t    dplbase;       // §3.3.32 — DMA Position Buffer base low
    uint32_t    dpubase;       // §3.3.33 — DMA Position Buffer base high
    uint32_t    icw;           // §3.4.1  — Immediate Command Output
    uint32_t    irr;           // §3.4.2  — Immediate Response Input
    uint16_t    ics;           // §3.4.3  — Immediate Command Status
    uint32_t    power_state;
    // Beep Generator widget (NID 4, §7.2.3.8 / §7.3.3.31). Independent
    // of stream playback — beep tone runs whenever beep_divider != 0,
    // regardless of whether any stream descriptor is RUN. Worker
    // lifecycle parallels the per-stream worker: spawn on first non-zero
    // SET_BEEP_GENERATION via CAS on beep_worker_alive, the worker
    // exits when it sees beep_running=0.
    uint8_t     beep_divider;  // §7.3.3.31 payload — 0 = off, N = 48000/(4N) Hz
    uint8_t     beep_mute;
    uint8_t     beep_gain;
    uint32_t    beep_running;  // guest intent: 1 = generate tone
    uint32_t    beep_worker_alive;
    uint8_t     pin_ctrl_out;  // NID 3 pin widget control byte (§7.3.3.13).
                               // Bit 6 OUT_ENABLE, 5 IN_ENABLE, 7 HPHN, 0 VREF.
                               // Default = OUT_ENABLE so probe-time GET returns
                               // the same value the previous hardcoded handler
                               // did.
    uint8_t     pin_ctrl_in;   // NID 6 (Mic-In) pin widget control byte.
                               // Linux's HDA generic pin power-up sets bit 5
                               // IN_ENABLE before recording starts; default to
                               // 0 so the guest can detect that it owns the
                               // configuration.

    sound_hda_stream_t streams[HDA_STREAMS_TOTAL];
    sound_subsystem_t  subsystem;
};

// Convenience accessor for the (currently single) output stream. The codec
// verbs route there directly; the MMIO dispatch addresses streams by
// descriptor index and doesn't need it.
#define HDA_OUTPUT_STREAM_INDEX  SOUND_HDA_PARAM_NO_IN
static inline sound_hda_stream_t *hda_output_stream(sound_hda_dev_t *hda) {
    return &hda->streams[HDA_OUTPUT_STREAM_INDEX];
}

// Forward decl — SDnCTL action body lives further down with the rest of
// the worker / start-stop machinery.
static void sound_hda_stream_ctl_action(sound_hda_dev_t *hda,
                                        sound_hda_stream_t *stream,
                                        uint32_t value);

// === Stream descriptor register dispatch ===
//
// All input, output, and bidirectional stream descriptors share the same
// 0x20-byte register layout (HDA spec §3.3.34). One table services every
// stream — adding ISD0 / multi-stream means changing HDA_STREAMS_TOTAL,
// not the table.
//
// Registers are categorised by `kind`:
//   SD_REG_RW_FIELD — read returns the stored field, writes store into it
//   SD_REG_RO_FIELD — read returns the stored field, writes ignored
//   SD_REG_RO_FN    — read calls a compute callback, writes ignored
//   SD_REG_ACTION   — both directions go through callbacks (used for
//                     SDnCTL where bits compose from multiple fields and
//                     the write must spawn / signal the worker, and for
//                     SDnSTS where reads compute FIFORDY and writes are
//                     RW1C against `status`).
typedef enum {
    SD_REG_RW_FIELD = 0,
    SD_REG_RO_FIELD,
    SD_REG_RO_FN,
    SD_REG_ACTION,
} sd_reg_kind_t;

typedef uint32_t (*sd_reg_read_fn) (sound_hda_dev_t*, sound_hda_stream_t*);
typedef void     (*sd_reg_write_fn)(sound_hda_dev_t*, sound_hda_stream_t*, uint32_t);

typedef struct {
    uint16_t        sub_off;     // offset within the 0x20 stream block
    uint8_t         width;       // canonical register width (1, 2, or 4)
    sd_reg_kind_t   kind;
    uint16_t        field_off;   // offsetof(sound_hda_stream_t, field)
    uint8_t         field_size;  // sizeof(field) — 1, 2, or 4
    sd_reg_read_fn  read_fn;     // ACTION / RO_FN
    sd_reg_write_fn write_fn;    // ACTION
    const char     *name;        // diagnostics; matches the spec mnemonic
} sd_reg_t;

// Read/write a struct field at (offset, size) as a little-endian
// unsigned integer of the requested width. Used by both the SD register
// table (base = sound_hda_stream_t*) and the global controller register
// table (base = sound_hda_dev_t*) — same shape of operation, different
// host struct, so the helper is generic over the base pointer.
static uint32_t reg_field_load(void *base, uint16_t off, uint8_t sz)
{
    uint8_t *p = (uint8_t*)base + off;
    switch (sz) {
        case 1:  return *(uint8_t  *)p;
        case 2:  return *(uint16_t *)p;
        case 4:  return *(uint32_t *)p;
        default: return 0;
    }
}

static void reg_field_store(void *base, uint16_t off, uint8_t sz, uint32_t v)
{
    uint8_t *p = (uint8_t*)base + off;
    switch (sz) {
        case 1: *(uint8_t  *)p = (uint8_t)v;  break;
        case 2: *(uint16_t *)p = (uint16_t)v; break;
        case 4: *(uint32_t *)p = v;           break;
    }
}

// SDnCTL (HDA spec §3.3.35): 24-bit register; bits 0 SRST, 1 RUN, 2
// IOCE, 3 FEIE, 4 DEIE, 17:16 STRIPE, 18 TP, 19 DIR (RO 0 unless bidir),
// 23:20 STRM (stream tag). Composed on read; write defers to the
// action body further down (worker spawn, SRST edge, etc.).
static uint32_t sd_ctl_read(sound_hda_dev_t *hda, sound_hda_stream_t *s)
{
    (void)hda;
    uint32_t v = 0;
    v |= (s->srst       & 1u) << 0;
    v |= (atomic_load_uint32_relax(&s->running) ? 1u : 0u) << 1;
    v |= (s->ioce       & 1u) << 2;
    v |= (s->feie       & 1u) << 3;
    v |= (s->deie       & 1u) << 4;
    v |= ((uint32_t)s->stripe  & 0x3u) << 16;
    v |= (s->tp         & 1u) << 18;
    v |= (s->dir == HDA_STREAM_DIR_BIDIR ? 1u : 0u) << 19;
    v |= ((uint32_t)s->ctl_strm & 0xFu) << 20;
    return v;
}

static void sd_ctl_write(sound_hda_dev_t *hda, sound_hda_stream_t *s, uint32_t v)
{
    sound_hda_stream_ctl_action(hda, s, v);
}

// SDnSTS (HDA spec §3.3.36): bits 2 BCIS, 3 FIFOE, 4 DESE are RW1C
// against `status`; bit 5 FIFORDY is computed RO. The worker latches
// BCIS into `status` under hda->lock; reads return it as-is.
static uint32_t sd_sts_read(sound_hda_dev_t *hda, sound_hda_stream_t *s)
{
    (void)hda;
    uint32_t v = s->status & 0x1Cu;
    if (s->lpib < s->bdl_len) v |= (1u << 5);
    return v;
}

static void sd_sts_write(sound_hda_dev_t *hda, sound_hda_stream_t *s, uint32_t v)
{
    (void)hda;
    s->status &= ~(uint8_t)(v & 0x1Cu);
}

// SDnFIFOS (HDA spec §3.3.40): max FIFO depth in bytes. Reset value is
// implementation-specific; we report a constant for both directions.
static uint32_t sd_fifos_read(sound_hda_dev_t *hda, sound_hda_stream_t *s)
{
    (void)hda; (void)s;
    return SOUND_HDA_FIFO_SIZE;
}

#define SD_FIELD_OFF(field)   offsetof(sound_hda_stream_t, field)
#define SD_FIELD_SIZE(field)  sizeof(((sound_hda_stream_t*)0)->field)
#define SD_RW(off, w, field) \
    { (off), (w), SD_REG_RW_FIELD, SD_FIELD_OFF(field), SD_FIELD_SIZE(field), NULL, NULL, #field }
#define SD_RO(off, w, field) \
    { (off), (w), SD_REG_RO_FIELD, SD_FIELD_OFF(field), SD_FIELD_SIZE(field), NULL, NULL, #field }

// Stream descriptor register table (§3.3.35–§3.3.43). Adding a new
// register or wiring a missing one is a one-row change; the dispatch
// below has no per-register knowledge.
static const sd_reg_t sd_regs[] = {
    { 0x00, 4, SD_REG_ACTION, 0, 0, sd_ctl_read,   sd_ctl_write,  "SDnCTL"   },
    { 0x03, 1, SD_REG_ACTION, 0, 0, sd_sts_read,   sd_sts_write,  "SDnSTS"   },
    SD_RO(0x04, 4, lpib),       // SDnLPIB — RO; worker advances this
    SD_RW(0x08, 4, bdl_len),    // SDnCBL  — Cyclic Buffer Length
    SD_RW(0x0C, 2, bdl_lvi),    // SDnLVI  — Last Valid Index
    { 0x10, 2, SD_REG_RO_FN,  0, 0, sd_fifos_read, NULL,          "SDnFIFOS" },
    SD_RW(0x12, 2, fmt),        // SDnFMT  — decoded by stream worker
    SD_RW(0x18, 4, bdl_lo),     // SDnBDPL — BDL pointer low (128-B aligned)
    SD_RW(0x1C, 4, bdl_hi),     // SDnBDPU — BDL pointer high (RO 0 if !64-bit)
};

#undef SD_RW
#undef SD_RO
#undef SD_FIELD_OFF
#undef SD_FIELD_SIZE

static const sd_reg_t *sd_reg_lookup(uint16_t sub_off)
{
    for (size_t i = 0; i < sizeof(sd_regs) / sizeof(sd_regs[0]); ++i) {
        if (sd_regs[i].sub_off == sub_off) return &sd_regs[i];
    }
    return NULL;
}

static bool sd_dispatch_read(sound_hda_dev_t *hda, sound_hda_stream_t *s,
                             uint16_t sub_off, void *data, uint8_t size)
{
    const sd_reg_t *r = sd_reg_lookup(sub_off);
    if (r == NULL) return false;
    uint32_t v = 0;
    switch (r->kind) {
        case SD_REG_RW_FIELD:
        case SD_REG_RO_FIELD:
            v = reg_field_load(s, r->field_off, r->field_size);
            break;
        case SD_REG_RO_FN:
        case SD_REG_ACTION:
            v = r->read_fn(hda, s);
            break;
    }
    mmio_store(data, size, v);
    return true;
}

static bool sd_dispatch_write(sound_hda_dev_t *hda, sound_hda_stream_t *s,
                              uint16_t sub_off, const void *data, uint8_t size)
{
    const sd_reg_t *r = sd_reg_lookup(sub_off);
    if (r == NULL) return false;
    uint32_t v = mmio_load(data, size);
    switch (r->kind) {
        case SD_REG_RW_FIELD:
            reg_field_store(s, r->field_off, r->field_size, v);
            break;
        case SD_REG_RO_FIELD:
        case SD_REG_RO_FN:
            // Read-only — writes ignored (no error per §3.1.2).
            break;
        case SD_REG_ACTION:
            r->write_fn(hda, s, v);
            break;
    }
    return true;
}

// === Global controller register dispatch ===
//
// Same pattern as sd_regs[] above, applied to the controller's global
// register block at offsets 0x00-0x7F (HDA spec §3.3.1-§3.3.31) plus
// the §3.4 immediate command interface and the §3.3.44 wall-clock
// alias. Adding a missing register is a one-row change; the dispatch
// has no per-register knowledge.
typedef enum {
    GR_REG_RW_FIELD = 0,
    GR_REG_RO_FIELD,
    GR_REG_RO_FN,
    GR_REG_RO_CONST,
    GR_REG_ACTION,
} gr_reg_kind_t;

typedef uint32_t (*gr_reg_read_fn) (sound_hda_dev_t*);
typedef void     (*gr_reg_write_fn)(sound_hda_dev_t*, uint32_t);

typedef struct {
    uint16_t        off;
    uint8_t         width;
    gr_reg_kind_t   kind;
    uint16_t        field_off;     // offsetof(sound_hda_dev_t, field)
    uint8_t         field_size;
    uint32_t        const_val;     // RO_CONST
    gr_reg_read_fn  read_fn;       // RO_FN / ACTION
    gr_reg_write_fn write_fn;      // ACTION
    const char     *name;
} gr_reg_t;

// Forward decls — bodies live below to keep table-construction tight.
static uint32_t sound_hda_compute_intsts(sound_hda_dev_t *hda);
static uint32_t sound_hda_wallclk(void);
static void     sound_hda_corb_wp_write(sound_hda_dev_t *hda, uint32_t v);
static void     sound_hda_codec_cmd(sound_hda_dev_t *hda, uint32_t cmd);
static void     sound_hda_controller_reset(sound_hda_dev_t *hda);
static void     sound_hda_beep_set(sound_hda_dev_t *hda, uint8_t divider);

// === Global register callbacks ===
static uint32_t gr_gctl_read(sound_hda_dev_t *hda)
{
    // GCTL bit 8 UNSOL is force-on: we accept unsolicited responses
    // unconditionally (no source generates them today, but the bit
    // advertises the capability). Bit 0 CRST round-trips honestly.
    return hda->gctl | (1u << 8);
}

static void gr_gctl_write(sound_hda_dev_t *hda, uint32_t v)
{
    // CRST 1→0: enter reset (halt streams + zero ring/state).
    // CRST 0→1: exit reset; re-set STATESTS bit 0 because the codec
    //           re-asserts presence on link out-of-reset (§3.3.9).
    uint32_t prev = hda->gctl;
    hda->gctl = v;
    if ((prev & 1u) && !(v & 1u)) {
        sound_hda_controller_reset(hda);
    } else if (!(prev & 1u) && (v & 1u)) {
        hda->statests |= 0x0001u;   // codec on SDIN[0]
    }
    // FCNTRL 0→1 (bit 1): DMA position buffer flush request (§3.3.7).
    // Spec: "The flush is complete when Flush Status is set." We have
    // no real DMA pipeline to flush, so complete the handshake
    // immediately by setting GSTS.FSTS — without this a guest that
    // does the flush dance (e.g. before suspend) would hang polling.
    if (!(prev & 0x2u) && (v & 0x2u)) {
        hda->gsts |= 0x2u;          // FSTS
    }
}

static uint32_t gr_intsts_read(sound_hda_dev_t *hda)  { return sound_hda_compute_intsts(hda); }
static uint32_t gr_wallclk_read(sound_hda_dev_t *hda) { (void)hda; return sound_hda_wallclk(); }

static void gr_corb_wp_write(sound_hda_dev_t *hda, uint32_t v)
{
    sound_hda_corb_wp_write(hda, v);
}

static uint32_t gr_corb_rp_read(sound_hda_dev_t *hda)
{
    // §3.3.21: bit 15 (CORBRPRST) is RW with a handshake — software
    // writes 1, polls for 1 (HW acknowledgment), writes 0, polls for
    // 0. We echo the stored corb_rprst flag for the readback so
    // Linux's azx_clear_corbrp loop terminates cleanly. Bits 7:0 hold
    // the actual RP value.
    return ((uint32_t)(hda->corb_rprst & 1u) << 15) | (hda->corb_rp & 0xFFu);
}

static void gr_corb_rp_write(sound_hda_dev_t *hda, uint32_t v)
{
    // §3.3.21: bit 15 = CORBRPRST. Writing 1 resets RP and the bit
    // reads back as 1; writing 0 clears the bit and stores the new
    // RP value (low 8 bits).
    //
    // Spec also requires CORBCTL.CORBRUN to be 0 before resetting RP —
    // "or else DMA transfer may be corrupted." Our CORB consumer runs
    // synchronously under hda->lock so there's no real corruption
    // window, but a guest that violates this is bug-checking against
    // spec-conformant hardware, so silently ignore the reset to match.
    if (v & 0x8000u) {
        if (hda->corbctl & 0x2u) {
            rvvm_debug("sound-hda: CORB_RP reset requested with"
                       " CORBRUN=1; ignored per §3.3.21");
            return;
        }
        hda->corb_rprst = 1;
        hda->corb_rp    = 0;
    } else {
        hda->corb_rprst = 0;
        hda->corb_rp    = v & 0xFFu;
    }
}

static const uint32_t hda_corb_sizes[4] = { 8,  64, 1024, 0 };
static const uint32_t hda_rirb_sizes[4] = { 16, 128, 2048, 0 };

static uint32_t gr_corb_size_read(sound_hda_dev_t *hda)
{
    (void)hda;
    return (SOUND_HDA_PARAM_CORBSZCAP << 4) | SOUND_HDA_PARAM_CORBSIZE;
}

static void gr_corb_size_write(sound_hda_dev_t *hda, uint32_t v)
{
    uint32_t sz = hda_corb_sizes[v & 0x3u];
    if (sz) hda->corb_size = sz;
}

static uint32_t gr_rirb_size_read(sound_hda_dev_t *hda)
{
    (void)hda;
    return (SOUND_HDA_PARAM_RIRBSZCAP << 4) | SOUND_HDA_PARAM_RIRBSIZE;
}

static void gr_rirb_size_write(sound_hda_dev_t *hda, uint32_t v)
{
    uint32_t sz = hda_rirb_sizes[v & 0x3u];
    if (sz) hda->rirb_size = sz;
}

static void gr_rirb_wp_write(sound_hda_dev_t *hda, uint32_t v)
{
    hda->rirb_wp = (v & 0x8000u) ? 0u : (v & 0xFFu);
}

static void gr_rirb_status_write(sound_hda_dev_t *hda, uint32_t v)
{
    hda->rirb_status &= ~(v & 0x3u);   // RW1C against bits 0/1
}

static void gr_statests_write(sound_hda_dev_t *hda, uint32_t v)
{
    // §3.3.9: RW1CS — writing 1 clears the corresponding bit. Bits are
    // sticky (only cleared by power-on reset or RW1C), preserved across
    // CRST. Clear masked bits in stored value.
    hda->statests &= ~(v & 0x7FFFu);
}

static void gr_gsts_write(sound_hda_dev_t *hda, uint32_t v)
{
    // §3.3.10: only bit 1 FSTS is RW1C. We don't model flush.
    hda->gsts &= ~(v & 0x2u);
}

static void gr_corbsts_write(sound_hda_dev_t *hda, uint32_t v)
{
    // §3.3.23: bit 0 CMEI is RW1C. We never raise CORB memory errors.
    hda->corbsts &= ~(v & 0x1u);
}

// §3.4.1 forbids ICW write while ICS.ICB=1; log and accept.
static void gr_icw_write(sound_hda_dev_t *hda, uint32_t v)
{
    if (hda->ics & 0x1u) {
        rvvm_debug("sound-hda: ICW written with ICS.ICB=1; "
                   "spec §3.4.1 prohibits this");
    }
    hda->icw = v;
}

// §3.4 Immediate Command Interface — PIO-style alternative to CORB/RIRB.
// Writing ICS bit 0 (ICB=1) dispatches the value in ICW as a codec verb,
// latches the response in IRR, sets ICS bit 1 (IRV), clears ICB.
static void gr_ics_write(sound_hda_dev_t *hda, uint32_t v)
{
    if (v & 0x1u) {
        // ICB=1 → dispatch the queued verb. Mirror the CORB path: the
        // codec_cmd function writes to RIRB and raises the IRQ; we
        // additionally latch the response in IRR for the PIO path.
        // codec_cmd's RIRB write side-effect is harmless if guest is
        // using PIO exclusively (RIRB ring may be unallocated; the
        // pci_get_dma_ptr inside sound_hda_write_rirb returns NULL and
        // the function exits cleanly).
        //
        // Use a tiny shim: capture the response by intercepting the
        // RIRB write, OR just dispatch through the same verb-decoder
        // path. Simplest: inline a minimal version.
        sound_hda_codec_cmd(hda, hda->icw);
        // Response was written into RIRB at hda->rirb_wp. Read it back
        // for IRR. (rirb_lo+rirb_wp*8 holds the response dword.)
        if (hda->rirb_lo) {
            uint32_t *resp = pci_get_dma_ptr(hda->pci_func,
                (rvvm_addr_t)hda->rirb_lo + hda->rirb_wp * 8, 4);
            if (resp) hda->irr = *resp;
        }
        hda->ics = (hda->ics & ~0x1u) | 0x2u;   // clear ICB, set IRV
    }
    if (v & 0x2u) {
        hda->ics &= ~0x2u;                       // RW1C clears IRV
    }
}

#define GR_FIELD_OFF(field)   offsetof(sound_hda_dev_t, field)
#define GR_FIELD_SIZE(field)  sizeof(((sound_hda_dev_t*)0)->field)
#define GR_RW(off, w, field) \
    { (off), (w), GR_REG_RW_FIELD, GR_FIELD_OFF(field), GR_FIELD_SIZE(field), 0, NULL, NULL, #field }
#define GR_RO(off, w, field) \
    { (off), (w), GR_REG_RO_FIELD, GR_FIELD_OFF(field), GR_FIELD_SIZE(field), 0, NULL, NULL, #field }
#define GR_RO_CONST(off, w, val, name_str) \
    { (off), (w), GR_REG_RO_CONST, 0, 0, (val), NULL, NULL, name_str }
#define GR_ACTION(off, w, fname, rfn, wfn, name_str) \
    { (off), (w), GR_REG_ACTION, GR_FIELD_OFF(fname), GR_FIELD_SIZE(fname), 0, (rfn), (wfn), name_str }
#define GR_RO_FN(off, w, rfn, name_str) \
    { (off), (w), GR_REG_RO_FN, 0, 0, 0, (rfn), NULL, name_str }

// Global controller register table.
static const gr_reg_t gr_regs[] = {
    GR_RO_CONST(SOUND_HDA_GCAP,         2, SOUND_HDA_PARAM_GCAP,                     "GCAP"      ),
    GR_RO_CONST(SOUND_HDA_VS,           2, SOUND_HDA_PARAM_V,                        "VS"        ),
    GR_RO_CONST(SOUND_HDA_OUTPAY,       2, 0x3C,                                     "OUTPAY"    ),
    GR_RO_CONST(SOUND_HDA_INPAY,        2, 0x1D,                                     "INPAY"     ),
    GR_ACTION(SOUND_HDA_GLOBAL_CTRL,    4, gctl,     gr_gctl_read,    gr_gctl_write, "GCTL"      ),
    GR_RW(SOUND_HDA_WAKEEN,             2, wakeen),
    GR_ACTION(SOUND_HDA_STATESTS,       2, statests, NULL, gr_statests_write,        "STATESTS"  ),
    GR_ACTION(SOUND_HDA_GSTS,           2, gsts,     NULL, gr_gsts_write,            "GSTS"      ),
    // OUTSTRMPAY/INSTRMPAY: 0x00 = "no limit beyond OUT/INPAY". Spec
    // §3.3.11/12 — most controllers report 0 here.
    GR_RO_CONST(SOUND_HDA_OUTSTRMPAY,   2, 0x0000,                                   "OUTSTRMPAY"),
    GR_RO_CONST(SOUND_HDA_INSTRMPAY,    2, 0x0000,                                   "INSTRMPAY" ),
    GR_RW(SOUND_HDA_INTR_CTRL,          4, intr_ctrl),
    GR_RO_FN(SOUND_HDA_INTSTS,          4, gr_intsts_read,                           "INTSTS"    ),
    GR_RO_FN(SOUND_HDA_WALL_CLOCK,      4, gr_wallclk_read,                          "WALLCLK"   ),
    GR_RW(SOUND_HDA_STREAM_SYNC,        4, ssync),
    GR_RW(SOUND_HDA_CORB_LO,            4, corb_lo),
    GR_RW(SOUND_HDA_CORB_HI,            4, corb_hi),
    GR_ACTION(SOUND_HDA_CORB_WP,        2, corb_wp,  NULL, gr_corb_wp_write,         "CORBWP"    ),
    GR_ACTION(SOUND_HDA_CORB_RP,        2, corb_rp,  gr_corb_rp_read, gr_corb_rp_write, "CORBRP" ),
    GR_RW(SOUND_HDA_CORB_CTRL,          1, corbctl),
    GR_ACTION(SOUND_HDA_CORB_STATUS,    1, corbsts,  NULL, gr_corbsts_write,         "CORBSTS"   ),
    GR_ACTION(SOUND_HDA_CORB_SIZE,      1, corb_size, gr_corb_size_read, gr_corb_size_write, "CORBSIZE"),
    GR_RW(SOUND_HDA_RIRB_LO,            4, rirb_lo),
    GR_RW(SOUND_HDA_RIRB_HI,            4, rirb_hi),
    GR_ACTION(SOUND_HDA_RIRB_WP,        2, rirb_wp,  NULL, gr_rirb_wp_write,         "RIRBWP"    ),
    GR_RW(SOUND_HDA_RIRB_INTR_CNT,      2, rirb_cnt),
    GR_RW(SOUND_HDA_RIRB_CTRL,          1, rirbctl),
    GR_ACTION(SOUND_HDA_RIRB_STATUS,    1, rirb_status, NULL, gr_rirb_status_write,  "RIRBSTS"   ),
    GR_ACTION(SOUND_HDA_RIRB_SIZE,      1, rirb_size, gr_rirb_size_read, gr_rirb_size_write, "RIRBSIZE"),
    GR_ACTION(SOUND_HDA_ICW,            4, icw,      NULL, gr_icw_write,             "ICW"       ),
    GR_RW(SOUND_HDA_IRR,                4, irr),
    GR_ACTION(SOUND_HDA_ICS,            2, ics,      NULL, gr_ics_write,             "ICS"       ),
    GR_RW(SOUND_HDA_DMA_LO,             4, dplbase),
    GR_RW(SOUND_HDA_DMA_HI,             4, dpubase),
    GR_RO_FN(SOUND_HDA_WALCLKA,         4, gr_wallclk_read,                          "WALCLKA"   ),
};

#undef GR_RW
#undef GR_RO
#undef GR_RO_CONST
#undef GR_ACTION
#undef GR_RO_FN
#undef GR_FIELD_OFF
#undef GR_FIELD_SIZE

static const gr_reg_t *gr_reg_lookup(uint16_t off)
{
    for (size_t i = 0; i < sizeof(gr_regs) / sizeof(gr_regs[0]); ++i) {
        if (gr_regs[i].off == off) return &gr_regs[i];
    }
    return NULL;
}

static bool gr_dispatch_read(sound_hda_dev_t *hda, size_t offset,
                             void *data, uint8_t size)
{
    const gr_reg_t *r = gr_reg_lookup((uint16_t)offset);
    if (r == NULL) return false;
    uint32_t v = 0;
    switch (r->kind) {
        case GR_REG_RW_FIELD:
        case GR_REG_RO_FIELD:
            v = reg_field_load(hda, r->field_off, r->field_size);
            break;
        case GR_REG_RO_CONST:
            v = r->const_val;
            break;
        case GR_REG_RO_FN:
            v = r->read_fn(hda);
            break;
        case GR_REG_ACTION:
            // ACTION may have a custom read (e.g. GCTL composes UNSOL
            // bit) or default to backing-field read.
            v = r->read_fn ? r->read_fn(hda)
                           : reg_field_load(hda, r->field_off, r->field_size);
            break;
    }
    mmio_store(data, size, v);
    return true;
}

static bool gr_dispatch_write(sound_hda_dev_t *hda, size_t offset,
                              const void *data, uint8_t size)
{
    const gr_reg_t *r = gr_reg_lookup((uint16_t)offset);
    if (r == NULL) return false;
    uint32_t v = mmio_load(data, size);
    switch (r->kind) {
        case GR_REG_RW_FIELD:
            reg_field_store(hda, r->field_off, r->field_size, v);
            break;
        case GR_REG_RO_FIELD:
        case GR_REG_RO_FN:
        case GR_REG_RO_CONST:
            // Read-only — writes ignored.
            break;
        case GR_REG_ACTION:
            r->write_fn(hda, v);
            break;
    }
    return true;
}

static void sound_hda_remove(rvvm_mmio_dev_t* dev)
{
    // Unbounded join on every worker: hang-on-teardown is recoverable;
    // returning while a worker still uses hda->pci_func is UAF.
    sound_hda_dev_t *hda = dev->data;
    if (hda == NULL) return;

    for (size_t i = 0; i < HDA_STREAMS_TOTAL; ++i) {
        atomic_store_uint32_relax(&hda->streams[i].running, 0);
    }
    atomic_store_uint32_relax(&hda->beep_running, 0);
    // Unblock workers stuck inside a blocking subsystem.write.
    if (hda->subsystem.abort != NULL) {
        hda->subsystem.abort(&hda->subsystem);
    }
    uint32_t waited_ms = 0;
    for (;;) {
        bool any_alive = atomic_load_uint32_relax(&hda->beep_worker_alive);
        for (size_t i = 0; !any_alive && i < HDA_STREAMS_TOTAL; ++i) {
            if (atomic_load_uint32_relax(&hda->streams[i].worker_alive)) {
                any_alive = true;
            }
        }
        if (!any_alive) break;
        sleep_ms(5);
        waited_ms += 5;
        if (waited_ms == 5000) {
            DO_ONCE(rvvm_warn("sound_hda_remove: stream worker still alive"
                              " after 5 s; backend may be blocking"));
        }
    }
}

static rvvm_mmio_type_t sound_hda_type = {
    .name = "intel_hda",
    .remove = sound_hda_remove,
};

// INTSTS (§3.3.15): bits 0..29 per-stream SIS, bit 30 CIS, bit 31 GIS.
// SIS bit number = descriptor index (§3.3.14).
static uint32_t sound_hda_compute_intsts(sound_hda_dev_t *hda)
{
    uint32_t sis = 0;
    for (size_t i = 0; i < HDA_STREAMS_TOTAL; ++i) {
        if (hda->streams[i].status & 0x1Cu) {
            sis |= 1u << hda->streams[i].intsts_bit;
        }
    }
    if (sis) sis |= (1u << 31);  // GIS mirrors any pending SIS
    return sis;
}

// Wall clock (§3.3.16): 32-bit monotonic counter at 24 MHz. Linux's
// azx_position_ok uses it as a sanity gate against bogus period IRQs.
static uint32_t sound_hda_wallclk(void)
{
    return (uint32_t)rvtimer_clocksource(24000000ULL);
}

// Resolve an MMIO offset in the [0x80, 0x80 + N*0x20) range to a
// (stream, sub_off) pair. Returns NULL on miss (offset is global or out
// of range).
static sound_hda_stream_t *hda_resolve_stream(sound_hda_dev_t *hda,
                                              size_t offset, uint16_t *sub_off)
{
    if (offset < HDA_STREAM_BASE || offset >= HDA_STREAM_REGION_END) return NULL;
    size_t   rel = offset - HDA_STREAM_BASE;
    uint32_t idx = rel / HDA_STREAM_STRIDE;
    *sub_off = rel % HDA_STREAM_STRIDE;
    return &hda->streams[idx];
}

static bool sound_hda_mmio_read(rvvm_mmio_dev_t* dev, void* data, size_t offset, uint8_t size)
{
    sound_hda_dev_t *hda = dev->data;
    spin_lock(&hda->lock);

    // Try the global controller register table first; on miss, try the
    // stream descriptor block (HDA spec §3.3.34: shared 0x20-byte layout
    // for input/output/bidir streams).
    bool ok = gr_dispatch_read(hda, offset, data, size);
    if (!ok) {
        uint16_t sub_off;
        sound_hda_stream_t *s = hda_resolve_stream(hda, offset, &sub_off);
        if (s != NULL) ok = sd_dispatch_read(hda, s, sub_off, data, size);
    }

    spin_unlock(&hda->lock);
    return ok;
}

static void sound_hda_write_rirb(sound_hda_dev_t *hda, uint32_t cad, uint32_t response)
{
    ++hda->rirb_wp;
    hda->rirb_wp %= hda->rirb_size;

    // §3.3.27 / §4.4.2.1: 8-byte entries (response dword + response_ex dword).
    uint32_t *rirb = pci_get_dma_ptr(
        hda->pci_func,
        (rvvm_addr_t)hda->rirb_lo + hda->rirb_wp * 8,
        8
    );
    if (rirb == NULL) return;

    rirb[0] = response;
    rirb[1] = cad;       // response_ex (codec address in bits 3:0)
    // pci_send_irq just queues into the IRQ controller; safe under hda->lock.
    pci_send_irq(hda->pci_func, 0);
}

// NID = 0
static uint32_t sound_hda_codec_root_cmd(uint32_t payload)
{
    switch (payload) {
        case CODEC_PARAM_VENDOR_ID:
            // §7.3.4.1: (vendor << 16) | device.
            return ((uint32_t)SOUND_VENDOR_ID_CMEDIA << 16) | SOUND_DEVICE_ID_CMEDIA;

        case CODEC_PARAM_REVISION_ID:
            return 0xFFFF;

        case CODEC_PARAM_SUB_NODE_COUNT:
            return 0x00010001; // 1 Subnode, StartNid = 1

        default:
            return 0;
    }
}

// NID = 1
static uint32_t sound_hda_codec_fg_output_cmd(uint32_t payload)
{
    switch (payload) {
        case CODEC_PARAM_SUB_NODE_COUNT:
            // §7.3.4.3: (starting_nid << 16) | count.
            // 5 subnodes (NIDs 2..6) starting at NID 2:
            //   2 = Output Converter (DAC)
            //   3 = Pin Out (Line-Out)
            //   4 = Beep Generator
            //   5 = Input Converter (ADC)
            //   6 = Pin In (Mic-In)
            return 0x00020005;

        case CODEC_PARAM_FUNC_GROUP_TYPE:
            return CODEC_PARAM_FUNC_GROUP_TYPE_AUDIO;

        case CODEC_PARAM_SUPP_PCM_SIZE_RATES:
            // §7.3.4.7. Worker derives pacing from stream->fmt, so any
            // advertised rate is safe. HDA-Intel's `default` PCM is raw
            // hw access (no `plug` plugin) — apps fail to open at rates
            // not in this set, so cover MP3 / WAV / hi-res.
            return HDA_PCM_SIZE_16
                 | HDA_RATE_BIT(44100) | HDA_RATE_BIT(48000)
                 | HDA_RATE_BIT(88200) | HDA_RATE_BIT(96000);

        case CODEC_PARAM_SUPP_STREAM_FMTS:
            return CODEC_PARAM_SUPP_STREAM_FMTS_PCM;

        case CODEC_PARAM_SUPP_POWER_STATES:
            return 0xF; // D3, D2, D1, D0

        default:
            return 0;
    }
}

// NID = 2
static uint32_t sound_hda_codec_output_cmd(uint32_t payload)
{
    switch (payload) {
        case CODEC_PARAM_VENDOR_ID:
            // §7.3.4.1: (vendor << 16) | device.
            return ((uint32_t)SOUND_VENDOR_ID_CMEDIA << 16) | SOUND_DEVICE_ID_CMEDIA;

        case CODEC_PARAM_REVISION_ID:
            return 0xFFFF;

        case CODEC_PARAM_SUB_NODE_COUNT:
            return 0x00010001; // 1 Subnode, StartNid = 1

        case CODEC_PARAM_AUDIO_WIDGET_CAPS:
            // No STEREO bit: tell the guest driver this widget is mono
            // only. Prevents Linux from configuring a stereo stream
            // (which it will by default on stereo-capable widgets, even
            // for mono input, and then silently downmixes the input
            // duplicating mono → L+R — so the emulator byte rate
            // doubles and pacing diverges).
            return CODEC_PARAM_AUDIO_WIDGET_CAPS_OUTPUT
                 | CODEC_PARAM_AUDIO_WIDGET_CAPS_FORMAT_OVR
                 | CODEC_PARAM_AUDIO_WIDGET_CAPS_AMP_OVR
                 | CODEC_PARAM_AUDIO_WIDGET_CAPS_AMP_OUT;

        case CODEC_PARAM_SUPP_PCM_SIZE_RATES:
            // Match the converter widget's advertised rates (see
            // CODEC_PARAM_SUPP_PCM_SIZE_RATES at the root node).
            return HDA_PCM_SIZE_16
                 | HDA_RATE_BIT(44100) | HDA_RATE_BIT(48000)
                 | HDA_RATE_BIT(88200) | HDA_RATE_BIT(96000);

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
static uint32_t sound_hda_codec_pin_output_cmd(uint32_t payload)
{
    switch (payload) {
        case CODEC_PARAM_AUDIO_WIDGET_CAPS:
            // Mono pin to match the output widget; see the output-widget
            // caps comment above.
            return CODEC_PARAM_AUDIO_WIDGET_CAPS_PIN
                 | CODEC_PARAM_AUDIO_WIDGET_CAPS_CONN_LIST;

        case CODEC_PARAM_PIN_CAPS:
            return CODEC_PARAM_PIN_CAPS_OUTPUT
                 | CODEC_PARAM_PIN_CAPS_PRESENSE_DETECT;

        case CODEC_PARAM_CONN_LIST_LEN:
            return 1;

        default:
            return 0;
    }
}

// NID = 4 — Beep Generator widget (HDA spec §7.2.3.8 / §7.3.4.6).
// Type code 7 (bits 23:20 of widget caps). Includes AMP_OUT so Linux
// exposes a "Beep Playback Volume" / "Beep Playback Switch" alsa
// control bound to the per-widget AMP_GAIN_MUTE verbs.
static uint32_t sound_hda_codec_beep_cmd(uint32_t payload)
{
    switch (payload) {
        case CODEC_PARAM_AUDIO_WIDGET_CAPS:
            // Type=7 (Beep Generator), AMP_OVR + AMP_OUT so the widget
            // exposes its own amp instead of inheriting the FG's. No
            // STEREO bit (beep is a single-channel tone). No CONN_LIST
            // — per spec §7.2.3.8 "this node is never listed on any
            // other node's connection list."
            return (7u << 20)
                 | CODEC_PARAM_AUDIO_WIDGET_CAPS_AMP_OVR
                 | CODEC_PARAM_AUDIO_WIDGET_CAPS_AMP_OUT;

        case CODEC_PARAM_OUTPUT_AMP_CAPS:
            // Same amp range as the main output — keeps the per-control
            // dB ↔ percent mapping consistent in alsamixer.
            return CODEC_PARAM_OUTPUT_AMP_CAPS_MUTE_CAP
                 | CODEC_PARAM_OUTPUT_AMP_CAPS_STEPSIZE
                 | CODEC_PARAM_OUTPUT_AMP_CAPS_NUMSTEPS
                 | CODEC_PARAM_OUTPUT_AMP_CAPS_OFFSET;

        default:
            return 0;
    }
}

// NID = 5 — Audio Input Converter widget (ADC, HDA spec §7.2.3.2).
// Type code 1 (bits 23:20 of widget caps). Conn list points at the mic
// pin (NID 6). Mono — see the comment on the output converter for why
// we deliberately don't advertise STEREO. Linux's HDA generic codec
// will create a "Capture Volume" / "Capture Switch" alsa control bound
// to the AMP_GAIN_MUTE verbs on this widget's input amp.
static uint32_t sound_hda_codec_input_cmd(uint32_t payload)
{
    switch (payload) {
        case CODEC_PARAM_AUDIO_WIDGET_CAPS:
            return CODEC_PARAM_AUDIO_WIDGET_CAPS_INPUT
                 | CODEC_PARAM_AUDIO_WIDGET_CAPS_FORMAT_OVR
                 | CODEC_PARAM_AUDIO_WIDGET_CAPS_AMP_OVR
                 | CODEC_PARAM_AUDIO_WIDGET_CAPS_AMP_IN
                 | CODEC_PARAM_AUDIO_WIDGET_CAPS_CONN_LIST;

        case CODEC_PARAM_SUPP_PCM_SIZE_RATES:
            return HDA_PCM_SIZE_16
                 | HDA_RATE_BIT(44100) | HDA_RATE_BIT(48000);

        case CODEC_PARAM_SUPP_STREAM_FMTS:
            return CODEC_PARAM_SUPP_STREAM_FMTS_PCM;

        case CODEC_PARAM_INPUT_AMP_CAPS:
            // Mirror the output-side amp caps so dB scaling matches what
            // alsamixer shows for the playback side. Mute capability is
            // important: Linux's "Capture Switch" defaults to muted on
            // some codecs, and without MUTE_CAP the toggle is silently
            // ignored.
            return CODEC_PARAM_OUTPUT_AMP_CAPS_MUTE_CAP
                 | CODEC_PARAM_OUTPUT_AMP_CAPS_STEPSIZE
                 | CODEC_PARAM_OUTPUT_AMP_CAPS_NUMSTEPS
                 | CODEC_PARAM_OUTPUT_AMP_CAPS_OFFSET;

        case CODEC_PARAM_CONN_LIST_LEN:
            return 1;   // single source — the mic pin

        default:
            return 0;
    }
}

// NID = 6 — Pin Complex configured as a Mic-In (HDA spec §7.2.3.3).
// Type code 4 (Pin Complex). Reports as a fixed internal mic — no
// jack-detect circuitry (the Java backend or host ALSA decides whether
// audio is "available"; presence is not modeled at the wire level).
static uint32_t sound_hda_codec_pin_input_cmd(uint32_t payload)
{
    switch (payload) {
        case CODEC_PARAM_AUDIO_WIDGET_CAPS:
            // Pin widgets advertise no STEREO (mono mic) and no
            // CONN_LIST (a Pin In is a leaf source — the input
            // converter pulls from it, not the other way around).
            return CODEC_PARAM_AUDIO_WIDGET_CAPS_PIN;

        case CODEC_PARAM_PIN_CAPS:
            // INPUT_CAPABLE is the bit that makes the codec generic
            // recognize this pin as a recording source. We advertise
            // VRef Hi-Z + Ground only; "Mic Boost" needs the proper
            // VRef levels and a separate amp on the pin which we don't
            // model. Linux is happy with the bare minimum.
            return CODEC_PARAM_PIN_CAPS_INPUT
                 | CODEC_PARAM_PIN_CAPS_VREF_CTRL_HIZ
                 | CODEC_PARAM_PIN_CAPS_VREF_CTRL_GROUND;

        default:
            return 0;
    }
}

#define NODE_ID_ROOT       0 // Root node
#define NODE_ID_FG_OUTPUT  1 // Audio Function Group
#define NODE_ID_OUTPUT     2 // Output converter widget (DAC)
#define NODE_ID_PIN_OUTPUT 3 // Pin output widget (Line-Out / Speaker)
#define NODE_ID_BEEP       4 // Beep Generator widget (§7.2.3.8)
#define NODE_ID_INPUT      5 // Input converter widget (ADC)
#define NODE_ID_PIN_INPUT  6 // Pin input widget (Mic-In)

static uint32_t sound_hda_codec_stream_cmd(sound_hda_stream_t *stream, uint32_t nid, uint32_t verb, uint32_t payload)
{
    uint32_t response = 0;

    switch (verb) {
        case VERB_GET_CONV_FMT:
            response = stream->fmt;
            break;
        case VERB_SET_CONV_FMT:
            stream->fmt = payload;
            break;
        case VERB_GET_AMP_GAIN_MUTE: {
            // §7.3.3.7 Fig. 62/63: payload bit 13 = left/right select;
            // response bit 7 mute, 6:0 gain. Output-amp-only codec.
            bool left = (payload & VERB_GET_AMP_GAIN_MUTE_LEFT) != 0;
            uint8_t mute = left ? stream->left_mute : stream->right_mute;
            uint8_t gain = left ? stream->left_gain : stream->right_gain;
            response = ((uint32_t)(mute & 1u) << 7) | (gain & 0x7Fu);
            break;
        }
        case VERB_SET_AMP_GAIN_MUTE: {
            // §7.3.3.7 Fig. 64: bit 15 Output, 14 Input, 13 Left, 12 Right,
            // 7 Mute, 6:0 Gain. Output-only codec; mono per NID 2 caps so
            // gain_q15 tracks the left channel.
            if (payload & VERB_SET_AMP_GAIN_MUTE_OUTPUT) {
                uint8_t mute = (payload & VERB_SET_AMP_GAIN_MUTE_MUTE) ? 1u : 0u;
                uint8_t gain = payload & VERB_SET_AMP_GAIN_MUTE_GAIN_MASK;
                if (payload & VERB_SET_AMP_GAIN_MUTE_LEFT) {
                    stream->left_mute = mute;
                    stream->left_gain = gain;
                }
                if (payload & VERB_SET_AMP_GAIN_MUTE_RIGHT) {
                    stream->right_mute = mute;
                    stream->right_gain = gain;
                }
                stream->gain_q15 = hda_gain_to_q15(stream->left_gain,
                                                   stream->left_mute);
            }
            break;
        }
        case VERB_GET_CONV_STREAM_CHAN:
            response = (stream->stream << 4) | stream->channel;
            break;
        case VERB_SET_CONV_STREAM_CHAN:
            stream->channel = payload & 0x0F;
            stream->stream = (payload >> 4) & 0x0F;
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

static void sound_hda_codec_cmd(sound_hda_dev_t *hda, uint32_t cmd)
{
    uint8_t cad = 0;
    uint8_t nid = 0;
    uint16_t verb = 0;
    uint16_t payload = 0;
    uint32_t response = 0;

    // 4.4.1.5 Other CORB Programming Notes
    // Zero is valid CORB command.
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

    // The single input stream lives at descriptor index 0 (NO_IN=1
    // means streams[0..0] are inputs). hda_output_stream uses the same
    // convention for the output side.
    sound_hda_stream_t *input_stream = &hda->streams[0];

    switch (verb) {
        case VERB_GET_PARAMETER:
            // We define NID count and start indices in CODEC_PARAM_SUB_NODE_COUNT
            switch (nid) {
            case NODE_ID_ROOT:
                response = sound_hda_codec_root_cmd(payload);
                break;
            case NODE_ID_FG_OUTPUT:
                response = sound_hda_codec_fg_output_cmd(payload);
                break;
            case NODE_ID_OUTPUT:
                response = sound_hda_codec_output_cmd(payload);
                break;
            case NODE_ID_PIN_OUTPUT:
                response = sound_hda_codec_pin_output_cmd(payload);
                break;
            case NODE_ID_BEEP:
                response = sound_hda_codec_beep_cmd(payload);
                break;
            case NODE_ID_INPUT:
                response = sound_hda_codec_input_cmd(payload);
                break;
            case NODE_ID_PIN_INPUT:
                response = sound_hda_codec_pin_input_cmd(payload);
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
            // §7.3.3.13: returns the pin widget control byte, per-NID.
            if (nid == NODE_ID_PIN_INPUT) response = hda->pin_ctrl_in;
            else                          response = hda->pin_ctrl_out;
            break;
        case VERB_SET_PIN_WIDGET_CTRL:
            // §7.3.3.13: payload bits 7:0 are the new pin control byte.
            // Linux's pin power-up sets the widget's enable bit before
            // recording (IN_ENABLE for mic, OUT_ENABLE for speaker)
            // and reads back to confirm. Track each pin separately.
            if (nid == NODE_ID_PIN_INPUT) hda->pin_ctrl_in  = payload & 0xFFu;
            else                          hda->pin_ctrl_out = payload & 0xFFu;
            break;
        case VERB_GET_PIN_SENSE:
            response = VERB_GET_PIN_SENSE_PRESENSE_PLUGGED;
            break;
        case VERB_GET_BEEP_GENERATION:
            // §7.3.3.31: response bits 7:0 are the current divider; 0 = off.
            response = hda->beep_divider;
            break;
        case VERB_SET_BEEP_GENERATION:
            // §7.3.3.31: payload bits 7:0 are divider. 0 disables;
            // non-zero starts a tone at 48000 / (4 * divider) Hz.
            sound_hda_beep_set(hda, payload & 0xFFu);
            break;
        case VERB_GET_AMP_GAIN_MUTE:
            // Beep widget (NID 4) has its own mono amp; converter widgets
            // (NID 2 output, NID 5 input) route to their per-stream
            // dispatcher.
            if (nid == NODE_ID_BEEP) {
                response = ((uint32_t)(hda->beep_mute & 1u) << 7)
                         | (hda->beep_gain & 0x7Fu);
            } else if (nid == NODE_ID_OUTPUT) {
                response = sound_hda_codec_stream_cmd(hda_output_stream(hda),
                                                      nid, verb, payload);
            } else if (nid == NODE_ID_INPUT) {
                response = sound_hda_codec_stream_cmd(input_stream,
                                                      nid, verb, payload);
            }
            break;
        case VERB_SET_AMP_GAIN_MUTE:
            if (nid == NODE_ID_BEEP) {
                // Mono — spec §7.3.3.7: "if the widget only supports a
                // single channel, [right] bits are ignored." Either
                // LEFT or RIGHT in the payload writes the single value.
                if (payload & VERB_SET_AMP_GAIN_MUTE_OUTPUT) {
                    hda->beep_mute = (payload & VERB_SET_AMP_GAIN_MUTE_MUTE) ? 1u : 0u;
                    hda->beep_gain = payload & VERB_SET_AMP_GAIN_MUTE_GAIN_MASK;
                }
            } else if (nid == NODE_ID_OUTPUT) {
                sound_hda_codec_stream_cmd(hda_output_stream(hda),
                                           nid, verb, payload);
            } else if (nid == NODE_ID_INPUT) {
                sound_hda_codec_stream_cmd(input_stream, nid, verb, payload);
            }
            break;
        case VERB_GET_CONN_LIST_ENTRY:
            // NID 3 (Pin Out) ← NID 2 (DAC).
            // NID 5 (ADC)     ← NID 6 (Pin In).
            // Other NIDs have no conn list, return 0.
            if (nid == NODE_ID_INPUT)        response = NODE_ID_PIN_INPUT;
            else if (nid == NODE_ID_PIN_OUTPUT) response = NODE_ID_OUTPUT;
            else                             response = 0;
            break;
        case VERB_GET_CONFIG_DEFAULT:
            // Per-pin Configuration Default. NID 3 = Line-Out (orange,
            // jack); NID 6 = Mic-In (pink, fixed internal mic).
            if (nid == NODE_ID_PIN_INPUT) {
                response = VERB_GET_CONFIG_DEFAULT_CONNECTIVITY_FIXED
                         | VERB_GET_CONFIG_DEFAULT_DEVICE_MIC_IN
                         | VERB_GET_CONFIG_DEFAULT_COLOR_PINK
                         // Different default-association from the output
                         // pin so the codec generic doesn't try to group
                         // them as one multichannel jack pair.
                         | (2u << 4);   // Default Association = 2
            } else {
                response = VERB_GET_CONFIG_DEFAULT_CONNECTIVITY_JACK
                         | VERB_GET_CONFIG_DEFAULT_DEVICE_LINE_OUT
                         | VERB_GET_CONFIG_DEFAULT_ASSOCIATION_DEFAULT
                         | VERB_GET_CONFIG_DEFAULT_COLOR_ORANGE;
            }
            break;
        case VERB_FUNCTION_RESET:
            // §7.3.3.33: reset FG widgets to power-on values (Configuration
            // Defaults excluded). §7.3.3.7 mute defaults to 1.
            if (nid == NODE_ID_FG_OUTPUT || nid == NODE_ID_OUTPUT) {
                sound_hda_stream_t *s = hda_output_stream(hda);
                s->fmt        = 0;
                s->channel    = 0;
                s->stream     = 0;
                s->left_gain  = HDA_AMP_OFFSET;
                s->right_gain = HDA_AMP_OFFSET;
                s->left_mute  = 1;
                s->right_mute = 1;
                s->gain_q15   = 0;
                hda->pin_ctrl_out = VERB_GET_PIN_WIDGET_CTRL_OUT_ENABLE;
            }
            if (nid == NODE_ID_FG_OUTPUT || nid == NODE_ID_INPUT) {
                input_stream->fmt        = 0;
                input_stream->channel    = 0;
                input_stream->stream     = 0;
                input_stream->left_gain  = HDA_AMP_OFFSET;
                input_stream->right_gain = HDA_AMP_OFFSET;
                input_stream->left_mute  = 1;
                input_stream->right_mute = 1;
                input_stream->gain_q15   = 0;
                hda->pin_ctrl_in = 0;
            }
            response = 0;
            break;
        default:
            switch (nid) {
            case NODE_ID_OUTPUT:
                response = sound_hda_codec_stream_cmd(hda_output_stream(hda), nid, verb, payload);
                break;
            case NODE_ID_INPUT:
                response = sound_hda_codec_stream_cmd(input_stream, nid, verb, payload);
                break;
            }
            break;
    }

    sound_hda_write_rirb(hda, cad, response);
}

// Drain the stream once: read current fmt/BDL, pace PCM to the backend
// until the guest clears running (or an unrecoverable condition bails).
// Separate from the worker-lifetime loop below so early-bail paths can
// just return without touching worker_alive.
//
// Operates on a single stream descriptor — generic across input/output/
// bidir streams. Today only output streams have a write_fn backend wired
// in init, so the drain on input streams burns its pacing loop and
// silently advances LPIB; the guest sees an idle but functional stream.
static void sound_hda_stream_drain(sound_hda_stream_t *stream)
{
    sound_hda_dev_t *hda = stream->hda;

    // Pace the worker to the stream's configured bytes-per-second.
    // Without pacing, non-blocking backends (ring buffers, null sinks)
    // let this loop blast through the BDL as fast as the CPU allows:
    // LPIB advances instantly, the guest HDA driver sees its DMA
    // "consume" data faster than it can refill, and aplay trips ALSA's
    // position-consistency assertion ("pcm_plugin.c Assertion
    // status->appl_ptr == *pcm->appl.ptr failed"). Blocking backends
    // (ALSA's snd_pcm_writei) dodge this accidentally because writei
    // blocks when the host buffer fills.
    //
    // The rate is derived from stream->fmt per HDA spec 7.3.3.10:
    //   bit  14      : base rate        (0=48000, 1=44100)
    //   bits 13:11   : rate multiplier  (N+1: 1×, 2×, 3×, 4×)
    //   bits 10:8    : rate divisor     (N+1: /1 .. /8)
    //   bits 6:4     : bits/sample      (0=8, 1=16, 2=20, 3=24, 4=32)
    //   bits 3:0     : channels minus 1 (0=1ch, 1=2ch, …)
    //
    // A previous iteration hardcoded 192 kHz mono then later 48 kHz
    // mono — both broke when the guest driver chose a different stream
    // format (Linux HDA likes to configure stereo streams even for mono
    // content, doubling the true byte rate). Deriving from fmt is the
    // only way to be robust across guest driver choices.
    uint16_t fmt              = stream->fmt;
    uint32_t channels         = (fmt & 0xF) + 1;
    uint32_t bytes_per_sample = hda_fmt_container_bytes[(fmt >> 4) & 7];
    uint32_t mult_code        = (fmt >> 11) & 7;
    uint32_t div              = ((fmt >> 8) & 7) + 1;
    uint32_t base_hz          = (fmt & (1u << 14)) ? 44100 : 48000;
    // Reject formats the spec marks reserved or that we don't render:
    //   - bit 15 TYPE=1 (Non-PCM): codec advertises PCM only
    //   - BITS code 5..7: container size 0 in the lookup table
    //   - MULT code 4..7: spec 3.3.41 / 3.7.1 reserved
    // Folded into the same bail-and-let-guest-retry path as fmt=0 below.
    bool fmt_valid = (fmt & (1u << 15)) == 0
                  && bytes_per_sample != 0
                  && mult_code <= 3;
    uint64_t sample_rate_hz = (uint64_t)base_hz * (mult_code + 1) / div;
    uint32_t bytes_per_frame = channels * bytes_per_sample;
    uint64_t SAMPLE_RATE_BYTES_PER_SEC = sample_rate_hz * bytes_per_frame;
    if (!fmt_valid || SAMPLE_RATE_BYTES_PER_SEC == 0) {
        // Guest wrote run=1 before configuring the format, or programmed
        // an encoding the spec marks reserved. Bail like the NULL-dma
        // case — driver will retry properly.
        atomic_store_uint32_relax(&stream->running, 0);
        return;
    }
    // Tell the backend what rate to expect. Backends with a fixed host
    // PCM (ALSA) reopen at the new rate; others (resampling sinks,
    // capture-to-WAV harnesses) typically no-op. Only meaningful for
    // output streams — the input drain doesn't talk to the playback
    // PCM. Safe to skip for input streams since alsa's set_rate
    // touches the playback path only, but we route by stream identity
    // anyway to keep the contract clean: set_rate is paired with
    // write(), not read().
    if (stream == hda_output_stream(hda)
            && hda->subsystem.set_rate != NULL) {
        hda->subsystem.set_rate(&hda->subsystem, (uint32_t)sample_rate_hz);
    }
    // §3.3.38: "CBL must represent an integer number of samples." Not
    // enforced by us (we don't know FMT at CBL write time, and rejecting
    // would silently break the stream), but log it so guest bugs are
    // visible. LPIB will wrap mid-sample and the guest's hw_ptr math
    // will drift; that's the guest's fault.
    if (stream->bdl_len % bytes_per_frame != 0) {
        rvvm_debug("sound-hda: SDnCBL=%u not a multiple of frame size"
                   " %u (channels=%u, bytes/sample=%u) — spec §3.3.38",
                   stream->bdl_len, bytes_per_frame,
                   channels, bytes_per_sample);
    }
    uint64_t       paced_start_ns  = 0;
    uint64_t       paced_bytes_out = 0;

    uint32_t total = stream->bdl_lvi + 1;
    // §3.6.3: 16-byte BDL entries (addr8 + len4 + flags4).
    uint64_t bdl_bytes = (uint64_t)total * 16;
    uint64_t *dma = pci_get_dma_ptr(hda->pci_func, stream->bdl_lo, bdl_bytes);

    // If the guest set up the stream control register without a valid BDL
    // (bdl_lo == 0 or invalid), pci_get_dma_ptr returns NULL. Bail out
    // cleanly — the guest's ALSA driver will eventually retry with a
    // proper BDL when userspace opens another PCM handle.
    if (dma == NULL) {
        atomic_store_uint32_relax(&stream->running, 0);
        return;
    }

    while (atomic_load_uint32_relax(&stream->running)) {
        for (uint32_t i = 0; i < total; ++i) {
            uint64_t *bdle = &dma[i * 2];
            uint64_t addr = bdle[0];
            uint32_t len  = bdle[1] & 0xFFFFFFFF;
            uint8_t  ioc  = bdle[1] >> 32 & 1;

            // §3.6.3: "The buffer length must be at least one Word."
            // A zero-length entry is a guest spec violation. Skip the
            // backend write (avoids degenerate snd_pcm_writei(buf, 0)
            // calls) but still latch IOC and advance LPIB by 0 so the
            // guest's bookkeeping isn't disturbed.
            if (len == 0) {
                rvvm_debug("sound-hda: zero-length BDL entry at idx %u"
                           " — spec §3.6.3", i);
                if (!atomic_load_uint32_relax(&stream->running)) return;
                if (ioc) {
                    spin_lock(&hda->lock);
                    stream->status |= 0x04;
                    uint8_t  ioce = stream->ioce;
                    uint32_t ic   = hda->intr_ctrl;
                    spin_unlock(&hda->lock);
                    if (ioce && (ic & (1u << 31))
                             && (ic & (1u << stream->intsts_bit))) {
                        pci_send_irq(hda->pci_func, 0);
                    }
                }
                continue;
            }

            // Dispatch PCM to the configured host-side backend. If no backend
            // was installed at init time (neither a compile-time USE_ALSA nor
            // a caller-supplied write_fn via sound_hda_init_ex), the HDA
            // device enumerates on the PCI bus but this path is a no-op —
            // the guest sees a working device and the LPIB counter advances
            // so its driver doesn't stall, but PCM data is silently dropped.
            //
            // Backend contract: always receives MONO 16-bit LE at the
            // stream's configured rate. If the guest driver configured a
            // multi-channel stream (Linux's HDA code likes stereo even
            // when the codec widget advertises mono capability), we
            // downmix here by averaging channels. Keeps backends simple
            // (they don't need to know channel count) and the pacing
            // math below stays 1:1 with the bytes they see.
            if (hda->subsystem.write != NULL) {
                void *pcm = pci_get_dma_ptr(hda->pci_func, addr, len);
                if (pcm == NULL) {
                    // Bad guest BDL entry (zero addr, unmapped region, or
                    // zero len). Don't feed NULL to the backend — ALSA's
                    // snd_pcm_writei(NULL, n) is UB and ringbuf writes
                    // memcpy from NULL. Pacing still advances below so
                    // LPIB keeps moving and the guest can recover.
                } else if (channels == 1) {
                    // Mono fast path — but we still need to apply the
                    // codec amp's gain/mute (HDA spec §7.3.3.7).
                    // Q15 multiply per sample; unity (32768) skips the
                    // copy and writes the guest buffer directly.
                    int32_t q15 = stream->gain_q15;
                    if (q15 == 32768) {
                        hda->subsystem.write(&hda->subsystem, pcm, len);
                    } else {
                        size_t frames = len / 2;            // 16-bit mono
                        int16_t *src = (int16_t*)pcm;
                        int16_t  scaled[4096];
                        size_t emitted = 0;
                        while (emitted < frames) {
                            size_t chunk = frames - emitted;
                            if (chunk > 4096) chunk = 4096;
                            for (size_t f = 0; f < chunk; f++) {
                                scaled[f] = (int16_t)(((int32_t)src[emitted + f] * q15) >> 15);
                            }
                            hda->subsystem.write(&hda->subsystem, scaled, chunk * 2);
                            emitted += chunk;
                        }
                    }
                } else if (bytes_per_sample == 2) {
                    // Common case: 16-bit multi-channel → 16-bit mono.
                    // Stack-allocate — BDL entries are small (256-4096 B).
                    // Apply the codec amp gain during the channel
                    // averaging — saves a second pass over the buffer.
                    int32_t q15           = stream->gain_q15;
                    size_t frame_bytes_in = (size_t)bytes_per_frame;
                    size_t frames         = len / frame_bytes_in;
                    int16_t *src = (int16_t*)pcm;
                    int16_t  mono_buf[4096];
                    size_t bytes_emitted = 0;
                    while (bytes_emitted < frames * 2) {
                        size_t chunk_frames = frames - (bytes_emitted / 2);
                        if (chunk_frames > 4096) chunk_frames = 4096;
                        for (size_t f = 0; f < chunk_frames; f++) {
                            int32_t sum = 0;
                            size_t base = (bytes_emitted / 2 + f) * channels;
                            for (uint32_t c = 0; c < channels; c++) {
                                sum += src[base + c];
                            }
                            int32_t mono = sum / (int32_t)channels;
                            mono_buf[f] = (int16_t)((mono * q15) >> 15);
                        }
                        hda->subsystem.write(&hda->subsystem, mono_buf, chunk_frames * 2);
                        bytes_emitted += chunk_frames * 2;
                    }
                } else {
                    // Rare: non-16-bit multi-channel. Pass raw; backends
                    // that care can parse stream->fmt from the caller.
                    hda->subsystem.write(&hda->subsystem, pcm, len);
                }
            } else {
                UNUSED(addr);
            }

            // Shutdown check between backend write and pacing / IRQ
            // dispatch. The outer `while (running)` only gates the
            // BDL pass; once started, a full pass is up to (lvi+1)
            // entries × ~43 ms of pacing — several seconds the
            // device might already be torn down behind us. Bailing
            // here ensures no pci_send_irq / pci_get_dma_ptr call
            // outlives sound_hda_remove(), which is how #208 got
            // a freed pci_func under the IRQ dispatch.
            if (!atomic_load_uint32_relax(&stream->running))
                return;

            // Wall-clock pacing. Compute the ideal elapsed time for the
            // bytes we've emitted so far and sleep the difference.
            paced_bytes_out += len;
            uint64_t now_ns = rvtimer_clocksource(1000000000ULL);
            if (paced_start_ns == 0) {
                paced_start_ns = now_ns;
            } else {
                uint64_t expected_ns = paced_bytes_out * 1000000000ULL
                                     / SAMPLE_RATE_BYTES_PER_SEC;
                uint64_t elapsed_ns  = now_ns - paced_start_ns;
                if (expected_ns > elapsed_ns) {
                    sleep_ns(expected_ns - elapsed_ns);
                } else if (elapsed_ns > expected_ns + 100000000ULL) {
                    // Fell more than 100 ms behind — rebase so we don't
                    // try to "catch up" by blasting. Happens on machine
                    // resume from pause, or very long Java-side GCs.
                    paced_start_ns  = now_ns;
                    paced_bytes_out = 0;
                }
            }

            stream->lpib += len;
            // If stream longer than BDL length, reset LPIB.
            if (stream->lpib >= stream->bdl_len)
                stream->lpib = 0;
            // When LPIB goes over BDL length, we are done.
            if (stream->bdl_len > 0 && stream->lpib > stream->bdl_len)
                atomic_store_uint32_relax(&stream->running, 0);
            if (ioc) {
                // §3.3.38: latch BCIS unconditionally (RW1C-cleared by
                // guest); IRQ gate is separate. Lock guards against the
                // MMIO-side RW1C clear racing the latch.
                spin_lock(&hda->lock);
                stream->status |= 0x04;
                uint8_t  ioce = stream->ioce;
                uint32_t ic   = hda->intr_ctrl;
                spin_unlock(&hda->lock);
                // §3.3.14 / §3.3.36: PCI IRQ requires IOCE & SIE & GIE.
                bool gie = (ic & (1u << 31)) != 0;
                bool sie = (ic & (1u << stream->intsts_bit)) != 0;
                if (ioce && gie && sie) {
                    pci_send_irq(hda->pci_func, 0);
                }
            }
        }
    }
}

// Input-direction drain. Mirror of sound_hda_stream_drain: same BDL
// walk, same fmt-derived pacing, but data flows from subsystem.read
// into guest memory via pci_get_dma_ptr. The backend may return short;
// we pad the rest of the BDL entry with silence so the pacing rate
// stays bit-exact regardless of mic availability.
static void sound_hda_stream_drain_input(sound_hda_stream_t *stream)
{
    sound_hda_dev_t *hda = stream->hda;

    uint16_t fmt              = stream->fmt;
    uint32_t channels         = (fmt & 0xF) + 1;
    uint32_t bytes_per_sample = hda_fmt_container_bytes[(fmt >> 4) & 7];
    uint32_t mult_code        = (fmt >> 11) & 7;
    uint32_t div              = ((fmt >> 8) & 7) + 1;
    uint32_t base_hz          = (fmt & (1u << 14)) ? 44100 : 48000;
    bool fmt_valid = (fmt & (1u << 15)) == 0
                  && bytes_per_sample != 0
                  && mult_code <= 3;
    uint64_t sample_rate_hz = (uint64_t)base_hz * (mult_code + 1) / div;
    uint32_t bytes_per_frame = channels * bytes_per_sample;
    uint64_t SAMPLE_RATE_BYTES_PER_SEC = sample_rate_hz * bytes_per_frame;
    if (!fmt_valid || SAMPLE_RATE_BYTES_PER_SEC == 0) {
        atomic_store_uint32_relax(&stream->running, 0);
        return;
    }

    uint64_t paced_start_ns  = 0;
    uint64_t paced_bytes_out = 0;

    uint32_t total = stream->bdl_lvi + 1;
    uint64_t bdl_bytes = (uint64_t)total * 16;
    uint64_t *dma = pci_get_dma_ptr(hda->pci_func, stream->bdl_lo, bdl_bytes);
    if (dma == NULL) {
        atomic_store_uint32_relax(&stream->running, 0);
        return;
    }

    while (atomic_load_uint32_relax(&stream->running)) {
        for (uint32_t i = 0; i < total; ++i) {
            uint64_t *bdle = &dma[i * 2];
            uint64_t addr = bdle[0];
            uint32_t len  = bdle[1] & 0xFFFFFFFF;
            uint8_t  ioc  = bdle[1] >> 32 & 1;

            if (len == 0) {
                rvvm_debug("sound-hda: zero-length BDL entry at idx %u"
                           " (input) — spec §3.6.3", i);
                if (!atomic_load_uint32_relax(&stream->running)) return;
                if (ioc) {
                    spin_lock(&hda->lock);
                    stream->status |= 0x04;
                    uint8_t  ioce = stream->ioce;
                    uint32_t ic   = hda->intr_ctrl;
                    spin_unlock(&hda->lock);
                    if (ioce && (ic & (1u << 31))
                             && (ic & (1u << stream->intsts_bit))) {
                        pci_send_irq(hda->pci_func, 0);
                    }
                }
                continue;
            }

            // Fill the guest buffer for this BDL entry. Pull from the
            // backend; pad the remainder (or the whole thing if no
            // backend) with silence so the guest sees a steady
            // sample-rate stream regardless of host-side gaps.
            void *dst = pci_get_dma_ptr(hda->pci_func, addr, len);
            if (dst != NULL) {
                size_t got = 0;
                if (hda->subsystem.read != NULL) {
                    got = hda->subsystem.read(&hda->subsystem, dst, len);
                    if (got > len) got = len;
                }
                if (got < len) {
                    memset((uint8_t*)dst + got, 0, (size_t)len - got);
                }
            }

            // Same shutdown check as the output path — a backend abort
            // or remove() can race the BDL walk; bail before any further
            // pci_send_irq / pci_get_dma_ptr calls outlive the device.
            if (!atomic_load_uint32_relax(&stream->running))
                return;

            // Wall-clock pacing — same shape as the output drain.
            paced_bytes_out += len;
            uint64_t now_ns = rvtimer_clocksource(1000000000ULL);
            if (paced_start_ns == 0) {
                paced_start_ns = now_ns;
            } else {
                uint64_t expected_ns = paced_bytes_out * 1000000000ULL
                                     / SAMPLE_RATE_BYTES_PER_SEC;
                uint64_t elapsed_ns  = now_ns - paced_start_ns;
                if (expected_ns > elapsed_ns) {
                    sleep_ns(expected_ns - elapsed_ns);
                } else if (elapsed_ns > expected_ns + 100000000ULL) {
                    paced_start_ns  = now_ns;
                    paced_bytes_out = 0;
                }
            }

            stream->lpib += len;
            if (stream->lpib >= stream->bdl_len)
                stream->lpib = 0;
            if (stream->bdl_len > 0 && stream->lpib > stream->bdl_len)
                atomic_store_uint32_relax(&stream->running, 0);

            // §3.3.36 BCIS for input: set after the last byte of an IOC
            // descriptor has been *removed from the FIFO* (i.e.,
            // committed to memory). For us, that's after the memset/
            // memcpy above, so latch it here.
            if (ioc) {
                spin_lock(&hda->lock);
                stream->status |= 0x04;
                uint8_t  ioce = stream->ioce;
                uint32_t ic   = hda->intr_ctrl;
                spin_unlock(&hda->lock);
                bool gie = (ic & (1u << 31)) != 0;
                bool sie = (ic & (1u << stream->intsts_bit)) != 0;
                if (ioce && gie && sie) {
                    pci_send_irq(hda->pci_func, 0);
                }
            }
        }
    }
}

// Beep generator worker (HDA spec §7.2.3.8 / §7.3.3.31). Runs whenever
// hda->beep_running is set; generates a square wave at 48000/(4*divider)
// Hz mixed at the beep widget's amp gain, fed to the same backend the
// stream worker uses. Same lockless single-worker pattern as the stream
// worker — beep_worker_alive CAS gate, post-exit re-check for missed
// wakeups.
static void *sound_hda_beep_worker(void *arg)
{
    sound_hda_dev_t *hda = arg;
    // Square-wave state: alternates sign every half-period.
    int16_t  sample_value = 0;
    uint32_t half_period_remaining = 0;
    uint32_t cur_divider = 0;

    uint64_t paced_start_ns  = 0;
    uint64_t paced_bytes_out = 0;
    const uint32_t SAMPLE_RATE_HZ = 48000;
    const uint32_t BYTES_PER_SAMPLE = 2;            // 16-bit mono

    for (;;) {
        while (atomic_load_uint32_relax(&hda->beep_running)) {
            uint8_t divider = hda->beep_divider;
            if (divider == 0) {
                atomic_store_uint32_relax(&hda->beep_running, 0);
                break;
            }
            // Re-derive the half-period if divider changed mid-tone.
            if (divider != cur_divider) {
                cur_divider = divider;
                // freq = 48000 / (4 * divider), period_samples = 4*divider
                half_period_remaining = 2u * (uint32_t)divider;
                sample_value = 0;
            }

            // Generate one ~10 ms chunk per iteration so divider /
            // gain / mute changes propagate quickly.
            int16_t buf[480];   // 480 samples = 10 ms @ 48 kHz
            uint8_t mute = hda->beep_mute;
            uint8_t gain = hda->beep_gain;
            int32_t q15  = hda_gain_to_q15(gain, mute);
            for (size_t i = 0; i < 480; ++i) {
                if (half_period_remaining == 0) {
                    // Toggle. Use ~50% of full-scale so the tone is
                    // present but not deafening before the per-widget
                    // amp scales it.
                    sample_value = (sample_value > 0) ? -16384 : 16384;
                    half_period_remaining = 2u * cur_divider;
                }
                buf[i] = (int16_t)(((int32_t)sample_value * q15) >> 15);
                half_period_remaining--;
            }

            if (hda->subsystem.write != NULL) {
                hda->subsystem.write(&hda->subsystem, buf, sizeof(buf));
            }

            // Wall-clock pacing — same shape as sound_hda_stream_drain.
            paced_bytes_out += sizeof(buf);
            uint64_t now_ns = rvtimer_clocksource(1000000000ULL);
            if (paced_start_ns == 0) {
                paced_start_ns = now_ns;
            } else {
                uint64_t expected_ns = paced_bytes_out * 1000000000ULL
                                     / ((uint64_t)SAMPLE_RATE_HZ * BYTES_PER_SAMPLE);
                uint64_t elapsed_ns  = now_ns - paced_start_ns;
                if (expected_ns > elapsed_ns) {
                    sleep_ns(expected_ns - elapsed_ns);
                } else if (elapsed_ns > expected_ns + 100000000ULL) {
                    paced_start_ns  = now_ns;
                    paced_bytes_out = 0;
                }
            }
        }

        atomic_store_uint32_relax(&hda->beep_worker_alive, 0);
        if (!atomic_load_uint32_relax(&hda->beep_running))
            return NULL;
        if (!atomic_cas_uint32(&hda->beep_worker_alive, 0, 1))
            return NULL;
        // Re-claimed; loop continues with the new beep state.
    }
}

// Beep enable/disable — driven by VERB_SET_BEEP_GENERATION. Divider
// 0 stops; non-zero starts (or updates the frequency of) the tone.
// Single tone at a time across the whole codec; beep is intrinsically
// global per spec §7.2.3.8 ("the codec generates the beep tone on all
// Pin Complexes that are currently configured as outputs").
static void sound_hda_beep_set(sound_hda_dev_t *hda, uint8_t divider)
{
    hda->beep_divider = divider;
    if (divider == 0) {
        atomic_store_uint32_relax(&hda->beep_running, 0);
        return;
    }
    atomic_store_uint32_relax(&hda->beep_running, 1);
    if (atomic_cas_uint32(&hda->beep_worker_alive, 0, 1)) {
        thread_create_task(sound_hda_beep_worker, hda);
    }
}

static void *sound_hda_stream_worker(void *arg)
{
    sound_hda_stream_t *stream = arg;

    // Worker lifetime is published via worker_alive. Loop handles a narrow
    // missed-wakeup window: guest writes run=1 after we read running=0 but
    // before we clear worker_alive — its spawn CAS sees worker_alive=1
    // and skips, leaving no worker for a running stream. We catch that by
    // re-reading running after the clear, and re-claim the slot to drain
    // again. If a concurrent spawn already won the CAS, they own the next
    // drain — we exit. Either way, exactly one worker runs at a time per
    // stream descriptor.
    for (;;) {
        if (stream->dir == HDA_STREAM_DIR_INPUT) {
            sound_hda_stream_drain_input(stream);
        } else {
            sound_hda_stream_drain(stream);
        }

        atomic_store_uint32_relax(&stream->worker_alive, 0);

        if (!atomic_load_uint32_relax(&stream->running))
            return NULL;
        if (!atomic_cas_uint32(&stream->worker_alive, 0, 1))
            return NULL;
        // Re-claimed the slot; drain again with freshly-read stream state.
    }
}

// SDnCTL action — invoked by the SD register dispatch on every guest
// write to offset 0x00 of any stream descriptor. Same body for input,
// output, and bidir streams; only the worker spawn is output-specific
// today (input-stream worker would consume from a host source instead).
//
// SDnCTL action (§3.3.35). SRST 0→1 resets LPIB/status; BDL/FMT/CBL/LVI
// are left alone because Linux rewrites them after observing SRST=0.
static void sound_hda_stream_ctl_action(sound_hda_dev_t *hda,
                                        sound_hda_stream_t *stream,
                                        uint32_t cmd)
{
    uint8_t srst    = (cmd >> 0)  & 1u;
    uint8_t run     = (cmd >> 1)  & 1u;
    uint8_t ioce    = (cmd >> 2)  & 1u;
    uint8_t feie    = (cmd >> 3)  & 1u;
    uint8_t deie    = (cmd >> 4)  & 1u;
    uint8_t stripe  = (cmd >> 16) & 0x3u;
    uint8_t tp      = (cmd >> 18) & 1u;
    uint8_t strm    = (cmd >> 20) & 0xFu;

    if (srst && !stream->srst) {
        // SRST 0→1: spec requires RUN=0 first; force-stop on guest
        // violation so the worker doesn't spin on zeroed BDL pointers.
        atomic_store_uint32_relax(&stream->running, 0);
        run            = 0;
        stream->lpib   = 0;
        stream->status = 0;
    }
    stream->srst     = srst;
    stream->ioce     = ioce;
    stream->feie     = feie;
    stream->deie     = deie;
    stream->stripe   = stripe;
    stream->tp       = tp;
    stream->ctl_strm = strm;

    // Publish RUN regardless of direction so MMIO reads round-trip it.
    // Both input and output streams spawn a worker — input pulls from
    // subsystem.read into guest memory, output pushes from guest memory
    // through subsystem.write. Bidir streams stay inert (we never set
    // NO_BSS != 0 today).
    atomic_store_uint32_relax(&stream->running, run ? 1u : 0u);
    if (run && (stream->dir == HDA_STREAM_DIR_OUTPUT
             || stream->dir == HDA_STREAM_DIR_INPUT)) {
        // running=1 must be published before the CAS so a worker
        // exiting concurrently sees it on its post-clear re-check.
        if (atomic_cas_uint32(&stream->worker_alive, 0, 1)) {
            thread_create_task(sound_hda_stream_worker, stream);
        }
    }
    UNUSED(hda);
}

// GCTL CRST=0 → controller reset (HDA spec §3.3.7). All state machines,
// FIFOs, and MMIO registers clear except WAKEEN, STATESTS, and CRST
// itself. Software is responsible for clearing CORB/RIRB RUN bits and
// stream RUN bits before asserting CRST=0 — but a guest that ignores
// that requirement (or a fresh probe after a previous session left
// streams running) shouldn't leave us with stale state. Halt every
// stream worker via the running flag, zero ring-buffer pointers, and
// reset per-stream MMIO state. UNSOL bit (8) follows CRST.
static void sound_hda_controller_reset(sound_hda_dev_t *hda)
{
    for (size_t i = 0; i < HDA_STREAMS_TOTAL; ++i) {
        sound_hda_stream_t *s = &hda->streams[i];
        atomic_store_uint32_relax(&s->running, 0);
        s->lpib     = 0;
        s->status   = 0;
        s->srst     = 0;
        s->ioce     = 0;
        s->feie     = 0;
        s->deie     = 0;
        s->stripe   = 0;
        s->tp       = 0;
        s->ctl_strm = 0;
        s->bdl_lo   = 0;
        s->bdl_hi   = 0;
        s->bdl_len  = 0;
        s->bdl_lvi  = 0;
        s->fmt      = 0;
    }
    hda->corb_rp = 0;
    hda->corb_wp = 0;
    hda->rirb_rp = 0;
    hda->rirb_wp = 0;
    hda->intr_ctrl = 0;
    // WAKEEN, STATESTS, power_state, codec subsystem registers are
    // explicitly preserved per spec ("only cleared on power-on reset").
    // We don't model wake bits today, so the preservation is moot.
    //
    // We don't wait for workers to drain — they bail on their next
    // running check (within one BDL entry), and the guest's reset poll
    // loop reads CRST=0 immediately. A worker still finishing a backend
    // write while we return is safe because the BDL pointers are now
    // zeroed: any new dma fetch will fail and the worker exits.
}

// CORB consumer (§3.3.20 / §4.4.1.4). Treat each CORBWP write as the
// dispatch trigger, walking RP→WP. We don't model CORBCTL.CORBRUN gating.
static void sound_hda_corb_wp_write(sound_hda_dev_t *hda, uint32_t v)
{
    hda->corb_wp = v & 0xFFu;
    if (hda->corb_size == 0) return;
    uint32_t entries = hda->corb_size / 4;
    uint32_t *corb = pci_get_dma_ptr(hda->pci_func, hda->corb_lo,
                                     (size_t)entries * 4);
    if (corb == NULL) return;
    while (hda->corb_rp != hda->corb_wp) {
        hda->corb_rp = (hda->corb_rp + 1) % entries;
        sound_hda_codec_cmd(hda, corb[hda->corb_rp]);
    }
}

static bool sound_hda_mmio_write(rvvm_mmio_dev_t* dev, void* data, size_t offset, uint8_t size)
{
    sound_hda_dev_t *hda = dev->data;
    spin_lock(&hda->lock);

    bool ok = gr_dispatch_write(hda, offset, data, size);
    if (!ok) {
        uint16_t sub_off;
        sound_hda_stream_t *s = hda_resolve_stream(hda, offset, &sub_off);
        if (s != NULL) ok = sd_dispatch_write(hda, s, sub_off, data, size);
    }

    spin_unlock(&hda->lock);
    return ok;
}

/*
 * Bridge struct stored in sound_subsystem_t.sound_data when a caller-supplied
 * backend is installed via sound_hda_init_ex. Adapts the public two-argument
 * callback to the subsystem's three-argument shape. Freed... never. The HDA
 * device itself is leaked on machine destruction today (see sound_hda_remove),
 * so this follows the same lifecycle.
 */
typedef struct {
    sound_hda_backend_write_fn user_fn;
    sound_hda_backend_abort_fn abort_fn;
    void                       *user_data;
} sound_hda_backend_bridge_t;

static void sound_hda_backend_bridge_write(sound_subsystem_t *sub, void *data, size_t size)
{
    sound_hda_backend_bridge_t *bridge = (sound_hda_backend_bridge_t *)sub->sound_data;
    if (bridge != NULL && bridge->user_fn != NULL) {
        bridge->user_fn(bridge->user_data, data, size);
    }
}

static void sound_hda_backend_bridge_abort(sound_subsystem_t *sub)
{
    sound_hda_backend_bridge_t *bridge = (sound_hda_backend_bridge_t *)sub->sound_data;
    if (bridge != NULL && bridge->abort_fn != NULL) {
        bridge->abort_fn(bridge->user_data);
    }
}

PUBLIC pci_dev_t *sound_hda_init_ex(pci_bus_t *pci_bus,
                                    sound_hda_backend_write_fn write_fn,
                                    sound_hda_backend_abort_fn abort_fn,
                                    void *user_data)
{
    sound_hda_dev_t *sound_hda = safe_new_obj(sound_hda_dev_t);

    // §3.3.9 STATESTS: bit 0 = 1 announces a codec on SDIN[0]. Sticky
    // until power-on reset, preserved across CRST. Linux's HDA driver
    // reads STATESTS during probe to enumerate codecs.
    sound_hda->statests = 0x0001;

    // §7.3.3.13 pin widget control: power-on default is impl-specific.
    // Default to OUT_ENABLE so a guest that doesn't explicitly enable
    // the pin (or a minimalist driver that skips that step) still gets
    // audio. Linux's HDA generic pin power-up overwrites this anyway.
    sound_hda->pin_ctrl_out = VERB_GET_PIN_WIDGET_CTRL_OUT_ENABLE;
    sound_hda->pin_ctrl_in  = 0;

    // Beep widget defaults — same convention as the main amp:
    // mute=1, gain=Offset (0 dB) per spec §7.3.3.7. Linux's HDA generic
    // codec creates "Beep Playback Switch" / "Beep Playback Volume"
    // alsa controls bound to these via SET_AMP_GAIN_MUTE on NID 4.
    sound_hda->beep_gain = HDA_AMP_OFFSET;
    sound_hda->beep_mute = 1;

    // Initialize per-stream identity. Descriptor index = SIE/SIS bit per
    // HDA spec §3.3.14, sequential ISS → OSS → BSS. Today: input slot at
    // index 0, output slot at index 1 (NO_IN=1, NO_OUT=1, NO_BSS=0).
    for (size_t i = 0; i < HDA_STREAMS_TOTAL; ++i) {
        sound_hda_stream_t *s = &sound_hda->streams[i];
        s->hda        = sound_hda;
        s->index      = (uint8_t)i;
        s->intsts_bit = (uint8_t)i;
        if (i < SOUND_HDA_PARAM_NO_IN) {
            s->dir = HDA_STREAM_DIR_INPUT;
        } else if (i < SOUND_HDA_PARAM_NO_IN + SOUND_HDA_PARAM_NO_OUT) {
            s->dir = HDA_STREAM_DIR_OUTPUT;
        } else {
            s->dir = HDA_STREAM_DIR_BIDIR;
        }
        // Spec §7.3.3.7: amp gain defaults to Offset (0 dB), and mute
        // "should default to 1 on codec reset." Linux's HDA generic
        // codec unmutes primary output paths during probe via
        // SET_AMP_GAIN_MUTE. Cache q15=0 so the worker treats the
        // pre-probe state as silent.
        s->left_gain  = HDA_AMP_OFFSET;
        s->right_gain = HDA_AMP_OFFSET;
        s->left_mute  = 1;
        s->right_mute = 1;
        s->gain_q15   = 0;
    }

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

    pci_dev_t *pci_dev = pci_attach_func(pci_bus, &sound_hda_desc);
    if (pci_dev)
        sound_hda->pci_func = pci_get_device_func(pci_dev, 0);

    // Backend selection priority:
    //   1. Caller-supplied write_fn (via sound_hda_init_ex) — skip the
    //      compile-time default. Used by embedders that want to route
    //      audio somewhere other than the host's native audio stack
    //      (managed runtimes, IPC channels, WAV capture fixtures, etc.).
    //   2. Compile-time USE_ALSA — the traditional Linux host path.
    //   3. Neither — PCI device enumerates but the stream worker drops PCM.
    if (write_fn != NULL) {
        sound_hda_backend_bridge_t *bridge = safe_new_obj(sound_hda_backend_bridge_t);
        bridge->user_fn = write_fn;
        bridge->abort_fn = abort_fn;
        bridge->user_data = user_data;
        sound_hda->subsystem.sound_data = bridge;
        sound_hda->subsystem.write = sound_hda_backend_bridge_write;
        // Only publish the bridge's abort indirection when the caller
        // actually supplied one — otherwise sound_hda_remove would
        // invoke a no-op indirection every teardown.
        if (abort_fn != NULL) {
            sound_hda->subsystem.abort = sound_hda_backend_bridge_abort;
        }
    } else {
#ifdef USE_ALSA
        if (!alsa_sound_init(&sound_hda->subsystem))
            return NULL;
#endif
    }

    return pci_dev;
}

PUBLIC pci_dev_t *sound_hda_init_auto_ex(rvvm_machine_t *machine,
                                         sound_hda_backend_write_fn write_fn,
                                         sound_hda_backend_abort_fn abort_fn,
                                         void *user_data)
{
    return sound_hda_init_ex(rvvm_get_pci_bus(machine), write_fn, abort_fn, user_data);
}

PUBLIC pci_dev_t *sound_hda_init(pci_bus_t *pci_bus)
{
    return sound_hda_init_ex(pci_bus, NULL, NULL, NULL);
}

PUBLIC pci_dev_t *sound_hda_init_auto(rvvm_machine_t *machine)
{
    return sound_hda_init(rvvm_get_pci_bus(machine));
}

POP_OPTIMIZATION_SIZE

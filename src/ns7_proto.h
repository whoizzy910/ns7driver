/*
 * ns7_proto.h - Numark NS7 USB protocol (Ploytec isochronous family).
 *
 * Pure functions and constants, no I/O. Sources:
 *   - Numark NS7 Mac driver 3.3.11 (NumarkNS7Audio.kext, Ploytec): device IDs,
 *     kInACR 8 -> 42-byte MIDI transfers, inACR_NS7 (41 MIDI bytes + control),
 *     initNS7 (39 MIDI-out bytes allowed), kMsWait 200, 24-bit 44.1 kHz 4 ch.
 *   - Protocol research by the Ozzy project (github.com/mischa85/Ozzy, MIT):
 *     handshake order, rate requests, status confirm, PCM frame formats, feedback.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef NS7_PROTO_H
#define NS7_PROTO_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* ---------------------------------------------------------------- device */

#define NS7_VID              0x15E4
#define NS7_PID              0x0071
#define NS7_RATE             44100u
#define NS7_OUT_CHANNELS     4
#define NS7_IN_CHANNELS      4
#define NS7_BYTES_PER_SAMPLE 3                       /* S24_3LE */
#define NS7_OUT_FRAME_BYTES  (NS7_OUT_CHANNELS * 3)  /* plain frame on the isoc OUT pipe */
#define NS7_IN_FRAME_BYTES   64                      /* bit-interleaved device input frame */
#define NS7_ALT_SETTING      1
#define NS7_SETUP_WAIT_MS    200                     /* kMsWait */

/* ------------------------------------------------------- control requests */

#define NS7_RT_VENDOR_IN     0xC0
#define NS7_RT_VENDOR_OUT    0x40
#define NS7_RT_CLASS_EP_OUT  0x22   /* SET_CUR to an endpoint */
#define NS7_RT_CLASS_EP_IN   0xA2   /* GET_CUR from an endpoint */

#define NS7_REQ_FIRMWARE     0x56   /* 'V', 15 bytes */
#define NS7_REQ_STATUS       0x49   /* 'I', wIndex 0 = selector/status byte */
#define NS7_REQ_SET_CUR      0x01
#define NS7_REQ_GET_CUR      0x81
#define NS7_RATE_SELECTOR    0x0100 /* wValue for SET_CUR/GET_CUR sampling frequency */

#define NS7_STATUS_CLOCK_AUTO 0x02  /* selector bit 1: automatic clock source */
#define NS7_STATUS_ARM        0x30  /* bits 5|4 set by the confirm write at stream start */

static inline void ns7_encode_rate(uint32_t rate, uint8_t b[3])
{
    b[0] = rate & 0xff;
    b[1] = (rate >> 8) & 0xff;
    b[2] = (rate >> 16) & 0xff;
}

static inline uint32_t ns7_decode_rate(const uint8_t b[3])
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16);
}

/* Status confirm: wValue = status | 0x30, sign-extended from 8 bits like the official driver */
static inline uint16_t ns7_confirm_wvalue(uint8_t status)
{
    return (uint16_t)(int16_t)(int8_t)(status | NS7_STATUS_ARM);
}

/* Selector write wValue (sign-extended byte) */
static inline uint16_t ns7_byte_wvalue(uint8_t b)
{
    return (uint16_t)(int16_t)(int8_t)b;
}

/* Firmware 'V' reply: byte 0 chip ID, byte 2 = version, shown as v1.(b/10).(b%10) */
static inline void ns7_firmware_version(const uint8_t *v, unsigned *chip, unsigned *major, unsigned *minor)
{
    *chip = v[0];
    *major = v[2] / 10;
    *minor = v[2] % 10;
}

/* ------------------------------------------------------------------ MIDI */

#define NS7_MIDI_XFER        42     /* bytes per MIDI transfer, each direction */
#define NS7_MIDI_IN_BYTES    41     /* MIDI slots before the control byte */
#define NS7_MIDI_IDLE        0xFD   /* empty slot */
#define NS7_MIDI_OUT_CTRL    0xE0   /* last byte of an OUT transfer */
#define NS7_MIDI_INTERVAL_US 1000   /* one transfer per millisecond */

/*
 * MIDI bytes in one IN transfer: the first 41 bytes minus idle slots.
 * Returns the count written to out (room for NS7_MIDI_IN_BYTES).
 */
static inline unsigned ns7_midi_in_unpack(const uint8_t *xfer, unsigned len, uint8_t *out)
{
    unsigned n = len < NS7_MIDI_IN_BYTES ? len : NS7_MIDI_IN_BYTES, c = 0;
    for (unsigned i = 0; i < n; i++)
        if (xfer[i] != NS7_MIDI_IDLE)
            out[c++] = xfer[i];
    return c;
}

/*
 * Prepare one OUT transfer and return how many MIDI bytes may go at its start.
 * Paced at 4,3,3,3,3,3,3,3 bytes per millisecond = 3125 bytes/s, a real
 * 31250-baud MIDI cable, so the controller's MIDI input never overruns.
 * (The official driver allows up to 39 per transfer on the NS7.)
 */
static inline unsigned ns7_midi_out_begin(uint8_t xfer[NS7_MIDI_XFER], uint32_t *index)
{
    memset(xfer, NS7_MIDI_IDLE, NS7_MIDI_XFER - 1);
    xfer[NS7_MIDI_XFER - 1] = NS7_MIDI_OUT_CTRL;
    return 3 + ((((*index)++) & 7) == 0);
}

/* --------------------------------------------------------- audio frames */

/* Output: the isoc OUT pipe carries plain S24_3LE frames, 4 channels interleaved. */

/*
 * Input: each 64-byte device frame carries up to 16 channels, one bit lane per
 * channel pair. Channel c (0-based) uses bit (c/2) of its bytes; even c
 * (channels 1,3,...) lives in bytes 0x00..0x17, odd c in 0x20..0x37. Byte i
 * holds sample bit (23 - i), MSB first. Output: channels x S24_3LE.
 */
static inline void ns7_decode_in_frame(const uint8_t *src, uint8_t *dst, unsigned channels)
{
    for (unsigned c = 0; c < channels; c++) {
        const uint8_t *b = src + ((c & 1) ? 0x20 : 0x00);
        unsigned lane = c >> 1;
        uint32_t s = 0;
        for (unsigned i = 0; i < 24; i++)
            s |= (uint32_t)((b[i] >> lane) & 1) << (23 - i);
        dst[c * 3 + 0] = s & 0xff;
        dst[c * 3 + 1] = (s >> 8) & 0xff;
        dst[c * 3 + 2] = (s >> 16) & 0xff;
    }
}

/* Inverse of ns7_decode_in_frame (for tests). */
static inline void ns7_encode_in_frame(const uint8_t *src, uint8_t *dst, unsigned channels)
{
    memset(dst, 0, NS7_IN_FRAME_BYTES);
    for (unsigned c = 0; c < channels; c++) {
        uint8_t *b = dst + ((c & 1) ? 0x20 : 0x00);
        unsigned lane = c >> 1;
        uint32_t s = src[c * 3] | (src[c * 3 + 1] << 8) | ((uint32_t)src[c * 3 + 2] << 16);
        for (unsigned i = 0; i < 24; i++)
            b[i] |= (uint8_t)(((s >> (23 - i)) & 1) << lane);
    }
}

static inline int32_t ns7_s24_get(const uint8_t *p)
{
    return (int32_t)((uint32_t)(p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16)) << 8) >> 8;
}

static inline void ns7_s24_put(uint8_t *p, int32_t s)
{
    p[0] = s & 0xff;
    p[1] = (s >> 8) & 0xff;
    p[2] = (s >> 16) & 0xff;
}

/* ------------------------------------------------- isochronous feedback */

/*
 * The feedback pipe (isoc IN, 3-byte packets) reports in byte 0 how many
 * frames the device played during that packet's interval. The host then
 * sends exactly that many, so output follows the NS7's own clock.
 * Timing is kept in 125 us units ("ticks"): a High Speed microframe, or 1/8
 * of a Full Speed frame.
 */
#define NS7_TICKS_PER_SEC    8000u
#define NS7_FB_RING          128u   /* ticks of history (16 ms), power of two */
#define NS7_FB_PACKET        3
#define NS7_FB_MAX_ERRORS    40

typedef struct {
    uint8_t  ring[NS7_FB_RING];     /* frames per tick from feedback, 0 = none */
    uint32_t rd, wr;
    uint32_t nominal_acc;           /* nominal-rate error accumulator */
    uint32_t fb_ticks;              /* ticks covered by one feedback packet */
    uint32_t lo, hi;                /* accepted counts per feedback packet */
    uint32_t errors;
    bool     broken;                /* too many bad packets: run at the nominal rate */
} ns7_pacer;

static inline void ns7_pacer_init(ns7_pacer *p, uint32_t fb_ticks)
{
    memset(p, 0, sizeof *p);
    p->fb_ticks = fb_ticks ? fb_ticks : 8;
    uint32_t tol = p->fb_ticks >= 8 ? 2 : 1;
    p->lo = NS7_RATE * p->fb_ticks / NS7_TICKS_PER_SEC;
    p->lo = p->lo > tol ? p->lo - tol : 0;
    p->hi = (NS7_RATE * p->fb_ticks + NS7_TICKS_PER_SEC - 1) / NS7_TICKS_PER_SEC + tol;
}

/* One feedback packet: spread its count over the ticks it covers. */
static inline void ns7_pacer_feedback(ns7_pacer *p, const uint8_t *pkt, uint32_t len)
{
    bool valid = len == NS7_FB_PACKET;
    uint32_t v = valid ? pkt[0] : 0;
    bool ok = valid && v >= p->lo && v <= p->hi && !p->broken;
    for (uint32_t k = 0; k < p->fb_ticks; k++) {
        uint32_t n = ok ? (v * (k + 1)) / p->fb_ticks - (v * k) / p->fb_ticks : 0;
        __atomic_store_n(&p->ring[(p->wr + k) & (NS7_FB_RING - 1)], (uint8_t)n, __ATOMIC_RELAXED);
    }
    p->wr += p->fb_ticks;
    if (!valid) {
        if (++p->errors > NS7_FB_MAX_ERRORS) p->broken = true;
    } else if (ok && p->errors) {
        p->errors--;
    }
}

/* Frames for an output packet covering `ticks` ticks: device counts, or nominal where none. */
static inline uint32_t ns7_pacer_frames(ns7_pacer *p, uint32_t ticks)
{
    uint32_t frames = 0;
    for (uint32_t k = 0; k < ticks; k++) {
        p->nominal_acc += NS7_RATE;
        uint32_t nominal = p->nominal_acc / NS7_TICKS_PER_SEC;
        p->nominal_acc -= nominal * NS7_TICKS_PER_SEC;
        uint32_t n = __atomic_load_n(&p->ring[p->rd & (NS7_FB_RING - 1)], __ATOMIC_RELAXED);
        p->rd++;
        frames += n ? n : nominal;
    }
    return frames;
}

#endif

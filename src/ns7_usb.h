/*
 * ns7_usb.h - userspace USB engine for the Numark NS7 (libusb, no kernel extension).
 * SPDX-License-Identifier: MIT
 */
#ifndef NS7_USB_H
#define NS7_USB_H

#include <stdint.h>
#include "ns7_ring.h"

typedef struct ns7_dev ns7_dev;

typedef void (*ns7_midi_in_fn)(void *ctx, const uint8_t *bytes, uint32_t n);

/*
 * Audio callbacks, all called on the USB thread (real-time: no locks, no allocation).
 * Positions count frames on the device's own clock since streaming started.
 *   out_fill:  fill `frames` S24_3LE x 4 frames that will be sent at position `pos`;
 *              return how many were real audio (the rest is counted as underrun).
 *   in_store:  `frames` recorded S24_3LE x 4 frames, the first at input position `pos`.
 *   clock:     `pos` frames have been handed to USB at host time `host` (mach ticks
 *              on macOS, CLOCK_MONOTONIC ns elsewhere); called once per OUT transfer.
 */
typedef struct {
    uint32_t     (*out_fill)(void *ctx, uint8_t *dst, uint32_t frames, uint64_t pos);
    void         (*in_store)(void *ctx, const uint8_t *src, uint32_t frames, uint64_t pos);
    void         (*clock)(void *ctx, uint64_t pos, uint64_t host);
    void          *audio_ctx;
    ns7_ring      *midi_out;      /* bytes for the controller; NULL = none */
    ns7_midi_in_fn midi_in;       /* bytes from the controller; NULL = discard */
    void          *midi_ctx;
} ns7_io;

uint64_t ns7_host_time(void);     /* the clock used by io.clock */
uint32_t ns7_frames_in_flight(ns7_dev *d);   /* output frames queued in USB transfers */
uint32_t ns7_out_transfer_frames(ns7_dev *d);

typedef struct {
    uint64_t out_frames, in_frames;
    uint64_t out_underruns;       /* frames filled with silence */
    uint64_t in_overruns;         /* frames dropped: nobody reading */
    uint64_t fb_packets, fb_bad;
    uint64_t midi_in_bytes, midi_out_bytes;
    uint64_t errors;
    int      feedback_ok;
    int      gone;                /* device unplugged */
} ns7_stats;

int      ns7_present(void);              /* is an NS7 plugged in? (cheap, no claiming) */
ns7_dev *ns7_open(int verbose);          /* find, open, claim, select alt 1, find pipes */
void     ns7_print_info(ns7_dev *d);     /* descriptors and pipes */
int      ns7_handshake(ns7_dev *d);      /* arm for streaming at 44.1 kHz; 0 = ok */
int      ns7_start(ns7_dev *d, const ns7_io *io);
void     ns7_get_stats(ns7_dev *d, ns7_stats *s);
void     ns7_stop(ns7_dev *d);
void     ns7_close(ns7_dev *d);

#endif

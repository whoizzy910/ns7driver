/*
 * ns7_serve.h - the driver service: owns the NS7, shares audio, MIDI and the
 * clock with the CoreAudio plug-in and the MIDI helper.
 * SPDX-License-Identifier: MIT
 */
#ifndef NS7_SERVE_H
#define NS7_SERVE_H

#include <stdint.h>
#include "ns7_shm.h"

/* Callback state ns7d uses while streaming into shared memory. */
typedef struct {
    ns7_shm          *shm;
    ns7_clock_tracker clock;
    uint64_t          last_clock_pos;
    int64_t           in_offset;       /* input position -> timeline position */
    int               in_aligned;
} ns7_serve_audio;

void     ns7_serve_audio_init(ns7_serve_audio *a, ns7_shm *shm, double host_ticks_per_sec, uint64_t epoch);
uint32_t ns7_serve_out_fill(void *ctx, uint8_t *dst, uint32_t frames, uint64_t pos);
void     ns7_serve_in_store(void *ctx, const uint8_t *src, uint32_t frames, uint64_t pos);
void     ns7_serve_clock(void *ctx, uint64_t pos, uint64_t host);

/* Shared memory transport: XPC on macOS, POSIX shm elsewhere (tests). */
ns7_shm *ns7_shm_create(void);                 /* ns7d: create and start handing it out */
ns7_shm *ns7_shm_connect(int quiet);           /* plug-in / helper / tests: map it */

double   ns7_host_ticks_per_sec(void);

int      ns7_serve_main(int verbose);          /* `ns7d serve`: runs until signalled */
int      ns7_midi_helper_main(void);           /* `ns7d midi-helper`: CoreMIDI ports for the user session */

#endif

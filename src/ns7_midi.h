/*
 * ns7_midi.h - MIDI side of the NS7 driver.
 *
 * Bytes from the controller are assembled into complete MIDI messages
 * (running status, SysEx) and published on a virtual MIDI source "NS7".
 * Messages apps send to the virtual destination "NS7" (LEDs) are queued
 * for the controller.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef NS7_MIDI_H
#define NS7_MIDI_H

#include <stdint.h>
#include "ns7_ring.h"

/* Byte stream -> complete messages */
typedef struct {
    uint8_t  status;        /* running status, 0 = none */
    uint8_t  buf[256];
    uint32_t len;
    uint32_t need;          /* data bytes still expected for the current message */
    int      sysex;
} ns7_midi_parser;

typedef void (*ns7_midi_msg_fn)(void *ctx, const uint8_t *msg, uint32_t len);

void ns7_midi_parser_init(ns7_midi_parser *p);
void ns7_midi_parse(ns7_midi_parser *p, const uint8_t *bytes, uint32_t n, ns7_midi_msg_fn fn, void *ctx);

/* Backend: CoreMIDI on macOS, stdout elsewhere. */
typedef struct ns7_midi ns7_midi;

/* to_device: ring the backend fills with bytes apps send to the NS7 (may be NULL). */
ns7_midi *ns7_midi_open(const char *name, ns7_ring *to_device, int monitor);
void      ns7_midi_from_device(ns7_midi *m, const uint8_t *bytes, uint32_t n);  /* USB thread */
void      ns7_midi_close(ns7_midi *m);

#endif

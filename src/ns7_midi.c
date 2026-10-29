/*
 * ns7_midi.c - MIDI message assembly and the CoreMIDI (or stdout) backend.
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ns7_midi.h"

/* ---------------------------------------------------------------- parser */

void ns7_midi_parser_init(ns7_midi_parser *p)
{
    memset(p, 0, sizeof *p);
}

static uint32_t data_bytes(uint8_t status)
{
    switch (status & 0xF0) {
    case 0xC0: case 0xD0: return 1;
    case 0xF0:
        switch (status) {
        case 0xF1: case 0xF3: return 1;
        case 0xF2: return 2;
        default: return 0;
        }
    default: return 2;
    }
}

void ns7_midi_parse(ns7_midi_parser *p, const uint8_t *bytes, uint32_t n, ns7_midi_msg_fn fn, void *ctx)
{
    for (uint32_t i = 0; i < n; i++) {
        uint8_t b = bytes[i];

        if (b >= 0xF8) {                         /* real-time: pass through, keep state */
            fn(ctx, &b, 1);
            continue;
        }
        if (b == 0xF0) {
            p->sysex = 1;
            p->status = 0;
            p->buf[0] = b;
            p->len = 1;
            continue;
        }
        if (b == 0xF7) {
            if (p->sysex) {
                p->buf[p->len++] = b;
                fn(ctx, p->buf, p->len);
            }
            p->sysex = 0;
            p->len = 0;
            continue;
        }
        if (b & 0x80) {                          /* any other status ends SysEx */
            if (p->sysex && p->len) fn(ctx, p->buf, p->len);
            p->sysex = 0;
            if (b >= 0xF0) {                     /* system common: no running status */
                p->status = 0;
                p->need = data_bytes(b);
                if (b == 0xF4 || b == 0xF5) { p->len = 0; continue; }
                p->buf[0] = b;
                p->len = 1;
                if (!p->need) { fn(ctx, p->buf, 1); p->len = 0; }
                else p->status = b;              /* held only until its data arrives */
                continue;
            }
            p->status = b;
            p->buf[0] = b;
            p->len = 1;
            p->need = data_bytes(b);
            continue;
        }

        /* data byte */
        if (p->sysex) {
            p->buf[p->len++] = b;
            if (p->len == sizeof p->buf) {       /* long SysEx: pass on in chunks */
                fn(ctx, p->buf, p->len);
                p->len = 0;
            }
            continue;
        }
        if (!p->status) continue;                /* stray data byte */
        if (p->len == 0) {                       /* running status */
            p->buf[0] = p->status;
            p->len = 1;
            p->need = data_bytes(p->status);
        }
        p->buf[p->len++] = b;
        if (--p->need == 0) {
            fn(ctx, p->buf, p->len);
            p->len = 0;
            if (p->status >= 0xF0) p->status = 0;
        }
    }
}

/* --------------------------------------------------------------- monitor */

static void print_msg(const uint8_t *msg, uint32_t len)
{
    static const char *names[8] = { "Note off", "Note on", "Poly AT", "CC", "Program", "Ch AT", "Pitch", "System" };
    uint8_t s = msg[0];
    if (s < 0xF0)
        printf("ch %2d  %-8s", (s & 0x0F) + 1, names[(s >> 4) - 8]);
    else
        printf("       %-8s", s == 0xF0 ? "SysEx" : "System");
    for (uint32_t i = 0; i < len && i < 16; i++)
        printf(" %02x", msg[i]);
    if (len > 16) printf(" ... (%u bytes)", len);
    printf("\n");
    fflush(stdout);
}

/* --------------------------------------------------------------- backend */

#ifdef __APPLE__
#include <CoreMIDI/CoreMIDI.h>
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

struct ns7_midi {
    MIDIClientRef    client;
    MIDIEndpointRef  source, dest;
    ns7_ring        *to_device;
    ns7_midi_parser  parser;
    int              monitor;
    Byte             list[1024];
    MIDIPacketList  *pl;
    MIDIPacket      *pkt;
};

static void flush_list(ns7_midi *m)
{
    if (m->pl->numPackets) MIDIReceived(m->source, m->pl);
    m->pkt = MIDIPacketListInit(m->pl);
}

static void emit(void *ctx, const uint8_t *msg, uint32_t len)
{
    ns7_midi *m = ctx;
    if (m->monitor) print_msg(msg, len);
    MIDIPacket *next = MIDIPacketListAdd(m->pl, sizeof m->list, m->pkt, 0, len, msg);
    if (!next) {
        flush_list(m);
        next = MIDIPacketListAdd(m->pl, sizeof m->list, m->pkt, 0, len, msg);
    }
    if (next) m->pkt = next;
}

static void dest_read(const MIDIPacketList *list, void *refcon, void *src)
{
    (void)src;
    ns7_midi *m = refcon;
    if (!m->to_device) return;
    const MIDIPacket *p = &list->packet[0];
    for (UInt32 i = 0; i < list->numPackets; i++) {
        ns7_ring_write(m->to_device, p->data, p->length);   /* drops if the ring is full */
        p = MIDIPacketNext(p);
    }
}

ns7_midi *ns7_midi_open(const char *name, ns7_ring *to_device, int monitor)
{
    ns7_midi *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->to_device = to_device;
    m->monitor = monitor;
    ns7_midi_parser_init(&m->parser);
    m->pl = (MIDIPacketList *)m->list;
    m->pkt = MIDIPacketListInit(m->pl);

    CFStringRef n = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
    OSStatus s = MIDIClientCreate(n, NULL, NULL, &m->client);
    if (!s) s = MIDISourceCreate(m->client, n, &m->source);
    if (!s) s = MIDIDestinationCreate(m->client, n, dest_read, m, &m->dest);
    CFRelease(n);
    if (s) {
        fprintf(stderr, "CoreMIDI error %d creating the %s ports\n", (int)s, name);
        ns7_midi_close(m);
        return NULL;
    }
    return m;
}

void ns7_midi_from_device(ns7_midi *m, const uint8_t *bytes, uint32_t n)
{
    ns7_midi_parse(&m->parser, bytes, n, emit, m);
    flush_list(m);
}

void ns7_midi_close(ns7_midi *m)
{
    if (!m) return;
    if (m->source) MIDIEndpointDispose(m->source);
    if (m->dest) MIDIEndpointDispose(m->dest);
    if (m->client) MIDIClientDispose(m->client);
    free(m);
}

#else  /* stdout backend for non-Mac builds */

struct ns7_midi {
    ns7_midi_parser parser;
    int monitor;
};

static void emit(void *ctx, const uint8_t *msg, uint32_t len)
{
    (void)ctx;
    print_msg(msg, len);
}

ns7_midi *ns7_midi_open(const char *name, ns7_ring *to_device, int monitor)
{
    (void)to_device;
    ns7_midi *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    ns7_midi_parser_init(&m->parser);
    m->monitor = monitor;
    fprintf(stderr, "[midi] no CoreMIDI on this system: printing \"%s\" messages\n", name);
    return m;
}

void ns7_midi_from_device(ns7_midi *m, const uint8_t *bytes, uint32_t n)
{
    ns7_midi_parse(&m->parser, bytes, n, emit, m);
}

void ns7_midi_close(ns7_midi *m)
{
    free(m);
}

#endif

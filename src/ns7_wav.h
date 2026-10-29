/*
 * ns7_wav.h - minimal PCM WAV reading and writing for the driver's test modes.
 * SPDX-License-Identifier: MIT
 */
#ifndef NS7_WAV_H
#define NS7_WAV_H

#include <stdint.h>
#include <stdio.h>

typedef struct {
    FILE    *f;
    uint32_t rate;
    uint16_t channels;
    uint16_t bits;           /* 16 or 24 */
    uint32_t frames;         /* total (reader) or written so far (writer) */
    uint32_t pos;            /* reader position */
    long     data_offset;
    int      writing;
} ns7_wav;

/* Reader: 16- or 24-bit PCM. Returns 0 on success, prints the reason otherwise. */
int      ns7_wav_open_read(ns7_wav *w, const char *path);
/* Read up to n frames converted to S24_3LE with `out_channels` channels
 * (mono -> both, stereo -> 1/2 copied to 3/4 if dup34, extra channels dropped). */
uint32_t ns7_wav_read_s24(ns7_wav *w, uint8_t *dst, uint32_t n, unsigned out_channels, int dup34);

/* Writer: 24-bit PCM. */
int      ns7_wav_open_write(ns7_wav *w, const char *path, uint32_t rate, uint16_t channels);
void     ns7_wav_write_s24(ns7_wav *w, const uint8_t *src, uint32_t frames);

void     ns7_wav_close(ns7_wav *w);

#endif

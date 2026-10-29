/*
 * ns7_wav.c - minimal PCM WAV I/O.
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include "ns7_wav.h"

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void wr32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void wr16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

int ns7_wav_open_read(ns7_wav *w, const char *path)
{
    memset(w, 0, sizeof *w);
    w->f = fopen(path, "rb");
    if (!w->f) { perror(path); return -1; }
    uint8_t h[12];
    if (fread(h, 1, 12, w->f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
        fprintf(stderr, "%s: not a WAV file\n", path);
        goto fail;
    }
    int have_fmt = 0;
    for (;;) {
        uint8_t ch[8];
        if (fread(ch, 1, 8, w->f) != 8) { fprintf(stderr, "%s: no audio data found\n", path); goto fail; }
        uint32_t size = rd32(ch + 4);
        if (!memcmp(ch, "fmt ", 4)) {
            uint8_t fmt[40] = { 0 };
            uint32_t take = size < sizeof fmt ? size : sizeof fmt;
            if (fread(fmt, 1, take, w->f) != take) goto fail;
            if (size > take) fseek(w->f, size - take, SEEK_CUR);
            uint16_t tag = rd16(fmt);
            if (tag == 0xFFFE && take >= 26) tag = rd16(fmt + 24);   /* WAVE_FORMAT_EXTENSIBLE */
            w->channels = rd16(fmt + 2);
            w->rate = rd32(fmt + 4);
            w->bits = rd16(fmt + 14);
            if (tag != 1 || (w->bits != 16 && w->bits != 24) || !w->channels) {
                fprintf(stderr, "%s: need 16- or 24-bit PCM (got format %u, %u bits)\n", path, tag, w->bits);
                goto fail;
            }
            have_fmt = 1;
        } else if (!memcmp(ch, "data", 4)) {
            if (!have_fmt) { fprintf(stderr, "%s: data before fmt\n", path); goto fail; }
            w->data_offset = ftell(w->f);
            w->frames = size / (w->channels * (w->bits / 8));
            return 0;
        } else {
            fseek(w->f, size + (size & 1), SEEK_CUR);
        }
    }
fail:
    fclose(w->f);
    w->f = NULL;
    return -1;
}

uint32_t ns7_wav_read_s24(ns7_wav *w, uint8_t *dst, uint32_t n, unsigned out_channels, int dup34)
{
    uint32_t bps = w->bits / 8, frame = w->channels * bps, done = 0;
    uint8_t buf[4096];
    while (done < n && w->pos < w->frames) {
        uint32_t want = n - done, left = w->frames - w->pos;
        if (want > left) want = left;
        if (want > sizeof buf / frame) want = sizeof buf / frame;
        uint32_t got = (uint32_t)fread(buf, frame, want, w->f);
        if (!got) break;
        for (uint32_t i = 0; i < got; i++) {
            uint8_t *o = dst + (done + i) * out_channels * 3;
            for (unsigned c = 0; c < out_channels; c++) {
                unsigned src_c = c;
                if (w->channels == 1) src_c = 0;
                else if (c >= w->channels) src_c = dup34 ? c % 2 : (unsigned)-1;
                if (src_c == (unsigned)-1) { o[c * 3] = o[c * 3 + 1] = o[c * 3 + 2] = 0; continue; }
                const uint8_t *s = buf + i * frame + src_c * bps;
                if (bps == 2) { o[c * 3] = 0; o[c * 3 + 1] = s[0]; o[c * 3 + 2] = s[1]; }
                else { o[c * 3] = s[0]; o[c * 3 + 1] = s[1]; o[c * 3 + 2] = s[2]; }
            }
        }
        done += got;
        w->pos += got;
    }
    return done;
}

static void write_header(ns7_wav *w)
{
    uint8_t h[44];
    uint32_t data = w->frames * w->channels * 3;
    memcpy(h, "RIFF", 4); wr32(h + 4, 36 + data); memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4); wr32(h + 16, 16); wr16(h + 20, 1); wr16(h + 22, w->channels);
    wr32(h + 24, w->rate); wr32(h + 28, w->rate * w->channels * 3); wr16(h + 32, w->channels * 3); wr16(h + 34, 24);
    memcpy(h + 36, "data", 4); wr32(h + 40, data);
    fseek(w->f, 0, SEEK_SET);
    fwrite(h, 1, 44, w->f);
    fseek(w->f, 0, SEEK_END);
}

int ns7_wav_open_write(ns7_wav *w, const char *path, uint32_t rate, uint16_t channels)
{
    memset(w, 0, sizeof *w);
    w->f = fopen(path, "wb");
    if (!w->f) { perror(path); return -1; }
    w->rate = rate;
    w->channels = channels;
    w->bits = 24;
    w->writing = 1;
    write_header(w);
    w->data_offset = 44;
    return 0;
}

void ns7_wav_write_s24(ns7_wav *w, const uint8_t *src, uint32_t frames)
{
    fwrite(src, (size_t)w->channels * 3, frames, w->f);
    w->frames += frames;
}

void ns7_wav_close(ns7_wav *w)
{
    if (!w->f) return;
    if (w->writing) write_header(w);
    fclose(w->f);
    w->f = NULL;
}

/*
 * ns7d - userspace driver for the Numark NS7 on Apple Silicon (no kernel extension).
 *
 *   ns7d info                 USB layout, firmware, status (read-only)
 *   ns7d run                  NS7 MIDI ports + audio engine, input level meters
 *   ns7d midi                 like run, and print every MIDI message
 *   ns7d tone [seconds]       test tone: 440 Hz on outputs 1/2, 660 Hz on 3/4
 *   ns7d play FILE.wav        play a 44.1 kHz WAV on outputs 1/2 (add --cue for 3/4 too)
 *   ns7d record FILE.wav SEC  record the 4 inputs
 *   ns7d serve                driver service for the NS7 Audio plug-in (launchd runs this)
 *   ns7d midi-helper          NS7 MIDI ports in your login session (launchd runs this)
 *   options: -v verbose
 *
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ns7_midi.h"
#include "ns7_proto.h"
#include "ns7_ring.h"
#include "ns7_serve.h"
#include "ns7_usb.h"
#include "ns7_wav.h"

#define OUT_RING_BYTES  65536          /* ~124 ms of 4-channel S24 */
#define IN_RING_BYTES   131072
#define MIDI_RING_BYTES 4096
#define OUT_TARGET_FRAMES 441          /* keep ~10 ms queued ahead of the device */

static volatile sig_atomic_t g_stop;
static void on_signal(int s) { (void)s; g_stop = 1; }

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void nap_ms(int ms)
{
    struct timespec ts = { 0, ms * 1000000L };
    nanosleep(&ts, NULL);
}

static void usage(void)
{
    fprintf(stderr,
        "usage: ns7d [-v] info | run | midi | tone [sec] | play FILE.wav [--cue] | record FILE.wav SEC\n"
        "                 | serve | midi-helper\n");
}

/* --------------------------------------------------------- audio sources */

typedef struct {
    int      kind;          /* 0 silence, 1 tone, 2 wav */
    double   phase[2];
    ns7_wav  wav;
    int      dup34;
    int      finished;
} source;

static uint32_t source_fill(source *s, uint8_t *dst, uint32_t frames)
{
    if (s->kind == 1) {
        const double f[2] = { 440.0, 660.0 }, amp = 0.25 * 8388607.0;   /* -12 dBFS */
        for (uint32_t i = 0; i < frames; i++) {
            for (int pair = 0; pair < 2; pair++) {
                int32_t v = (int32_t)(sin(s->phase[pair]) * amp);
                s->phase[pair] += 2 * M_PI * f[pair] / NS7_RATE;
                if (s->phase[pair] > 2 * M_PI) s->phase[pair] -= 2 * M_PI;
                ns7_s24_put(dst + i * 12 + pair * 6, v);
                ns7_s24_put(dst + i * 12 + pair * 6 + 3, v);
            }
        }
        return frames;
    }
    if (s->kind == 2) {
        uint32_t got = ns7_wav_read_s24(&s->wav, dst, frames, NS7_OUT_CHANNELS, s->dup34);
        if (got < frames) {
            memset(dst + got * 12, 0, (frames - got) * 12);
            s->finished = 1;
        }
        return frames;
    }
    memset(dst, 0, frames * 12);
    return frames;
}

/* Keep the output ring about OUT_TARGET_FRAMES ahead of the device. */
static void feed(ns7_ring *out, source *s)
{
    uint8_t buf[256 * 12];
    while (ns7_ring_used(out) < OUT_TARGET_FRAMES * 12) {
        uint32_t n = source_fill(s, buf, 256);
        ns7_ring_write(out, buf, n * 12);
    }
}

/* ------------------------------------------------------------------ MIDI */

static void midi_in(void *ctx, const uint8_t *bytes, uint32_t n)
{
    ns7_midi_from_device((ns7_midi *)ctx, bytes, n);
}

/* Test modes move audio through plain FIFO rings (the plug-in path uses ns7_shm instead). */
typedef struct { ns7_ring *out, *in; uint64_t in_overruns; } fifo_audio;

static uint32_t fifo_out_fill(void *ctx, uint8_t *dst, uint32_t frames, uint64_t pos)
{
    (void)pos;
    fifo_audio *a = ctx;
    uint32_t got = ns7_ring_read(a->out, dst, frames * NS7_OUT_FRAME_BYTES) / NS7_OUT_FRAME_BYTES;
    memset(dst + got * NS7_OUT_FRAME_BYTES, 0, (frames - got) * NS7_OUT_FRAME_BYTES);
    return got;
}

static void fifo_in_store(void *ctx, const uint8_t *src, uint32_t frames, uint64_t pos)
{
    (void)pos;
    fifo_audio *a = ctx;
    uint32_t bytes = frames * NS7_IN_CHANNELS * 3;
    if (ns7_ring_write(a->in, src, bytes) < bytes) a->in_overruns++;
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv)
{
    int verbose = 0, argi = 1;
    while (argi < argc && argv[argi][0] == '-' && strcmp(argv[argi], "--cue")) {
        if (!strcmp(argv[argi], "-v")) verbose = 1;
        else { usage(); return 2; }
        argi++;
    }
    const char *cmd = argi < argc ? argv[argi++] : "run";

    source src = { 0 };
    const char *rec_path = NULL;
    double duration = 0;
    int monitor = 0;

    if (!strcmp(cmd, "serve")) return ns7_serve_main(verbose);
    if (!strcmp(cmd, "midi-helper")) return ns7_midi_helper_main();

    if (!strcmp(cmd, "info")) {
    } else if (!strcmp(cmd, "run")) {
    } else if (!strcmp(cmd, "midi")) {
        monitor = 1;
    } else if (!strcmp(cmd, "tone")) {
        src.kind = 1;
        duration = argi < argc ? atof(argv[argi]) : 10;
    } else if (!strcmp(cmd, "play")) {
        if (argi >= argc) { usage(); return 2; }
        if (ns7_wav_open_read(&src.wav, argv[argi])) return 1;
        if (src.wav.rate != NS7_RATE)
            fprintf(stderr, "note: %s is %u Hz; the NS7 runs at 44100 Hz, so it will play at the wrong speed\n",
                    argv[argi], src.wav.rate);
        src.kind = 2;
        for (int i = argi + 1; i < argc; i++) if (!strcmp(argv[i], "--cue")) src.dup34 = 1;
    } else if (!strcmp(cmd, "record")) {
        if (argi + 1 >= argc) { usage(); return 2; }
        rec_path = argv[argi];
        duration = atof(argv[argi + 1]);
        if (duration <= 0) { usage(); return 2; }
    } else {
        usage();
        return 2;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    ns7_dev *d = ns7_open(verbose);
    if (!d) return 1;

    if (!strcmp(cmd, "info")) {
        ns7_print_info(d);
        ns7_close(d);
        return 0;
    }
    if (verbose) ns7_print_info(d);
    if (ns7_handshake(d)) {
        fprintf(stderr, "Setup failed. Run with -v and send the output.\n");
        ns7_close(d);
        return 1;
    }

    ns7_ring *out = ns7_ring_new(OUT_RING_BYTES);
    ns7_ring *in = ns7_ring_new(IN_RING_BYTES);
    ns7_ring *to_dev = ns7_ring_new(MIDI_RING_BYTES);
    ns7_midi *midi = ns7_midi_open("NS7", to_dev, monitor);

    ns7_wav rec = { 0 };
    if (rec_path && ns7_wav_open_write(&rec, rec_path, NS7_RATE, NS7_IN_CHANNELS)) return 1;

    feed(out, &src);
    fifo_audio fa = { out, in, 0 };
    ns7_io io = { fifo_out_fill, fifo_in_store, NULL, &fa, to_dev, midi ? midi_in : NULL, midi };
    if (ns7_start(d, &io)) fprintf(stderr, "warning: some transfers failed to start\n");

    printf("Running. %s Ctrl-C to stop.\n",
           midi ? "MIDI ports \"NS7\" are live." : "MIDI ports unavailable.");
    fflush(stdout);

    double t0 = now(), last = t0;
    ns7_stats prev = { 0 };
    int32_t peak[NS7_IN_CHANNELS] = { 0 };
    uint8_t buf[1024 * 12];

    while (!g_stop) {
        feed(out, &src);
        if (src.kind == 2 && src.finished && ns7_ring_used(out) == 0) break;

        uint32_t got;
        while ((got = ns7_ring_read(in, buf, sizeof buf)) > 0) {
            uint32_t frames = got / 12;
            if (rec_path) ns7_wav_write_s24(&rec, buf, frames);
            for (uint32_t f = 0; f < frames; f++)
                for (int c = 0; c < NS7_IN_CHANNELS; c++) {
                    int32_t v = abs(ns7_s24_get(buf + f * 12 + c * 3));
                    if (v > peak[c]) peak[c] = v;
                }
        }

        double t = now();
        if (duration > 0 && t - t0 >= duration) break;
        if (t - last >= 2.0) {
            ns7_stats s;
            ns7_get_stats(d, &s);
            if (s.gone) { fprintf(stderr, "NS7 disconnected.\n"); break; }
            double dt = t - last;
            printf("out %5.0f fr/s  in %5.0f fr/s  feedback %s  underruns %llu  midi in %llu out %llu  errors %llu  | in dB:",
                   (s.out_frames - prev.out_frames) / dt, (s.in_frames - prev.in_frames) / dt,
                   s.feedback_ok ? "ok " : "off", (unsigned long long)s.out_underruns,
                   (unsigned long long)s.midi_in_bytes, (unsigned long long)s.midi_out_bytes,
                   (unsigned long long)s.errors);
            for (int c = 0; c < NS7_IN_CHANNELS; c++) {
                if (peak[c]) printf(" %5.1f", 20 * log10(peak[c] / 8388608.0));
                else printf("  -inf");
                peak[c] = 0;
            }
            printf("\n");
            fflush(stdout);
            prev = s;
            last = t;
        }
        nap_ms(2);
    }

    ns7_stop(d);
    ns7_stats s;
    ns7_get_stats(d, &s);
    printf("Stopped. %llu frames out, %llu in, %llu underruns, %llu input overruns, feedback packets %llu (%llu bad)\n",
           (unsigned long long)s.out_frames, (unsigned long long)s.in_frames,
           (unsigned long long)s.out_underruns, (unsigned long long)fa.in_overruns,
           (unsigned long long)s.fb_packets, (unsigned long long)s.fb_bad);
    if (rec_path) {
        printf("Recorded %u frames to %s\n", rec.frames, rec_path);
        ns7_wav_close(&rec);
    }
    if (src.kind == 2) ns7_wav_close(&src.wav);
    ns7_midi_close(midi);
    ns7_close(d);
    free(out); free(in); free(to_dev);
    return 0;
}

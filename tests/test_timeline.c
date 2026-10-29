/*
 * test_timeline.c - simulates the NS7, the USB thread and CoreAudio together,
 * in simulated time, to check that audio written by CoreAudio at the sample
 * times derived from the shared clock reaches the USB packets without gaps,
 * and that recorded input is where CoreAudio looks for it.
 *
 * The simulated NS7 runs off-nominal (its crystal) and USB completions jitter.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/ns7_proto.h"
#include "../src/ns7_shm.h"
#include "../src/ns7_serve.h"

#define XFER_FRAMES   88          /* 2 ms transfers, as the engine uses */
#define IN_XFER       80
#define HAL_BUFFER    128

static float expected_out(uint64_t s) { return (float)((s % 9973) / 9973.0 - 0.5); }
static float expected_in(uint64_t s, int c) { return (float)(((s * 7 + c) % 8191) / 8191.0 - 0.5); }

static uint32_t rng = 1;
static double jitter(double max_ns) { rng = rng * 1664525 + 1013904223; return (rng >> 8) / 16777216.0 * max_ns; }

static int run(double ppm, double jitter_ns, int verbose)
{
    static ns7_shm shm;
    ns7_shm_init(&shm, NS7_RATE);
    ns7_serve_audio a;
    ns7_serve_audio_init(&a, &shm, 1e9, 1);
    shm.client_running = 1;

    const double ns_per_frame = 1e9 / (NS7_RATE * (1 + ppm * 1e-6));   /* true device rate */
    const uint32_t safety = 2 * XFER_FRAMES + 32;
    const double t_end = 20e9;

    uint64_t out_pos = 0;                 /* frames handed to USB (engine) */
    uint64_t in_pos = 0;                  /* frames recorded (engine) */
    double next_refill = 0, next_in = 0, next_hal = 0;
    uint64_t hal_next_write = 0;          /* CoreAudio: next output sample time it will write */
    int hal_started = 0;
    long checked = 0, bad = 0, in_checked = 0, in_bad = 0, in_missing = 0;
    double max_clock_err = 0;
    uint8_t pkt[XFER_FRAMES * 12];

    /* pre-queue 3 transfers like the engine does before the first completion */
    for (int i = 0; i < 3; i++) {
        ns7_serve_clock(&a, out_pos, 0);
        ns7_serve_out_fill(&a, pkt, XFER_FRAMES, out_pos);
        out_pos += XFER_FRAMES;
    }

    for (double t = 0; t < t_end;) {
        double tn = next_refill;
        if (next_in < tn) tn = next_in;
        if (next_hal < tn) tn = next_hal;
        t = tn;

        if (t == next_refill) {
            /* a transfer completed: the device consumed up to (out_pos - 2 transfers) */
            uint64_t host = (uint64_t)(t + jitter(jitter_ns));
            ns7_serve_clock(&a, out_pos, host);
            ns7_serve_out_fill(&a, pkt, XFER_FRAMES, out_pos);
            if (t > 2e9) {
                for (int f = 0; f < XFER_FRAMES; f++) {
                    int32_t got = ns7_s24_get(pkt + f * 12);
                    int32_t want = (int32_t)(expected_out(out_pos + f) * 8388608.0f);
                    checked++;
                    if (abs(got - want) > 1) bad++;
                }
            }
            out_pos += XFER_FRAMES;
            /* when is the next completion? when the device has played one more transfer */
            next_refill = (double)(out_pos - 2 * XFER_FRAMES) * ns_per_frame;
            /* how far is the published clock from the truth here? */
            ns7_clock c;
            if (ns7_clock_read(&shm, &c) && t > 2e9) {
                double truth = (double)(out_pos - 2 * XFER_FRAMES - XFER_FRAMES);  /* position being sent ~now */
                double err = fabs(ns7_clock_pos_at(&c, (uint64_t)t) - truth);
                if (err > max_clock_err) max_clock_err = err;
            }
        }
        if (t == next_in) {
            /* the NS7 recorded IN_XFER more frames; the transfer completes now */
            uint8_t s24[IN_XFER * 12];
            for (int f = 0; f < IN_XFER; f++)
                for (int c = 0; c < 4; c++)
                    ns7_s24_put(s24 + (f * 4 + c) * 3, (int32_t)(expected_in(in_pos + f, c) * 8388608.0f));
            ns7_serve_in_store(&a, s24, IN_XFER, in_pos);
            in_pos += IN_XFER;
            next_in = (double)in_pos * ns_per_frame + jitter(jitter_ns);
        }
        if (t == next_hal) {
            /* CoreAudio IO cycle: write output ahead of now, read input behind now */
            ns7_clock c;
            if (ns7_clock_read(&shm, &c)) {
                double now = ns7_clock_pos_at(&c, (uint64_t)t);
                uint64_t first = (uint64_t)now + safety;
                if (!hal_started) { hal_next_write = first; hal_started = 1; }
                if (first > hal_next_write + HAL_BUFFER) hal_next_write = first;  /* HAL would resync */
                for (int f = 0; f < HAL_BUFFER; f++) {
                    uint64_t s = hal_next_write + f;
                    float *dst = &shm.out[(s & (NS7_SHM_RING_FRAMES - 1)) * 4];
                    for (int ch = 0; ch < 4; ch++) dst[ch] = expected_out(s);
                }
                hal_next_write += HAL_BUFFER;

                /* input: the HAL reads [now - in_safety - buffer, now - in_safety) */
                int64_t in_first = (int64_t)now - 256 - HAL_BUFFER;
                uint64_t written = shm.in_written;
                if (t > 2e9 && in_first > 0) {
                    for (int f = 0; f < HAL_BUFFER; f++) {
                        uint64_t s = (uint64_t)in_first + f;
                        if (s >= written) { in_missing++; continue; }
                        float *src = &shm.in[(s & (NS7_SHM_RING_FRAMES - 1)) * 4];
                        /* which recorded frame landed at timeline position s? */
                        in_checked++;
                        int64_t rec = (int64_t)s - a.in_offset;
                        if (rec < 0 || fabsf(src[1] - expected_in((uint64_t)rec, 1)) > 1e-5f) in_bad++;
                    }
                }
            }
            /* the HAL wakes once per buffer of its own sample time */
            next_hal = t + HAL_BUFFER * ns_per_frame;
        }
    }

    int ok = bad == 0 && in_bad == 0 && in_missing == 0;
    double rate = 1e9 / a.clock.out.host_per_frame;
    double want_rate = NS7_RATE * (1 + ppm * 1e-6);
    if (fabs(rate - want_rate) > 0.5) ok = 0;
    if (verbose || !ok)
        printf("%s ppm %+5.0f jitter %4.0f us: out %ld frames, %ld bad | in %ld, %ld bad, %ld not yet written | "
               "rate %.3f Hz (true %.3f) | clock error max %.1f frames\n",
               ok ? "ok  " : "FAIL", ppm, jitter_ns / 1000, checked, bad, in_checked, in_bad, in_missing,
               rate, want_rate, max_clock_err);
    return ok;
}

int main(void)
{
    int ok = 1;
    const double ppms[] = { 0, 100, -100, 300 };
    const double jit[] = { 0, 500e3, 1000e3 };
    for (unsigned i = 0; i < 4; i++)
        for (unsigned j = 0; j < 3; j++)
            ok &= run(ppms[i], jit[j], 1);
    printf(ok ? "timeline: all scenarios passed\n" : "timeline: FAILED\n");
    return !ok;
}

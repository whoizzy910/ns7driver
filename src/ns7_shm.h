/*
 * ns7_shm.h - memory shared between ns7d (USB side), the NS7 Audio plug-in
 * (CoreAudio side) and the MIDI helper. Plain C, no pointers, fixed layout.
 *
 * Timeline: every sample position is a frame count on the NS7's own clock,
 * counted from when streaming started (clock_epoch changes on each restart).
 * Position P is sent to USB at host time clock_host + (P - clock_pos) * host_per_frame.
 * CoreAudio's sample time is this same position, so the plug-in writes output
 * frame S into out[S mod ring] and ns7d sends it when its position reaches S.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef NS7_SHM_H
#define NS7_SHM_H

#include <stdint.h>
#include <string.h>

#define NS7_SHM_MAGIC        0x3137534Eu   /* "NS71" */
#define NS7_SHM_VERSION      1u
#define NS7_SHM_RING_FRAMES  16384u        /* power of two; also CoreAudio's zero-timestamp period */
#define NS7_SHM_CHANNELS     4u
#define NS7_SHM_MIDI_BYTES   4096u
#define NS7_XPC_SERVICE      "com.ns7driver.daemon"

/* Same layout as ns7_ring (ns7_ring.h), with its data inline. */
typedef struct {
    uint32_t size, _pad;
    uint64_t head, tail;
    uint8_t  data[NS7_SHM_MIDI_BYTES];
} ns7_shm_byte_ring;

typedef struct {
    uint32_t magic, version;
    uint32_t rate, channels_out, channels_in, ring_frames;

    /* written by ns7d */
    uint32_t device_present;           /* NS7 connected and streaming */
    uint32_t clock_seq;                /* seqlock: odd while the clock fields change */
    uint64_t clock_epoch;              /* +1 every time streaming (re)starts */
    uint64_t clock_pos;                /* a position ... */
    uint64_t clock_host;               /* ... and the host time it is sent at */
    double   host_per_frame;           /* host ticks per frame (follows the NS7's crystal) */
    uint32_t out_latency, in_latency;  /* frames, for CoreAudio */
    uint32_t out_safety, in_safety;
    uint64_t in_written;               /* input positions below this are valid */
    uint64_t out_underruns;

    /* written by the plug-in */
    uint32_t client_running;           /* CoreAudio IO is running */
    uint32_t _pad;

    /* MIDI: ns7d -> helper, helper -> ns7d */
    ns7_shm_byte_ring midi_from_device;
    ns7_shm_byte_ring midi_to_device;

    /* audio, float32 interleaved x 4, indexed by position mod ring_frames */
    float out[NS7_SHM_RING_FRAMES * NS7_SHM_CHANNELS];
    float in[NS7_SHM_RING_FRAMES * NS7_SHM_CHANNELS];
} ns7_shm;

static inline void ns7_shm_init(ns7_shm *s, uint32_t rate)
{
    memset(s, 0, sizeof *s);
    s->magic = NS7_SHM_MAGIC;
    s->version = NS7_SHM_VERSION;
    s->rate = rate;
    s->channels_out = s->channels_in = NS7_SHM_CHANNELS;
    s->ring_frames = NS7_SHM_RING_FRAMES;
    s->midi_from_device.size = s->midi_to_device.size = NS7_SHM_MIDI_BYTES;
}

static inline int ns7_shm_valid(const ns7_shm *s)
{
    return s->magic == NS7_SHM_MAGIC && s->version == NS7_SHM_VERSION &&
           s->ring_frames == NS7_SHM_RING_FRAMES && s->channels_out == NS7_SHM_CHANNELS;
}

/* ------------------------------------------------------------- the clock */

typedef struct {
    uint64_t epoch, pos, host;
    double   host_per_frame;
} ns7_clock;

static inline void ns7_clock_publish(ns7_shm *s, const ns7_clock *c)
{
    uint32_t seq = __atomic_load_n(&s->clock_seq, __ATOMIC_RELAXED);
    __atomic_store_n(&s->clock_seq, seq + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    s->clock_epoch = c->epoch;
    s->clock_pos = c->pos;
    s->clock_host = c->host;
    s->host_per_frame = c->host_per_frame;
    __atomic_store_n(&s->clock_seq, seq + 2, __ATOMIC_RELEASE);
}

/* Returns 1 with a consistent snapshot, 0 if there is no clock yet. */
static inline int ns7_clock_read(const ns7_shm *s, ns7_clock *c)
{
    for (int tries = 0; tries < 100; tries++) {
        uint32_t a = __atomic_load_n(&s->clock_seq, __ATOMIC_ACQUIRE);
        if (a & 1) continue;
        c->epoch = s->clock_epoch;
        c->pos = s->clock_pos;
        c->host = s->clock_host;
        c->host_per_frame = s->host_per_frame;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&s->clock_seq, __ATOMIC_RELAXED) == a)
            return c->host_per_frame > 0;
    }
    return 0;
}

/* Position being sent at host time `host`. */
static inline double ns7_clock_pos_at(const ns7_clock *c, uint64_t host)
{
    return (double)c->pos + ((double)(int64_t)(host - c->host)) / c->host_per_frame;
}

/*
 * CoreAudio zero timestamp: the latest multiple of `period` frames at or
 * before `now`, and its host time.
 */
static inline void ns7_clock_zero(const ns7_clock *c, uint64_t now, uint32_t period,
                                  double *sample_time, uint64_t *host_time)
{
    double p = ns7_clock_pos_at(c, now);
    if (p < 0) p = 0;
    uint64_t z = (uint64_t)p / period * period;
    *sample_time = (double)z;
    *host_time = c->host + (uint64_t)(int64_t)(((double)z - (double)c->pos) * c->host_per_frame);
}

/*
 * Clock tracking in ns7d. USB completions are only ever late (by up to a
 * millisecond or so), never early, so the earliest completion in each quarter
 * second sits on the true line. A least-squares fit through those points over
 * the last 16 s gives the NS7's real rate without the jitter.
 */
#define NS7_CLOCK_POINTS  64
#define NS7_CLOCK_WINDOW  11025          /* frames per point (0.25 s) */

typedef struct {
    ns7_clock out;
    double    nominal;                   /* host ticks per frame at exactly 44.1 kHz */
    uint64_t  base_pos, base_host;       /* origin for the fit (keeps the doubles small) */
    int       started;
    /* current window: the earliest completion relative to the nominal line */
    uint64_t  win_start;
    double    win_best;                  /* min of (host - base_host) - (pos - base_pos) * nominal */
    uint64_t  win_pos, win_host;
    /* fitted points */
    double    px[NS7_CLOCK_POINTS], py[NS7_CLOCK_POINTS];
    unsigned  npoints, next;
    double    fit_a, fit_b;              /* host - base_host = a + b * (pos - base_pos) */
    int       have_fit;
} ns7_clock_tracker;

static inline void ns7_clock_tracker_init(ns7_clock_tracker *t, double host_ticks_per_sec, uint32_t rate, uint64_t epoch)
{
    memset(t, 0, sizeof *t);
    t->nominal = host_ticks_per_sec / rate;
    t->out.epoch = epoch;
}

static inline void ns7_clock_tracker_fit(ns7_clock_tracker *t)
{
    double sx = 0, sy = 0, sxx = 0, sxy = 0, n = t->npoints;
    for (unsigned i = 0; i < t->npoints; i++) {
        sx += t->px[i]; sy += t->py[i];
        sxx += t->px[i] * t->px[i]; sxy += t->px[i] * t->py[i];
    }
    double den = n * sxx - sx * sx;
    if (t->npoints >= 4 && den > 0) {
        double b = (n * sxy - sx * sy) / den;
        if (b > t->nominal * 0.99 && b < t->nominal * 1.01) {   /* a crystal is within 0.01% */
            t->fit_b = b;
            t->fit_a = (sy - b * sx) / n;
            t->have_fit = 1;
            return;
        }
    }
    /* too few points: nominal rate through the earliest point seen */
    double best = 1e300;
    for (unsigned i = 0; i < t->npoints; i++) {
        double d = t->py[i] - t->px[i] * t->nominal;
        if (d < best) best = d;
    }
    t->fit_b = t->nominal;
    t->fit_a = t->npoints ? best : 0;
    t->have_fit = 1;
}

static inline void ns7_clock_tracker_update(ns7_clock_tracker *t, uint64_t pos, uint64_t host)
{
    if (!t->started) {
        t->started = 1;
        t->base_pos = pos;
        t->base_host = host;
        t->win_start = pos;
        t->win_best = 1e300;
        t->fit_b = t->nominal;
        t->fit_a = 0;
        t->have_fit = 1;
    }
    double x = (double)(pos - t->base_pos);
    double y = (double)(int64_t)(host - t->base_host);
    double d = y - x * t->nominal;
    if (d < t->win_best) { t->win_best = d; t->win_pos = pos; t->win_host = host; }

    if (pos - t->win_start >= NS7_CLOCK_WINDOW) {
        t->px[t->next] = (double)(t->win_pos - t->base_pos);
        t->py[t->next] = (double)(int64_t)(t->win_host - t->base_host);
        t->next = (t->next + 1) % NS7_CLOCK_POINTS;
        if (t->npoints < NS7_CLOCK_POINTS) t->npoints++;
        t->win_start = pos;
        t->win_best = 1e300;
        ns7_clock_tracker_fit(t);
    } else if (t->npoints == 0 && d < t->fit_a) {
        t->fit_a = d;                    /* first quarter second: follow the earliest completion */
    }

    t->out.pos = pos;
    t->out.host = t->base_host + (uint64_t)(int64_t)(t->fit_a + t->fit_b * x);
    t->out.host_per_frame = t->fit_b;
}

#endif

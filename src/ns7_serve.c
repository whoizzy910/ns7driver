/*
 * ns7_serve.c - `ns7d serve` and `ns7d midi-helper`, and the shared-memory transport.
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>

#include "ns7_midi.h"
#include "ns7_proto.h"
#include "ns7_ring.h"
#include "ns7_serve.h"
#include "ns7_usb.h"

#ifdef __APPLE__
#include <mach/mach_time.h>
#include <xpc/xpc.h>
#include <dispatch/dispatch.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#define NS7_POSIX_SHM "/ns7driver"
#endif

#define MASK (NS7_SHM_RING_FRAMES - 1)

static volatile sig_atomic_t g_stop;
static void on_signal(int s) { (void)s; g_stop = 1; }

static void nap_ms(unsigned ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    time_t t = time(NULL);
    char ts[32];
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&t));
    fprintf(stderr, "%s ns7d: ", ts);
    va_list a;
    va_start(a, fmt);
    vfprintf(stderr, fmt, a);
    va_end(a);
    fputc('\n', stderr);
}

double ns7_host_ticks_per_sec(void)
{
#ifdef __APPLE__
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    return 1e9 * (double)tb.denom / (double)tb.numer;
#else
    return 1e9;
#endif
}

/* ------------------------------------------------------ audio callbacks */

void ns7_serve_audio_init(ns7_serve_audio *a, ns7_shm *shm, double host_ticks_per_sec, uint64_t epoch)
{
    memset(a, 0, sizeof *a);
    a->shm = shm;
    ns7_clock_tracker_init(&a->clock, host_ticks_per_sec, NS7_RATE, epoch);
}

static inline int32_t f2s24(float f)
{
    if (f > 0.99999988f) f = 0.99999988f;     /* (2^23 - 1) / 2^23 */
    if (f < -1.0f) f = -1.0f;
    return (int32_t)(f * 8388608.0f);
}

uint32_t ns7_serve_out_fill(void *ctx, uint8_t *dst, uint32_t frames, uint64_t pos)
{
    ns7_serve_audio *a = ctx;
    ns7_shm *s = a->shm;
    if (!__atomic_load_n(&s->client_running, __ATOMIC_ACQUIRE)) {
        memset(dst, 0, frames * NS7_OUT_FRAME_BYTES);
        return frames;
    }
    for (uint32_t f = 0; f < frames; f++) {
        float *src = &s->out[((pos + f) & MASK) * NS7_SHM_CHANNELS];
        for (unsigned c = 0; c < NS7_OUT_CHANNELS; c++) {
            ns7_s24_put(dst + f * NS7_OUT_FRAME_BYTES + c * 3, f2s24(src[c]));
            src[c] = 0.0f;                    /* never replay a frame CoreAudio didn't refresh */
        }
    }
    return frames;
}

void ns7_serve_in_store(void *ctx, const uint8_t *src, uint32_t frames, uint64_t pos)
{
    ns7_serve_audio *a = ctx;
    ns7_shm *s = a->shm;
    if (!a->in_aligned) {
        /* the newest input frame lines up with the position being sent now */
        a->in_offset = (int64_t)a->last_clock_pos - (int64_t)(pos + frames);
        a->in_aligned = 1;
    }
    for (uint32_t f = 0; f < frames; f++) {
        int64_t t = (int64_t)(pos + f) + a->in_offset;
        if (t < 0) continue;
        float *dst = &s->in[((uint64_t)t & MASK) * NS7_SHM_CHANNELS];
        for (unsigned c = 0; c < NS7_IN_CHANNELS; c++)
            dst[c] = (float)ns7_s24_get(src + (f * NS7_IN_CHANNELS + c) * 3) / 8388608.0f;
    }
    int64_t end = (int64_t)(pos + frames) + a->in_offset;
    if (end > 0) __atomic_store_n(&s->in_written, (uint64_t)end, __ATOMIC_RELEASE);
}

void ns7_serve_clock(void *ctx, uint64_t pos, uint64_t host)
{
    ns7_serve_audio *a = ctx;
    a->last_clock_pos = pos;
    ns7_clock_tracker_update(&a->clock, pos, host);
    ns7_clock_publish(a->shm, &a->clock.out);
}

/* ------------------------------------------------------------ transport */

static size_t shm_bytes(void)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    return (sizeof(ns7_shm) + page - 1) / page * page;
}

#ifdef __APPLE__

static void *g_region;

ns7_shm *ns7_shm_create(void)
{
    size_t size = shm_bytes();
    g_region = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    if (g_region == MAP_FAILED) { say("mmap: %s", strerror(errno)); return NULL; }
    ns7_shm_init(g_region, NS7_RATE);

    dispatch_queue_t q = dispatch_queue_create("ns7d.xpc", DISPATCH_QUEUE_SERIAL);
    xpc_connection_t listener = xpc_connection_create_mach_service(NS7_XPC_SERVICE, q,
                                                                   XPC_CONNECTION_MACH_SERVICE_LISTENER);
    if (!listener) { say("can't register %s", NS7_XPC_SERVICE); return NULL; }
    xpc_connection_set_event_handler(listener, ^(xpc_object_t peer) {
        if (xpc_get_type(peer) != XPC_TYPE_CONNECTION) return;
        xpc_connection_set_event_handler(peer, ^(xpc_object_t msg) {
            if (xpc_get_type(msg) != XPC_TYPE_DICTIONARY) return;
            xpc_object_t reply = xpc_dictionary_create_reply(msg);
            if (!reply) return;
            xpc_object_t mem = xpc_shmem_create(g_region, size);
            if (mem) {
                xpc_dictionary_set_value(reply, "shm", mem);
                xpc_release(mem);
            }
            xpc_connection_send_message(peer, reply);
            xpc_release(reply);
        });
        xpc_connection_resume(peer);
    });
    xpc_connection_resume(listener);
    return g_region;
}

ns7_shm *ns7_shm_connect(int quiet)
{
    xpc_connection_t c = xpc_connection_create_mach_service(NS7_XPC_SERVICE, NULL, 0);
    if (!c) return NULL;
    xpc_connection_set_event_handler(c, ^(xpc_object_t e) { (void)e; });
    xpc_connection_resume(c);
    xpc_object_t msg = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_string(msg, "op", "hello");
    xpc_object_t reply = xpc_connection_send_message_with_reply_sync(c, msg);
    xpc_release(msg);
    ns7_shm *s = NULL;
    if (xpc_get_type(reply) == XPC_TYPE_DICTIONARY) {
        xpc_object_t mem = xpc_dictionary_get_value(reply, "shm");
        void *addr = NULL;
        size_t size = mem ? xpc_shmem_map(mem, &addr) : 0;
        if (size >= sizeof(ns7_shm) && ns7_shm_valid(addr)) s = addr;
        else if (!quiet) fprintf(stderr, "ns7d: shared memory from %s is not usable\n", NS7_XPC_SERVICE);
    } else if (!quiet) {
        fprintf(stderr, "ns7d: the driver service (%s) isn't running\n", NS7_XPC_SERVICE);
    }
    xpc_release(reply);
    if (!s) { xpc_connection_cancel(c); xpc_release(c); }
    return s;                                  /* the connection stays open with the mapping */
}

#else  /* POSIX shared memory, for tests on other systems */

ns7_shm *ns7_shm_create(void)
{
    int fd = shm_open(NS7_POSIX_SHM, O_CREAT | O_RDWR, 0600);
    if (fd < 0) { say("shm_open: %s", strerror(errno)); return NULL; }
    size_t size = shm_bytes();
    if (ftruncate(fd, (off_t)size)) { close(fd); return NULL; }
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return NULL;
    ns7_shm_init(p, NS7_RATE);
    return p;
}

ns7_shm *ns7_shm_connect(int quiet)
{
    int fd = shm_open(NS7_POSIX_SHM, O_RDWR, 0600);
    if (fd < 0) { if (!quiet) fprintf(stderr, "ns7d: driver service not running\n"); return NULL; }
    void *p = mmap(NULL, shm_bytes(), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED || !ns7_shm_valid(p)) return NULL;
    return p;
}

#endif

/* ----------------------------------------------------------------- serve */

static void midi_to_shm(void *ctx, const uint8_t *bytes, uint32_t n)
{
    ns7_shm *s = ctx;
    ns7_ring_write((ns7_ring *)&s->midi_from_device, bytes, n);
}

int ns7_serve_main(int verbose)
{
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    ns7_shm *shm = ns7_shm_create();
    if (!shm) return 1;
    say("service %s ready, waiting for the NS7", NS7_XPC_SERVICE);

    double ticks = ns7_host_ticks_per_sec();
    uint64_t epoch = 0;
    int waiting_said = 1;

    while (!g_stop) {
        if (!ns7_present()) {
            if (!waiting_said) { say("waiting for the NS7"); waiting_said = 1; }
            nap_ms(1000);
            continue;
        }
        waiting_said = 0;
        say("NS7 found, setting up");
        ns7_dev *d = ns7_open(verbose);
        if (!d) { nap_ms(2000); continue; }
        if (ns7_handshake(d)) { say("setup failed, retrying"); ns7_close(d); nap_ms(2000); continue; }

        ns7_serve_audio audio;
        ns7_serve_audio_init(&audio, shm, ticks, ++epoch);
        memset(shm->out, 0, sizeof shm->out);
        memset(shm->in, 0, sizeof shm->in);
        __atomic_store_n(&shm->in_written, 0, __ATOMIC_RELEASE);

        ns7_io io = { ns7_serve_out_fill, ns7_serve_in_store, ns7_serve_clock, &audio,
                      (ns7_ring *)&shm->midi_to_device, midi_to_shm, shm };
        if (ns7_start(d, &io)) say("warning: some transfers failed to start");

        uint32_t xfer = ns7_out_transfer_frames(d);
        shm->out_safety = 2 * xfer + 32;       /* one transfer of lead + one of completion jitter */
        shm->out_latency = ns7_frames_in_flight(d);
        shm->in_safety = 256;                  /* PCM IN transfers are 80 frames */
        shm->in_latency = 80;
        __atomic_store_n(&shm->device_present, 1, __ATOMIC_RELEASE);
        say("streaming at %u Hz (epoch %llu); output latency %u + safety %u frames",
            NS7_RATE, (unsigned long long)epoch, shm->out_latency, shm->out_safety);

        ns7_stats st, prev = { 0 };
        double last_report = 0;
        while (!g_stop) {
            nap_ms(500);
            ns7_get_stats(d, &st);
            if (st.gone) { say("NS7 disconnected"); break; }
            shm->out_underruns = st.out_underruns;
            last_report += 0.5;
            if (verbose && last_report >= 10) {
                say("out %.0f fr/s, in %.0f fr/s, feedback %s, midi in %llu out %llu, errors %llu, rate %.3f Hz",
                    (st.out_frames - prev.out_frames) / last_report, (st.in_frames - prev.in_frames) / last_report,
                    st.feedback_ok ? "ok" : "off", (unsigned long long)st.midi_in_bytes,
                    (unsigned long long)st.midi_out_bytes, (unsigned long long)st.errors,
                    ticks / audio.clock.out.host_per_frame);
                prev = st;
                last_report = 0;
            }
        }
        __atomic_store_n(&shm->device_present, 0, __ATOMIC_RELEASE);
        ns7_stop(d);
        ns7_close(d);
        if (!g_stop) nap_ms(1000);
    }
    say("stopping");
    return 0;
}

/* ----------------------------------------------------------- midi helper */

int ns7_midi_helper_main(void)
{
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    ns7_shm *shm = NULL;
    int said = 0;
    while (!g_stop && !(shm = ns7_shm_connect(1))) {
        if (!said) { fprintf(stderr, "ns7 midi helper: waiting for the driver service\n"); said = 1; }
        nap_ms(1000);
    }
    if (!shm) return 0;

    ns7_ring *from = (ns7_ring *)&shm->midi_from_device;
    __atomic_store_n(&from->tail, __atomic_load_n(&from->head, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);  /* drop backlog */

    ns7_midi *m = ns7_midi_open("NS7", (ns7_ring *)&shm->midi_to_device, 0);
    if (!m) return 1;
    fprintf(stderr, "ns7 midi helper: NS7 MIDI ports live\n");

    uint8_t buf[512];
    while (!g_stop) {
        uint32_t n = ns7_ring_read(from, buf, sizeof buf);
        if (n) ns7_midi_from_device(m, buf, n);
        else nap_ms(1);
    }
    ns7_midi_close(m);
    return 0;
}

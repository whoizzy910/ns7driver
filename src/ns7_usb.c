/*
 * ns7_usb.c - userspace USB engine for the Numark NS7.
 *
 * Pipes (Ploytec isochronous family, alt setting 1 on interfaces 0 and 1):
 *   interface 0: PCM out (isoc OUT), MIDI out (bulk/int OUT), MIDI in (bulk/int IN)
 *   interface 1: PCM in (bulk/int IN), feedback (isoc IN)
 * Everything runs on one libusb event thread at real-time priority, plus a
 * 1 ms MIDI-out thread.
 *
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <libusb.h>

#include "ns7_proto.h"
#include "ns7_usb.h"

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#endif

#define OUT_XFERS        3     /* isoc OUT transfers in flight */
#define FB_XFERS         2
#define IN_XFERS         2
#define IN_FRAMES        80    /* frames per PCM IN transfer (5120 bytes) */
#define MIDI_IN_XFERS    4
#define XFER_TICKS       16    /* 2 ms of USB time per isoc transfer (3 in flight = 6 ms) */
#define CTRL_TIMEOUT_MS  1000

enum { P_PCM_OUT, P_MIDI_OUT, P_MIDI_IN, P_PCM_IN, P_FB, P_COUNT };
static const char *pipe_names[P_COUNT] = { "PCM out", "MIDI out", "MIDI in", "PCM in", "feedback" };

typedef struct {
    uint8_t  addr;
    uint8_t  type;           /* LIBUSB_TRANSFER_TYPE_* */
    uint8_t  interval;
    uint8_t  iface;
    int      max_packet;     /* for isoc: per (micro)frame incl. high-bandwidth mult */
} pipe_info;

struct ns7_dev {
    libusb_context       *ctx;
    libusb_device_handle *h;
    int                   verbose;
    int                   speed;
    int                   claimed[2];
    pipe_info             pipe[P_COUNT];

    /* streaming */
    ns7_io                io;
    ns7_pacer             pacer;
    uint32_t              out_ticks_per_pkt, out_pkts, out_max_frames;
    uint32_t              fb_ticks_per_pkt, fb_pkts;
    struct libusb_transfer *out_x[OUT_XFERS], *fb_x[FB_XFERS], *in_x[IN_XFERS], *mi_x[MIDI_IN_XFERS], *mo_x;
    volatile int          active;        /* transfers in flight */
    volatile int          running, stop_events, midi_busy;
    uint32_t              midi_pattern;
    uint64_t              out_pos, in_pos;   /* frames sent / received since start */
    pthread_t             event_thread, midi_thread;
    int                   threads;
    ns7_stats             st;
};

/* ------------------------------------------------------------------ log */

static void logf_(ns7_dev *d, const char *fmt, ...)
{
    if (d && !d->verbose) return;
    va_list a;
    va_start(a, fmt);
    vfprintf(stderr, fmt, a);
    va_end(a);
    fputc('\n', stderr);
}

static void errf(const char *fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    fputs("ns7: ", stderr);
    vfprintf(stderr, fmt, a);
    va_end(a);
    fputc('\n', stderr);
}

static void sleep_ms(unsigned ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
}

static void rt_priority(double period_ms)
{
#ifdef __APPLE__
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    double per_ms = 1e6 * tb.denom / tb.numer;
    thread_time_constraint_policy_data_t pol = {
        .period = (uint32_t)(period_ms * per_ms),
        .computation = (uint32_t)(0.25 * per_ms),
        .constraint = (uint32_t)(0.5 * per_ms),
        .preemptible = 1,
    };
    thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY,
                      (thread_policy_t)&pol, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
#else
    (void)period_ms;
#endif
}

/* -------------------------------------------------------------- opening */

static const char *type_name(int t)
{
    switch (t) {
    case LIBUSB_TRANSFER_TYPE_ISOCHRONOUS: return "isoc";
    case LIBUSB_TRANSFER_TYPE_BULK: return "bulk";
    case LIBUSB_TRANSFER_TYPE_INTERRUPT: return "interrupt";
    default: return "control";
    }
}

static const char *speed_name(int s)
{
    switch (s) {
    case LIBUSB_SPEED_LOW: return "low speed";
    case LIBUSB_SPEED_FULL: return "full speed (12 Mbit/s)";
    case LIBUSB_SPEED_HIGH: return "high speed (480 Mbit/s)";
    case LIBUSB_SPEED_SUPER: return "super speed";
    default: return "unknown speed";
    }
}

/* Ticks (125 us) between packets of an isoc endpoint. */
static uint32_t iso_ticks(int speed, uint8_t b_interval)
{
    uint32_t n = 1u << ((b_interval ? b_interval : 1) - 1);
    return speed >= LIBUSB_SPEED_HIGH ? n : n * 8;
}

static void find_pipes(ns7_dev *d, const struct libusb_config_descriptor *cd)
{
    for (int i = 0; i < cd->bNumInterfaces; i++) {
        const struct libusb_interface *itf = &cd->interface[i];
        for (int a = 0; a < itf->num_altsetting; a++) {
            const struct libusb_interface_descriptor *id = &itf->altsetting[a];
            if (id->bAlternateSetting != NS7_ALT_SETTING && itf->num_altsetting > 1) continue;
            for (int e = 0; e < id->bNumEndpoints; e++) {
                const struct libusb_endpoint_descriptor *ed = &id->endpoint[e];
                int type = ed->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;
                int in = ed->bEndpointAddress & LIBUSB_ENDPOINT_IN;
                int iso = type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS;
                int role = -1;
                if (iso && !in) role = P_PCM_OUT;
                else if (iso && in) role = P_FB;
                else if (!in) role = P_MIDI_OUT;
                else role = id->bInterfaceNumber == 0 ? P_MIDI_IN : P_PCM_IN;
                if (role < 0 || d->pipe[role].addr) continue;
                pipe_info *p = &d->pipe[role];
                p->addr = ed->bEndpointAddress;
                p->type = (uint8_t)type;
                p->interval = ed->bInterval;
                p->iface = id->bInterfaceNumber;
                p->max_packet = iso ? libusb_get_max_iso_packet_size(libusb_get_device(d->h), ed->bEndpointAddress)
                                    : ed->wMaxPacketSize;
            }
        }
    }
}

int ns7_present(void)
{
    libusb_context *ctx;
    if (libusb_init(&ctx)) return 0;
    libusb_device **list;
    ssize_t n = libusb_get_device_list(ctx, &list);
    int found = 0;
    for (ssize_t i = 0; i < n && !found; i++) {
        struct libusb_device_descriptor dd;
        if (libusb_get_device_descriptor(list[i], &dd) == 0)
            found = dd.idVendor == NS7_VID && dd.idProduct == NS7_PID;
    }
    if (n >= 0) libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return found;
}

ns7_dev *ns7_open(int verbose)
{
    ns7_dev *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->verbose = verbose;
    int r = libusb_init(&d->ctx);
    if (r) { errf("libusb: %s", libusb_strerror(r)); free(d); return NULL; }

    d->h = libusb_open_device_with_vid_pid(d->ctx, NS7_VID, NS7_PID);
    if (!d->h) {
        errf("no Numark NS7 (%04x:%04x) found. Is it plugged in and switched on?", NS7_VID, NS7_PID);
        ns7_close(d);
        return NULL;
    }
    d->speed = libusb_get_device_speed(libusb_get_device(d->h));
    libusb_set_auto_detach_kernel_driver(d->h, 1);

    int cfg = 0;
    if (libusb_get_configuration(d->h, &cfg) == 0 && cfg == 0)
        libusb_set_configuration(d->h, 1);

    for (int i = 0; i < 2; i++) {
        r = libusb_claim_interface(d->h, i);
        if (r) {
            errf("can't claim interface %d: %s%s", i, libusb_strerror(r),
                 r == LIBUSB_ERROR_ACCESS || r == LIBUSB_ERROR_BUSY ? " (another driver has it, or try sudo)" : "");
            ns7_close(d);
            return NULL;
        }
        d->claimed[i] = 1;
        r = libusb_set_interface_alt_setting(d->h, i, NS7_ALT_SETTING);
        if (r) logf_(d, "interface %d: alt setting %d: %s", i, NS7_ALT_SETTING, libusb_strerror(r));
    }

    struct libusb_config_descriptor *cd;
    if (libusb_get_active_config_descriptor(libusb_get_device(d->h), &cd) == 0) {
        find_pipes(d, cd);
        libusb_free_config_descriptor(cd);
    }
    for (int p = 0; p < P_COUNT; p++)
        if (!d->pipe[p].addr) errf("warning: no %s pipe found", pipe_names[p]);
    return d;
}

void ns7_print_info(ns7_dev *d)
{
    libusb_device *dev = libusb_get_device(d->h);
    struct libusb_device_descriptor dd;
    libusb_get_device_descriptor(dev, &dd);
    unsigned char s[128] = "";
    printf("Device   %04x:%04x, USB %x.%02x, %s\n", dd.idVendor, dd.idProduct,
           dd.bcdUSB >> 8, dd.bcdUSB & 0xff, speed_name(d->speed));
    if (dd.iProduct && libusb_get_string_descriptor_ascii(d->h, dd.iProduct, s, sizeof s) > 0)
        printf("Product  %s\n", s);

    struct libusb_config_descriptor *cd;
    if (libusb_get_active_config_descriptor(dev, &cd) == 0) {
        for (int i = 0; i < cd->bNumInterfaces; i++) {
            const struct libusb_interface *itf = &cd->interface[i];
            for (int a = 0; a < itf->num_altsetting; a++) {
                const struct libusb_interface_descriptor *id = &itf->altsetting[a];
                printf("  interface %d alt %d  class 0x%02x/0x%02x\n", id->bInterfaceNumber,
                       id->bAlternateSetting, id->bInterfaceClass, id->bInterfaceSubClass);
                for (int e = 0; e < id->bNumEndpoints; e++) {
                    const struct libusb_endpoint_descriptor *ed = &id->endpoint[e];
                    printf("    EP 0x%02x %s %-9s maxpkt %4u interval %u\n", ed->bEndpointAddress,
                           ed->bEndpointAddress & 0x80 ? "IN " : "OUT",
                           type_name(ed->bmAttributes & 3), ed->wMaxPacketSize, ed->bInterval);
                }
            }
        }
        libusb_free_config_descriptor(cd);
    }
    printf("Pipes:\n");
    for (int p = 0; p < P_COUNT; p++) {
        if (d->pipe[p].addr)
            printf("  %-9s EP 0x%02x (%s, interface %u, %d bytes)\n", pipe_names[p], d->pipe[p].addr,
                   type_name(d->pipe[p].type), d->pipe[p].iface, d->pipe[p].max_packet);
        else
            printf("  %-9s not found\n", pipe_names[p]);
    }
}

/* ------------------------------------------------------------ handshake */

static int ctrl(ns7_dev *d, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx, uint8_t *data, uint16_t len,
                const char *what)
{
    int r = libusb_control_transfer(d->h, rt, req, val, idx, data, len, CTRL_TIMEOUT_MS);
    if (r < 0) errf("%s failed: %s", what, libusb_strerror(r));
    return r;
}

static int read_status(ns7_dev *d, uint8_t *b)
{
    int r = ctrl(d, NS7_RT_VENDOR_IN, NS7_REQ_STATUS, 0, 0, b, 1, "status read");
    return r < 0 ? r : 0;
}

static int read_rate(ns7_dev *d, uint32_t *rate)
{
    uint8_t b[3] = { 0 };
    int r = libusb_control_transfer(d->h, NS7_RT_CLASS_EP_IN, NS7_REQ_GET_CUR, NS7_RATE_SELECTOR, 0, b, 3, CTRL_TIMEOUT_MS);
    if (r < 0)   /* some firmwares want the endpoint in wIndex, as UAC1 specifies */
        r = ctrl(d, NS7_RT_CLASS_EP_IN, NS7_REQ_GET_CUR, NS7_RATE_SELECTOR, d->pipe[P_PCM_IN].addr, b, 3, "rate read");
    if (r < 0) return r;
    *rate = ns7_decode_rate(b);
    return 0;
}

int ns7_handshake(ns7_dev *d)
{
    uint8_t v[15] = { 0 }, st = 0, b[3];
    uint32_t rate = 0;
    unsigned chip, maj, min;

    if (!d->pipe[P_PCM_IN].addr || !d->pipe[P_PCM_OUT].addr) {
        errf("audio pipes missing; can't set up streaming");
        return -1;
    }

    /* 1. firmware */
    if (ctrl(d, NS7_RT_VENDOR_IN, NS7_REQ_FIRMWARE, 0, 0, v, sizeof v, "firmware read") < 0) return -1;
    ns7_firmware_version(v, &chip, &maj, &min);
    printf("Firmware v1.%u.%u (chip 0x%02x)\n", maj, min, chip);

    /* 2. the NS7 wants 200 ms before configuration (kMsWait) */
    sleep_ms(NS7_SETUP_WAIT_MS);

    /* 3. status and current rate */
    if (read_status(d, &st)) return -1;
    if (read_rate(d, &rate)) return -1;
    logf_(d, "status 0x%02x, rate at power-up %u Hz", st, rate);

    /* 4. clock source: automatic (selector bit 1), written only if it changes */
    sleep_ms(5);
    if (read_status(d, &st)) return -1;
    if (st & 0x04) { sleep_ms(20); if (read_status(d, &st)) return -1; }
    uint8_t want = st | NS7_STATUS_CLOCK_AUTO;
    if (want != st) {
        if (ctrl(d, NS7_RT_VENDOR_OUT, NS7_REQ_STATUS, ns7_byte_wvalue(want), 0, NULL, 0, "clock select") < 0) return -1;
        sleep_ms(40);
        read_rate(d, &rate);
        logf_(d, "clock: status 0x%02x -> 0x%02x", st, want);
    }

    /* 5. sample rate on the PCM IN, then the PCM OUT endpoint */
    ns7_encode_rate(NS7_RATE, b);
    if (ctrl(d, NS7_RT_CLASS_EP_OUT, NS7_REQ_SET_CUR, NS7_RATE_SELECTOR, d->pipe[P_PCM_IN].addr, b, 3, "set rate (in)") < 0) return -1;
    ns7_encode_rate(NS7_RATE, b);
    if (ctrl(d, NS7_RT_CLASS_EP_OUT, NS7_REQ_SET_CUR, NS7_RATE_SELECTOR, d->pipe[P_PCM_OUT].addr, b, 3, "set rate (out)") < 0) return -1;

    /* 6. verify */
    if (read_rate(d, &rate)) return -1;
    if (rate != NS7_RATE) { errf("device reports %u Hz instead of %u Hz", rate, NS7_RATE); return -1; }
    sleep_ms(50);

    /* 7. confirm: write the status back with bits 5 and 4 set, which arms streaming */
    if (read_status(d, &st)) return -1;
    if (ctrl(d, NS7_RT_VENDOR_OUT, NS7_REQ_STATUS, ns7_confirm_wvalue(st), 0, NULL, 0, "status confirm") < 0) return -1;
    logf_(d, "armed: status 0x%02x, wValue 0x%04x", st, ns7_confirm_wvalue(st));
    printf("Set up for %u Hz, %d in / %d out\n", NS7_RATE, NS7_IN_CHANNELS, NS7_OUT_CHANNELS);
    return 0;
}

/* ------------------------------------------------------------ transfers */

static int submit(ns7_dev *d, struct libusb_transfer *x)
{
    int r = libusb_submit_transfer(x);
    if (r == 0) {
        __atomic_add_fetch(&d->active, 1, __ATOMIC_ACQ_REL);
    } else {
        d->st.errors++;
        if (r == LIBUSB_ERROR_NO_DEVICE) d->st.gone = 1;
        if (d->st.errors < 10) errf("submit on EP 0x%02x: %s", x->endpoint, libusb_strerror(r));
    }
    return r;
}

static int done(ns7_dev *d, struct libusb_transfer *x)
{
    __atomic_sub_fetch(&d->active, 1, __ATOMIC_ACQ_REL);
    if (x->status == LIBUSB_TRANSFER_NO_DEVICE) { d->st.gone = 1; d->running = 0; }
    return d->running && x->status != LIBUSB_TRANSFER_CANCELLED;
}

static void fill_out(ns7_dev *d, struct libusb_transfer *x)
{
    uint8_t *p = x->buffer;
    if (d->io.clock) d->io.clock(d->io.audio_ctx, d->out_pos, ns7_host_time());
    for (int i = 0; i < x->num_iso_packets; i++) {
        uint32_t frames = ns7_pacer_frames(&d->pacer, d->out_ticks_per_pkt);
        if (frames > d->out_max_frames) frames = d->out_max_frames;
        uint32_t bytes = frames * NS7_OUT_FRAME_BYTES, real = 0;
        if (d->io.out_fill) {
            real = d->io.out_fill(d->io.audio_ctx, p, frames, d->out_pos);
            if (real < frames) d->st.out_underruns += frames - real;
        } else {
            memset(p, 0, bytes);
        }
        x->iso_packet_desc[i].length = bytes;
        d->out_pos += frames;
        d->st.out_frames += frames;
        p += bytes;                    /* isoc OUT packets are contiguous in the buffer */
    }
}

static void LIBUSB_CALL out_cb(struct libusb_transfer *x)
{
    ns7_dev *d = x->user_data;
    if (!done(d, x)) return;
    if (x->status != LIBUSB_TRANSFER_COMPLETED) d->st.errors++;
    fill_out(d, x);
    submit(d, x);
}

static void LIBUSB_CALL fb_cb(struct libusb_transfer *x)
{
    ns7_dev *d = x->user_data;
    if (!done(d, x)) return;
    for (int i = 0; i < x->num_iso_packets; i++) {
        struct libusb_iso_packet_descriptor *pd = &x->iso_packet_desc[i];
        uint8_t *pkt = libusb_get_iso_packet_buffer_simple(x, i);
        uint32_t len = pd->status == LIBUSB_TRANSFER_COMPLETED ? pd->actual_length : 0;
        ns7_pacer_feedback(&d->pacer, pkt, len);
        d->st.fb_packets++;
        if (len != NS7_FB_PACKET) d->st.fb_bad++;
    }
    d->st.feedback_ok = !d->pacer.broken && d->st.fb_packets > d->st.fb_bad;
    submit(d, x);
}

static void LIBUSB_CALL in_cb(struct libusb_transfer *x)
{
    ns7_dev *d = x->user_data;
    if (!done(d, x)) return;
    if (x->status == LIBUSB_TRANSFER_COMPLETED) {
        uint32_t frames = (uint32_t)x->actual_length / NS7_IN_FRAME_BYTES;
        uint8_t s24[IN_FRAMES * NS7_IN_CHANNELS * 3];
        if (frames > IN_FRAMES) frames = IN_FRAMES;
        for (uint32_t f = 0; f < frames; f++)
            ns7_decode_in_frame(x->buffer + f * NS7_IN_FRAME_BYTES, s24 + f * NS7_IN_CHANNELS * 3, NS7_IN_CHANNELS);
        if (d->io.in_store && frames) d->io.in_store(d->io.audio_ctx, s24, frames, d->in_pos);
        d->in_pos += frames;
        d->st.in_frames += frames;
    } else {
        d->st.errors++;
    }
    submit(d, x);
}

static void LIBUSB_CALL midi_in_cb(struct libusb_transfer *x)
{
    ns7_dev *d = x->user_data;
    if (!done(d, x)) return;
    if (x->status == LIBUSB_TRANSFER_COMPLETED && x->actual_length > 0) {
        uint8_t bytes[NS7_MIDI_IN_BYTES];
        unsigned n = ns7_midi_in_unpack(x->buffer, (unsigned)x->actual_length, bytes);
        if (n) {
            d->st.midi_in_bytes += n;
            if (d->io.midi_in) d->io.midi_in(d->io.midi_ctx, bytes, n);
        }
    }
    submit(d, x);
}

static void LIBUSB_CALL midi_out_cb(struct libusb_transfer *x)
{
    ns7_dev *d = x->user_data;
    done(d, x);
    __atomic_store_n(&d->midi_busy, 0, __ATOMIC_RELEASE);
}

static void *midi_thread(void *arg)
{
    ns7_dev *d = arg;
    while (d->running) {
        if (!__atomic_exchange_n(&d->midi_busy, 1, __ATOMIC_ACQ_REL)) {
            uint8_t *buf = d->mo_x->buffer;
            unsigned n = ns7_midi_out_begin(buf, &d->midi_pattern);
            if (d->io.midi_out) d->st.midi_out_bytes += ns7_ring_read(d->io.midi_out, buf, n);
            if (submit(d, d->mo_x)) __atomic_store_n(&d->midi_busy, 0, __ATOMIC_RELEASE);
        }
        sleep_ms(1);
    }
    return NULL;
}

static void *event_thread(void *arg)
{
    ns7_dev *d = arg;
    rt_priority(1.0);
    struct timeval tv = { 0, 100000 };
    while (!d->stop_events)
        libusb_handle_events_timeout_completed(d->ctx, &tv, (int *)&d->stop_events);
    return NULL;
}

static struct libusb_transfer *make(ns7_dev *d, int pipe, int packets, int length, libusb_transfer_cb_fn cb)
{
    struct libusb_transfer *x = libusb_alloc_transfer(packets);
    uint8_t *buf = calloc(1, (size_t)length);
    if (!x || !buf) { libusb_free_transfer(x); free(buf); return NULL; }
    pipe_info *p = &d->pipe[pipe];
    switch (p->type) {
    case LIBUSB_TRANSFER_TYPE_ISOCHRONOUS:
        libusb_fill_iso_transfer(x, d->h, p->addr, buf, length, packets, cb, d, 0);
        libusb_set_iso_packet_lengths(x, (unsigned)(length / packets));
        break;
    case LIBUSB_TRANSFER_TYPE_INTERRUPT:
        libusb_fill_interrupt_transfer(x, d->h, p->addr, buf, length, cb, d, 0);
        break;
    default:
        libusb_fill_bulk_transfer(x, d->h, p->addr, buf, length, cb, d, 0);
        break;
    }
    x->flags |= LIBUSB_TRANSFER_FREE_BUFFER;
    return x;
}

uint64_t ns7_host_time(void)
{
#ifdef __APPLE__
    return mach_absolute_time();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

uint32_t ns7_out_transfer_frames(ns7_dev *d)
{
    return (NS7_RATE * d->out_ticks_per_pkt * d->out_pkts + NS7_TICKS_PER_SEC - 1) / NS7_TICKS_PER_SEC;
}

uint32_t ns7_frames_in_flight(ns7_dev *d)
{
    return OUT_XFERS * ns7_out_transfer_frames(d);
}

int ns7_start(ns7_dev *d, const ns7_io *io)
{
    d->io = *io;
    memset(&d->st, 0, sizeof d->st);
    d->out_pos = d->in_pos = 0;

    pipe_info *po = &d->pipe[P_PCM_OUT], *pf = &d->pipe[P_FB];
    d->out_ticks_per_pkt = iso_ticks(d->speed, po->interval);
    d->out_pkts = XFER_TICKS / d->out_ticks_per_pkt ? XFER_TICKS / d->out_ticks_per_pkt : 1;
    d->out_max_frames = (uint32_t)po->max_packet / NS7_OUT_FRAME_BYTES;
    uint32_t nominal_max = (NS7_RATE * d->out_ticks_per_pkt + NS7_TICKS_PER_SEC - 1) / NS7_TICKS_PER_SEC + 1;
    if (d->out_max_frames < nominal_max)
        errf("warning: PCM out packets hold %u frames, %u needed", d->out_max_frames, nominal_max);

    d->fb_ticks_per_pkt = pf->addr ? iso_ticks(d->speed, pf->interval) : 0;
    d->fb_pkts = d->fb_ticks_per_pkt && XFER_TICKS / d->fb_ticks_per_pkt ? XFER_TICKS / d->fb_ticks_per_pkt : 1;
    ns7_pacer_init(&d->pacer, d->fb_ticks_per_pkt);
    if (!pf->addr) d->pacer.broken = true;

    logf_(d, "PCM out: %u ticks/packet, %u packets/transfer, up to %u frames/packet",
          d->out_ticks_per_pkt, d->out_pkts, d->out_max_frames);
    logf_(d, "feedback: %u ticks/packet, %u packets/transfer", d->fb_ticks_per_pkt, d->fb_pkts);

    for (int i = 0; i < OUT_XFERS; i++)
        d->out_x[i] = make(d, P_PCM_OUT, (int)d->out_pkts, (int)(d->out_pkts * d->out_max_frames * NS7_OUT_FRAME_BYTES), out_cb);
    if (pf->addr)
        for (int i = 0; i < FB_XFERS; i++)
            d->fb_x[i] = make(d, P_FB, (int)d->fb_pkts, (int)d->fb_pkts * (pf->max_packet > 0 ? pf->max_packet : NS7_FB_PACKET), fb_cb);
    if (d->pipe[P_PCM_IN].addr)
        for (int i = 0; i < IN_XFERS; i++)
            d->in_x[i] = make(d, P_PCM_IN, 0, IN_FRAMES * NS7_IN_FRAME_BYTES, in_cb);
    if (d->pipe[P_MIDI_IN].addr)
        for (int i = 0; i < MIDI_IN_XFERS; i++)
            d->mi_x[i] = make(d, P_MIDI_IN, 0, NS7_MIDI_XFER, midi_in_cb);
    if (d->pipe[P_MIDI_OUT].addr)
        d->mo_x = make(d, P_MIDI_OUT, 0, NS7_MIDI_XFER, midi_out_cb);

    d->running = 1;
    d->stop_events = 0;
    if (pthread_create(&d->event_thread, NULL, event_thread, d)) { errf("can't start USB thread"); return -1; }
    d->threads = 1;

    for (int i = 0; i < IN_XFERS; i++) if (d->in_x[i]) submit(d, d->in_x[i]);
    for (int i = 0; i < FB_XFERS; i++) if (d->fb_x[i]) submit(d, d->fb_x[i]);
    /* fill all OUT transfers before submitting any: after that only the USB thread touches the pacer */
    for (int i = 0; i < OUT_XFERS; i++) if (d->out_x[i]) fill_out(d, d->out_x[i]);
    for (int i = 0; i < OUT_XFERS; i++) if (d->out_x[i]) submit(d, d->out_x[i]);
    for (int i = 0; i < MIDI_IN_XFERS; i++) if (d->mi_x[i]) submit(d, d->mi_x[i]);
    if (d->mo_x && !pthread_create(&d->midi_thread, NULL, midi_thread, d)) d->threads = 2;

    return d->st.errors ? -1 : 0;
}

void ns7_get_stats(ns7_dev *d, ns7_stats *s)
{
    *s = d->st;
}

static void cancel_all(ns7_dev *d)
{
    struct libusb_transfer **all[] = { d->out_x, d->fb_x, d->in_x, d->mi_x };
    int counts[] = { OUT_XFERS, FB_XFERS, IN_XFERS, MIDI_IN_XFERS };
    for (int g = 0; g < 4; g++)
        for (int i = 0; i < counts[g]; i++)
            if (all[g][i]) libusb_cancel_transfer(all[g][i]);
    if (d->mo_x) libusb_cancel_transfer(d->mo_x);
}

static void free_all(ns7_dev *d)
{
    struct libusb_transfer **all[] = { d->out_x, d->fb_x, d->in_x, d->mi_x };
    int counts[] = { OUT_XFERS, FB_XFERS, IN_XFERS, MIDI_IN_XFERS };
    for (int g = 0; g < 4; g++)
        for (int i = 0; i < counts[g]; i++) { libusb_free_transfer(all[g][i]); all[g][i] = NULL; }
    libusb_free_transfer(d->mo_x);
    d->mo_x = NULL;
}

void ns7_stop(ns7_dev *d)
{
    if (!d->threads) return;
    d->running = 0;
    if (d->threads == 2) pthread_join(d->midi_thread, NULL);
    cancel_all(d);
    for (int i = 0; i < 200 && __atomic_load_n(&d->active, __ATOMIC_ACQUIRE) > 0; i++)
        sleep_ms(5);                           /* the event thread completes the cancellations */
    d->stop_events = 1;
    pthread_join(d->event_thread, NULL);
    d->threads = 0;
    if (__atomic_load_n(&d->active, __ATOMIC_ACQUIRE) == 0) free_all(d);
    else errf("warning: %d transfers did not finish", d->active);
}

void ns7_close(ns7_dev *d)
{
    if (!d) return;
    ns7_stop(d);
    if (d->h) {
        for (int i = 0; i < 2; i++)
            if (d->claimed[i]) {
                libusb_set_interface_alt_setting(d->h, i, 0);
                libusb_release_interface(d->h, i);
            }
        libusb_close(d->h);
    }
    if (d->ctx) libusb_exit(d->ctx);
    free(d);
}

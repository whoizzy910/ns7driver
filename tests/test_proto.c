/*
 * test_proto.c - unit tests for the NS7 protocol pieces that can be checked
 * without the hardware. Build with OZZY=/path/to/Ozzy to also cross-check the
 * input decoder against Ozzy's independently generated 4-channel decoder.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/ns7_proto.h"
#include "../src/ns7_ring.h"
#include "../src/ns7_midi.h"
#include "../src/ns7_wav.h"

#ifdef HAVE_OZZY
void ploytec_decode_frame_4ch(uint8_t *dest, const uint8_t *src);
#endif

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rng = 12345;
static uint8_t rnd(void) { rng = rng * 1103515245 + 12345; return (uint8_t)(rng >> 16); }

static void test_frames(void)
{
    for (int t = 0; t < 2000; t++) {
        uint8_t s24[12], dev[64], back[12];
        for (int i = 0; i < 12; i++) s24[i] = rnd();
        ns7_encode_in_frame(s24, dev, 4);
        ns7_decode_in_frame(dev, back, 4);
        CHECK(!memcmp(s24, back, 12), "input frame round trip %d", t);
    }
    /* channel 1 sample 0x800001: MSB in byte 0 bit 0, LSB in byte 0x17 bit 0 */
    uint8_t s[12] = { 0x01, 0x00, 0x80 }, dev[64];
    ns7_encode_in_frame(s, dev, 4);
    CHECK(dev[0x00] == 0x01 && dev[0x17] == 0x01 && dev[0x01] == 0, "bit placement ch1");
    uint8_t s3[12] = { 0 }; s3[6] = 0x01;           /* channel 3 LSB -> lane 1 of byte 0x17 */
    ns7_encode_in_frame(s3, dev, 4);
    CHECK(dev[0x17] == 0x02, "bit placement ch3 (got %02x)", dev[0x17]);
    uint8_t s2[12] = { 0 }; s2[3] = 0x01;           /* channel 2 LSB -> byte 0x37 */
    ns7_encode_in_frame(s2, dev, 4);
    CHECK(dev[0x37] == 0x01, "bit placement ch2");

#ifdef HAVE_OZZY
    for (int t = 0; t < 5000; t++) {
        uint8_t dev2[64], a[12], b[12];
        for (int i = 0; i < 64; i++) dev2[i] = rnd();
        ns7_decode_in_frame(dev2, a, 4);
        ploytec_decode_frame_4ch(b, dev2);
        CHECK(!memcmp(a, b, 12), "decoder differs from Ozzy's on random frame %d", t);
        if (memcmp(a, b, 12)) break;
    }
    printf("cross-checked input decoder against Ozzy's ploytec_decode_frame_4ch\n");
#endif
}

static void test_control(void)
{
    uint8_t b[3];
    ns7_encode_rate(44100, b);
    CHECK(b[0] == 0x44 && b[1] == 0xAC && b[2] == 0x00, "rate encode");
    CHECK(ns7_decode_rate(b) == 44100, "rate decode");
    CHECK(ns7_confirm_wvalue(0x02) == 0x0032, "confirm 0x02");
    CHECK(ns7_confirm_wvalue(0x9A) == 0xFFBA, "confirm sign extension (got %04x)", ns7_confirm_wvalue(0x9A));
    uint8_t v[15] = { 0x31, 0, 41 };
    unsigned chip, maj, min;
    ns7_firmware_version(v, &chip, &maj, &min);
    CHECK(chip == 0x31 && maj == 4 && min == 1, "firmware decode");
}

static void test_midi_framing(void)
{
    uint8_t x[42], out[41];
    memset(x, 0xFD, 42);
    x[41] = 0xFF;
    x[0] = 0xB0; x[1] = 0x07; x[5] = 0x40;
    CHECK(ns7_midi_in_unpack(x, 42, out) == 3 && out[0] == 0xB0 && out[2] == 0x40, "MIDI in unpack");
    x[41] = 0x90;                                   /* control byte must be ignored */
    CHECK(ns7_midi_in_unpack(x, 42, out) == 3, "control byte dropped");

    uint32_t idx = 0, total = 0;
    for (int i = 0; i < 8000; i++) {
        unsigned n = ns7_midi_out_begin(x, &idx);
        total += n;
        CHECK(x[41] == NS7_MIDI_OUT_CTRL && x[40] == NS7_MIDI_IDLE, "out transfer framing");
    }
    CHECK(total == 25000, "MIDI out pace: %u bytes in 8 s (want 3125/s)", total);
}

typedef struct { uint8_t msgs[64][8]; uint32_t lens[64]; int n; } sink;
static void collect(void *ctx, const uint8_t *m, uint32_t len)
{
    sink *s = ctx;
    if (s->n < 64) { memcpy(s->msgs[s->n], m, len < 8 ? len : 8); s->lens[s->n++] = len; }
}

static void test_midi_parser(void)
{
    ns7_midi_parser p;
    sink s = { 0 };
    ns7_midi_parser_init(&p);
    /* note on, running status note, real-time clock in the middle, CC, program change */
    const uint8_t in[] = { 0x90, 0x3C, 0x7F, 0x3E, 0xF8, 0x40, 0xB1, 0x07, 0x64, 0xC2, 0x05, 0x06 };
    ns7_midi_parse(&p, in, sizeof in, collect, &s);
    CHECK(s.n == 6, "parser message count %d", s.n);
    CHECK(s.lens[0] == 3 && s.msgs[0][0] == 0x90 && s.msgs[0][1] == 0x3C, "note on");
    CHECK(s.lens[1] == 1 && s.msgs[1][0] == 0xF8, "real-time passes through");
    CHECK(s.lens[2] == 3 && s.msgs[2][0] == 0x90 && s.msgs[2][1] == 0x3E && s.msgs[2][2] == 0x40, "running status across real-time");
    CHECK(s.lens[3] == 3 && s.msgs[3][0] == 0xB1, "CC");
    CHECK(s.lens[4] == 2 && s.msgs[4][1] == 0x05 && s.lens[5] == 2 && s.msgs[5][1] == 0x06, "program change + running status");

    /* split across calls, as USB transfers will split it */
    sink s2 = { 0 };
    ns7_midi_parser_init(&p);
    const uint8_t a[] = { 0xE0, 0x00 }, b2[] = { 0x40 };
    ns7_midi_parse(&p, a, 2, collect, &s2);
    ns7_midi_parse(&p, b2, 1, collect, &s2);
    CHECK(s2.n == 1 && s2.lens[0] == 3 && s2.msgs[0][2] == 0x40, "pitch bend split across transfers");

    sink s3 = { 0 };
    ns7_midi_parser_init(&p);
    const uint8_t sx[] = { 0xF0, 0x00, 0x20, 0x33, 0xF7, 0x80, 0x10, 0x00 };
    ns7_midi_parse(&p, sx, sizeof sx, collect, &s3);
    CHECK(s3.n == 2 && s3.lens[0] == 5 && s3.msgs[0][4] == 0xF7 && s3.lens[1] == 3, "SysEx then note off");
}

static void test_pacer(void)
{
    ns7_pacer p;
    /* no feedback: nominal rate exact over one second of 1 ms packets */
    ns7_pacer_init(&p, 8);
    uint32_t total = 0;
    for (int i = 0; i < 1000; i++) total += ns7_pacer_frames(&p, 8);
    CHECK(total == 44100, "nominal rate: %u frames/s", total);

    /* feedback says the device plays slightly fast: 45 frames every ms */
    ns7_pacer_init(&p, 8);
    total = 0;
    uint8_t pkt[3] = { 45, 0, 0 };
    for (int i = 0; i < 64; i++) ns7_pacer_feedback(&p, pkt, 3);   /* fill the ring */
    for (int i = 0; i < 1000; i++) { ns7_pacer_feedback(&p, pkt, 3); total += ns7_pacer_frames(&p, 8); }
    CHECK(total == 45000, "follows feedback: %u frames/s", total);

    /* High Speed: feedback per microframe-group of 8, output per microframe */
    ns7_pacer_init(&p, 8);
    total = 0;
    pkt[0] = 44;
    for (int i = 0; i < 1000; i++) {
        ns7_pacer_feedback(&p, pkt, 3);
        for (int k = 0; k < 8; k++) total += ns7_pacer_frames(&p, 1);
    }
    CHECK(total >= 43990 && total <= 44010, "per-microframe output: %u", total);

    /* garbage feedback is rejected and the nominal rate is used */
    ns7_pacer_init(&p, 8);
    total = 0;
    pkt[0] = 200;
    for (int i = 0; i < 1000; i++) { ns7_pacer_feedback(&p, pkt, 3); total += ns7_pacer_frames(&p, 8); }
    CHECK(total == 44100, "out-of-range feedback ignored: %u", total);
}

static void test_ring(void)
{
    ns7_ring *r = ns7_ring_new(64);
    uint8_t in[1000], out[200];
    uint32_t w = 0, rd = 0;
    for (int round = 0; round < 50; round++) {
        uint8_t chunk[37];
        for (int i = 0; i < 37; i++) chunk[i] = (uint8_t)(w + i);   /* byte value = stream position */
        w += ns7_ring_write(r, chunk, 37);
        uint32_t got = ns7_ring_read(r, out, 23);
        for (uint32_t i = 0; i < got; i++) CHECK(out[i] == (uint8_t)(rd + i), "ring order at %u", rd + i);
        rd += got;
    }
    CHECK(w - rd == ns7_ring_used(r), "ring fill level");
    memset(in, 0, sizeof in);
    CHECK(ns7_ring_write(r, in, 1000) <= 64, "ring never overfills");
    free(r);
}

static void test_wav(void)
{
    const char *path = "test_tmp.wav";
    ns7_wav w;
    uint8_t frames[10 * 12], back[10 * 12];
    for (int i = 0; i < (int)sizeof frames; i++) frames[i] = rnd();
    CHECK(ns7_wav_open_write(&w, path, 44100, 4) == 0, "wav create");
    ns7_wav_write_s24(&w, frames, 10);
    ns7_wav_close(&w);
    CHECK(ns7_wav_open_read(&w, path) == 0 && w.frames == 10 && w.channels == 4 && w.bits == 24, "wav reopen");
    CHECK(ns7_wav_read_s24(&w, back, 10, 4, 0) == 10 && !memcmp(frames, back, sizeof back), "wav round trip");
    ns7_wav_close(&w);
    remove(path);
}

int main(void)
{
    test_frames();
    test_control();
    test_midi_framing();
    test_midi_parser();
    test_pacer();
    test_ring();
    test_wav();
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}

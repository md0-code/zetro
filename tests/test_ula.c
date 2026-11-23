/* The sound and video chips' unit check: the beeper, the AY-3-8912 and the
 * ULA against a fake host (a clock, a deadline and the pins it is told
 * about), a handful of checks each. No SDL, no window. */
#include "ay8912.h"
#include "beeper.h"
#include "machine.h"
#include "state.h"
#include "ula.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(what) do { if (!(what)) { printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #what); ++failures; } } while (0)

#define CLOCK 14000000u
#define RATE 48000u

/* --- the fake host ----------------------------------------------------------------------- */

static struct {
    uint64_t now, deadline;
    int lines;
    uint64_t line_time[8];
    int line_state[8];
} fake;

static uint64_t f_now(void *ctx) { (void)ctx; return fake.now; }
static void f_schedule(void *ctx, void *dev, uint64_t when) { (void)ctx; (void)dev; fake.deadline = when; }
static void f_line(void *ctx, void *dev, int pin, int state) {
    (void)ctx; (void)dev; (void)pin;
    if (fake.lines < 8) { fake.line_time[fake.lines] = fake.now; fake.line_state[fake.lines] = state; }
    ++fake.lines;
}
static const zr_hw_host HOST = {NULL, f_now, f_schedule, f_line, NULL};

/* A chip's state out of one and into another. */
static int transfer(const zr_dev *from, zr_dev *to) {
    zr_state_writer w;
    zr_state_reader r;
    char error[128];
    zr_sw_init(&w);
    zr_sw_header(&w, "test", "test", NULL, 0);
    zr_sw_begin(&w, "TEST", 0);
    from->ops->save(from, &w);
    zr_sw_end(&w);
    int ok = zr_sw_finish(&w) && zr_sr_open(&r, w.data, w.size, error, sizeof error) && zr_sr_chunk(&r, "TEST", 0) && to->ops->load(to, &r);
    zr_sw_free(&w);
    return ok;
}

/* The loudest sample of a channel in frames [from, to). */
static int peak(const int16_t *s, size_t from, size_t to, unsigned channel) {
    int top = 0;
    for (size_t i = from; i < to; i++) {
        const int v = s[2 * i + channel] < 0 ? -s[2 * i + channel] : s[2 * i + channel];
        if (v > top) top = v;
    }
    return top;
}

/* Rising edges of a bipolar wave, with a margin so that noise is not counted. */
static int cycles(const int16_t *s, size_t from, size_t to, int margin) {
    int n = 0, low = 0;
    for (size_t i = from; i < to; i++) {
        if (s[2 * i] < -margin) low = 1;
        else if (s[2 * i] > margin && low) { low = 0; ++n; }
    }
    return n;
}

/* --- the beeper -------------------------------------------------------------------------- */

static int16_t buffer[2 * 8192], other[2 * 8192];

static void test_beeper(void) {
    static zr_beeper b, c;
    fake.now = 0;
    zr_beeper_init(&b, &HOST, CLOCK, RATE);
    /* A 1 kHz square wave on the speaker for 0.1 s: 200 edges 7000 ticks apart. */
    for (unsigned i = 1; i <= 200; i++) zr_beeper_set(&b, ZR_BEEPER_SPEAKER, (int)(i & 1u), (uint64_t)i * 7000u);
    size_t n = zr_beeper_render(&b, 1400000, buffer, 8192);
    CHECK(n == 4784);                                   /* 0.1 s = 4800 frames, the last 16 not yet final */
    CHECK(zr_beeper_render(&b, 1400000, buffer, 8192) == 0);
    const int top = peak(buffer, 1000, n, 0);
    CHECK(top > 4000 && top < 6500);                    /* +-5000: the weight, high-passed */
    CHECK(buffer[2 * 2000] == buffer[2 * 2000 + 1]);
    const int edges = cycles(buffer, 1000, n, 1000);
    CHECK(edges >= 77 && edges <= 80);                   /* 1 kHz for the 0.079 s from frame 1000 */
    /* A held level is silent: the high-pass lets the DC go. */
    zr_beeper_set(&b, ZR_BEEPER_SPEAKER, 1, 1400000);
    n = zr_beeper_render(&b, 2800000, buffer, 8192);
    CHECK(n > 4000 && peak(buffer, n - 100, n, 0) < 5);
    /* A state taken with edges still in flight continues the same sound. */
    zr_beeper_init(&b, &HOST, CLOCK, RATE);
    zr_beeper_init(&c, &HOST, CLOCK, RATE);
    for (unsigned i = 1; i <= 100; i++) zr_beeper_set(&b, ZR_BEEPER_SPEAKER, (int)(i & 1u), (uint64_t)i * 7000u + 3u);
    CHECK(transfer(&b.dev, &c.dev));
    for (unsigned i = 101; i <= 200; i++) {
        zr_beeper_set(&b, ZR_BEEPER_MIC, (int)(i & 1u), (uint64_t)i * 7000u + 3u);
        zr_beeper_set(&c, ZR_BEEPER_MIC, (int)(i & 1u), (uint64_t)i * 7000u + 3u);
    }
    const size_t nb = zr_beeper_render(&b, 1400000, buffer, 8192), nc = zr_beeper_render(&c, 1400000, other, 8192);
    CHECK(nb == nc && nb > 4000 && !memcmp(buffer, other, nb * 4));
}

/* --- the AY ------------------------------------------------------------------------------ */

static void ay_set(zr_ay *a, unsigned reg, uint8_t value) { zr_ay_select(a, (uint8_t)reg); zr_ay_write(a, value); }
static uint8_t ay_get(zr_ay *a, unsigned reg) { zr_ay_select(a, (uint8_t)reg); return zr_ay_read(a); }

static struct { uint8_t last; int calls; } port;
static uint8_t port_in(void *ctx) { (void)ctx; return 0x5Au; }
static void port_out(void *ctx, uint8_t value) { (void)ctx; port.last = value; ++port.calls; }

/* Channel A alone, tone period 100 (1093.75 Hz at 1.75 MHz), at amplitude `level`. */
static void tone_a(zr_ay *a, uint8_t level) {
    ay_set(a, 0, 100);
    ay_set(a, 7, 0x3E);
    ay_set(a, 8, level);
}

static void test_ay(void) {
    static zr_ay a, b;
    const zr_ay_io io = {NULL, port_in, port_out};
    fake.now = 0;
    zr_ay_init(&a, &HOST, CLOCK, 1750000, RATE, ZR_AY_MONO, &io);
    /* The registers keep their own bits; an address that is not the chip's leaves it deselected. */
    ay_set(&a, 1, 0xFF); ay_set(&a, 6, 0xFF); ay_set(&a, 13, 0xFF);
    CHECK(ay_get(&a, 1) == 0x0F && ay_get(&a, 6) == 0x1F && ay_get(&a, 13) == 0x0F);
    zr_ay_select(&a, 0x13);
    zr_ay_write(&a, 0x77);
    CHECK(zr_ay_read(&a) == 0xFF && ay_get(&a, 3) == 0);
    /* The tone: 1750000 / (16 * 100) Hz, and two levels of the DAC 3 dB twice (a factor 2) apart. */
    zr_ay_init(&a, &HOST, CLOCK, 1750000, RATE, ZR_AY_MONO, NULL);
    zr_ay_init(&b, &HOST, CLOCK, 1750000, RATE, ZR_AY_MONO, NULL);
    tone_a(&a, 15);
    tone_a(&b, 13);
    size_t n = zr_ay_render(&a, 1400000, buffer, 8192);
    CHECK(n == 4784 && zr_ay_render(&b, 1400000, other, 8192) == n);
    const int edges = cycles(buffer, 500, n, 1000);
    CHECK(edges >= 96 && edges <= 99);                   /* 0.089 s * 1093.75 Hz */
    const int p15 = peak(buffer, 500, n, 0), p13 = peak(other, 500, n, 0);
    CHECK(p15 > 2500 && p13 * 100 > p15 * 45 && p13 * 100 < p15 * 55);
    /* The envelope: shape 8 repeats every 256 * EP clocks (EP 64: 106.8 Hz, 53 cycles in 0.5 s). */
    zr_ay_init(&a, &HOST, CLOCK, 1750000, RATE, ZR_AY_MONO, NULL);
    ay_set(&a, 7, 0x3F); ay_set(&a, 8, 0x10); ay_set(&a, 11, 64); ay_set(&a, 13, 8);
    int jumps = 0;
    size_t skip = 0;
    n = 0;
    for (uint64_t t = 1750000; t <= 7000000; t += 1750000) {    /* 0.125 s a pull */
        const size_t got = zr_ay_render(&a, t, buffer, 8192);
        for (size_t i = 1; i < got; i++) {
            if (skip) { --skip; continue; }
            if (buffer[2 * i] - buffer[2 * (i - 1)] > 2500) { ++jumps; skip = 50; }
        }
        n += got;
    }
    CHECK(jumps >= 52 && jumps <= 54);
    /* Where the shapes end: held at 0 or 15 (9, 11, 13, 15; 0 and 4 drop to 0), the triangle (10) in step. */
    static const uint8_t HELD[][2] = {{0, 0}, {4, 0}, {9, 0}, {11, 15}, {13, 15}, {15, 0}};
    for (unsigned i = 0; i < sizeof HELD / sizeof HELD[0]; i++) {
        fake.now = 0;
        zr_ay_init(&a, &HOST, CLOCK, 1750000, RATE, ZR_AY_MONO, NULL);
        ay_set(&a, 11, 1); ay_set(&a, 13, HELD[i][0]);
        fake.now = 128u * 64u;                          /* 64 envelope steps of 128 master ticks */
        zr_ay_read(&a);
        CHECK(a.env == HELD[i][1]);
    }
    fake.now = 0;
    zr_ay_init(&a, &HOST, CLOCK, 1750000, RATE, ZR_AY_MONO, NULL);
    ay_set(&a, 11, 1); ay_set(&a, 13, 10);
    fake.now = 128u * 56u;
    zr_ay_read(&a);
    CHECK(a.env == 8);
    /* The noise is not a constant. */
    fake.now = 0;
    zr_ay_init(&a, &HOST, CLOCK, 1750000, RATE, ZR_AY_MONO, NULL);
    ay_set(&a, 6, 1); ay_set(&a, 7, 0x37); ay_set(&a, 8, 15);
    n = zr_ay_render(&a, 700000, buffer, 8192);
    CHECK(peak(buffer, 100, n, 0) > 1500);
    /* Stereo: with ABC channel A is the left only, with ACB channel C is in the middle. */
    zr_ay_init(&a, &HOST, CLOCK, 1750000, RATE, ZR_AY_ABC, NULL);
    tone_a(&a, 15);
    n = zr_ay_render(&a, 700000, buffer, 8192);
    CHECK(peak(buffer, 100, n, 0) > 1500 && peak(buffer, 100, n, 1) == 0);
    zr_ay_init(&a, &HOST, CLOCK, 1750000, RATE, ZR_AY_ACB, NULL);
    ay_set(&a, 4, 100); ay_set(&a, 7, 0x3B); ay_set(&a, 10, 15);
    n = zr_ay_render(&a, 700000, buffer, 8192);
    CHECK(peak(buffer, 100, n, 0) > 500 && peak(buffer, 100, n, 0) == peak(buffer, 100, n, 1));
    /* Port A: an output shows R14 on the pins, an input reads them. */
    zr_ay_init(&a, &HOST, CLOCK, 1750000, RATE, ZR_AY_MONO, &io);
    port.calls = 0;
    ay_set(&a, 14, 0xA5);
    CHECK(port.calls == 0);                             /* still an input */
    ay_set(&a, 7, 0x40);
    CHECK(port.last == 0xA5);
    ay_set(&a, 14, 0x3C);
    CHECK(port.last == 0x3C && ay_get(&a, 14) == 0x3C);
    ay_set(&a, 7, 0x00);
    CHECK(port.last == 0xFF && ay_get(&a, 14) == 0x5A);
    /* A state taken in the middle of a tone and an envelope continues the same sound. */
    zr_ay_init(&a, &HOST, CLOCK, 1750000, RATE, ZR_AY_ABC, NULL);
    zr_ay_init(&b, &HOST, CLOCK, 1750000, RATE, ZR_AY_ABC, NULL);
    tone_a(&a, 0x10);
    ay_set(&a, 11, 20); ay_set(&a, 13, 14);
    fake.now = 700000;
    zr_ay_read(&a);
    CHECK(transfer(&a.dev, &b.dev));
    const size_t na = zr_ay_render(&a, 1400000, buffer, 8192), nb = zr_ay_render(&b, 1400000, other, 8192);
    CHECK(na == nb && na > 4000 && !memcmp(buffer, other, na * 4));
    fake.now = 0;
}

/* --- the ULA ----------------------------------------------------------------------------- */

static uint8_t video[0x1B00];
static uint8_t read_video(void *ctx, uint16_t offset) { (void)ctx; return video[offset]; }

#define PIXEL(u, x, y) ((u).pixels[(y) * ZR_ULA_WIDTH + (x)])

/* The guest time the raster counter first has this line and pixel, as the frame's `frame`th. */
static uint64_t when(const zr_ula *u, unsigned frame, unsigned line, unsigned pixel) {
    const uint64_t counter = (uint64_t)frame * u->total + (uint64_t)line * u->cfg.pixels_per_line + pixel;
    return (counter - u->cfg.start_pixel) * u->cfg.ticks_per_pixel;
}

static void test_ula(void) {
    static zr_ula u, v;
    zr_frame frame;
    fake.now = 0;
    fake.lines = 0;
    memset(video, 0, sizeof video);
    memset(video + 0x1800, 0x2A, 0x300);                /* ink red (2), paper cyan (5) everywhere */
    video[0x0000] = 0x80;                               /* cell 0 of the first line: one dot at its left */
    video[0x0001] = 0xFF; video[0x1801] = 0x2A | 0x40; /* cell 1: bright */
    video[0x0002] = 0xFF; video[0x1802] = 0x2A | 0x80; /* cell 2: flashing */
    zr_ula_init(&u, &HOST, NULL, read_video, NULL);
    CHECK(zr_ula_frame(&u, &frame) && frame.width == 352 && frame.height == 288 && frame.colors == 16);
    CHECK(frame.aspect_w == 4 && frame.aspect_h == 3 && frame.palette[2] == 0xCD0000 && frame.palette[10] == 0xFF0000 && frame.palette[8] == 0);
    /* The raster: the counter is on its last pixel at time 0. */
    unsigned line, pixel;
    zr_ula_position(&u, 0, &line, &pixel);
    CHECK(line == 311 && pixel == 447);
    zr_ula_position(&u, when(&u, 1, 248, 5), &line, &pixel);
    CHECK(line == 248 && pixel == 5);
    /* The picture, the border and BRIGHT, after two frames (the first cell of a power-on frame was never fetched: the counters start on the last pixel). */
    zr_ula_set_border(&u, 1);
    zr_ula_catch_up(&u, 2 * zr_ula_frame_ticks(&u));
    CHECK(PIXEL(u, 0, 0) == 1 && PIXEL(u, 351, 287) == 1);
    CHECK(PIXEL(u, 48, 48) == 2 && PIXEL(u, 49, 48) == 5 && PIXEL(u, 48, 49) == 5);
    CHECK(PIXEL(u, 56, 48) == 10 && PIXEL(u, 63, 48) == 10);
    CHECK(PIXEL(u, 64, 48) == 2);                       /* cell 2, FLASH not yet on */
    CHECK(u.serial > 0);
    const uint64_t serial = u.serial;
    zr_ula_catch_up(&u, 2 * zr_ula_frame_ticks(&u));
    CHECK(u.serial == serial);                          /* nothing drawn, nothing changed */
    /* FLASH: the inks and papers swap from frame 16, every 16 frames. */
    zr_ula_catch_up(&u, when(&u, 16, 0, 300));
    CHECK(PIXEL(u, 64, 48) == 5 && PIXEL(u, 48, 48) == 2);
    /* A border change in mid-frame lands at its own pixel: a scan line is the end of the raster line before and the start of its own. */
    zr_ula_init(&u, &HOST, NULL, read_video, NULL);
    zr_ula_set_border(&u, 1);
    zr_ula_catch_up(&u, when(&u, 1, 100, 0));
    zr_ula_set_border(&u, 3);
    zr_ula_catch_up(&u, zr_ula_frame_ticks(&u));
    CHECK(PIXEL(u, 310, 147) == 1 && PIXEL(u, 10, 148) == 1);       /* before: the line 99, and the left of the scan line 148 */
    CHECK(PIXEL(u, 310, 148) == 3 && PIXEL(u, 10, 149) == 3);
    /* A screen without BRIGHT. */
    zr_ula_config plain;
    zr_ula_default_config(&plain);
    plain.bright = 0;
    plain.level_normal = 0xC0;
    zr_ula_init(&v, &HOST, &plain, read_video, NULL);
    zr_ula_catch_up(&v, zr_ula_frame_ticks(&v));
    zr_ula_frame(&v, &frame);
    CHECK(PIXEL(v, 56, 48) == 2 && frame.palette[2] == 0xC00000);
    /* The frame interrupt: up at line 248 of the second counter frame, down 32 T-states (128 ticks) later, a frame on. */
    fake.lines = 0;
    zr_ula_init(&u, &HOST, NULL, read_video, NULL);
    CHECK(fake.deadline == when(&u, 1, 248, 0));
    fake.now = fake.deadline;
    u.dev.ops->event(&u.dev, fake.now);
    CHECK(fake.lines == 1 && fake.line_state[0] == 1 && fake.deadline == fake.now + 128);
    const uint64_t up = fake.now;
    fake.now = fake.deadline;
    u.dev.ops->event(&u.dev, fake.now);
    CHECK(fake.lines == 2 && fake.line_state[1] == 0 && fake.deadline == up + zr_ula_frame_ticks(&u));
    /* The keyboard: the rows whose address line is low, a pressed key reads 0. */
    zr_ula_key(&u, 0, 0, 1);
    zr_ula_key(&u, 1, 2, 1);
    zr_ula_key(&u, 2, 3, 1);
    CHECK(zr_ula_keys(&u, 0xFF) == 0x1F && zr_ula_keys(&u, 0xFE) == 0x1E && zr_ula_keys(&u, 0xF9) == 0x13 && zr_ula_keys(&u, 0xFD) == 0x1B);
    zr_ula_key(&u, 0, 0, 0);
    CHECK(zr_ula_keys(&u, 0x00) == 0x13);
    /* A state taken mid-frame (pipeline, keys, interrupt) draws the same screen. */
    fake.now = 0;
    zr_ula_init(&u, &HOST, NULL, read_video, NULL);
    zr_ula_init(&v, &HOST, NULL, read_video, NULL);
    zr_ula_set_border(&u, 5);
    zr_ula_key(&u, 3, 1, 1);
    zr_ula_catch_up(&u, when(&u, 1, 3, 100));
    CHECK(transfer(&u.dev, &v.dev) && fake.deadline == u.int_at);
    zr_ula_set_border(&u, 2);
    zr_ula_set_border(&v, 2);
    zr_ula_catch_up(&u, zr_ula_frame_ticks(&u) * 2);
    zr_ula_catch_up(&v, zr_ula_frame_ticks(&v) * 2);
    CHECK(!memcmp(u.pixels, v.pixels, sizeof u.pixels) && zr_ula_keys(&v, 0xF7) == 0x1D && PIXEL(v, 20, 100) == 2);
}

/* Sinclair 48K timing: the contention table, the floating bus and the cell drawn from the bytes fetched at their T-states. */
static void test_sinclair(void) {
    static zr_ula u;
    zr_ula_config c;
    fake.now = 0;
    memset(video, 0, sizeof video);
    video[0x0000] = 0x11; video[0x1800] = 0x22; video[0x0001] = 0x33; video[0x1801] = 0x44; video[0x0100] = 0x55;
    zr_ula_default_config(&c);
    zr_ula_sinclair_config(&c, ZR_ULA_SINCLAIR_48K);
    zr_ula_init(&u, &HOST, &c, read_video, NULL);
#define AT(t) when(&u, 1, 0, 2u * (t))
    CHECK(u.tstates == 69888 && zr_ula_tstate(&u, AT(0)) == 0 && zr_ula_tstate(&u, AT(14335)) == 14335);
    static const uint8_t pattern[10] = {0, 6, 5, 4, 3, 2, 1, 0, 0, 6};
    for (unsigned i = 0; i < 10u; i++) CHECK(zr_ula_delay(&u, 14334u + i) == pattern[i]);
    CHECK(zr_ula_delay(&u, 14462) == 0 && zr_ula_delay(&u, 14463) == 0 && zr_ula_delay(&u, 14559) == 6);
    CHECK(zr_ula_delay(&u, 14335 + 191 * 224) == 6 && zr_ula_delay(&u, 14335 + 192 * 224) == 0 && zr_ula_delay(&u, 69888 + 14335) == 6);
    CHECK(zr_ula_floating(&u, AT(14337)) == 0xFF && zr_ula_floating(&u, AT(14338)) == 0x11 && zr_ula_floating(&u, AT(14339)) == 0x22);
    CHECK(zr_ula_floating(&u, AT(14340)) == 0x33 && zr_ula_floating(&u, AT(14341)) == 0x44 && zr_ula_floating(&u, AT(14342)) == 0xFF);
    CHECK(zr_ula_floating(&u, AT(14338 + 224)) == 0x55 && zr_ula_floating(&u, AT(14338 + 128)) == 0xFF);
    /* Cell 0's attribute is fetched at 14339: a change landing then is seen, one landing at 14340 is not (cell 1's). */
    video[0x0000] = 0xFF;
    video[0x0001] = 0xFF;
    zr_ula_catch_up(&u, AT(14339));
    video[0x1800] = 0x02;                               /* ink red */
    zr_ula_catch_up(&u, AT(14340));
    video[0x1800] = 0x04;
    zr_ula_catch_up(&u, AT(14342));
    video[0x1801] = 0x01;                               /* after cell 1's fetch at 14341: still 44h, BRIGHT ink 4 */
    zr_ula_catch_up(&u, AT(20000));
    CHECK(PIXEL(u, 48, 48) == 2 && PIXEL(u, 56, 48) == 12);
#undef AT
}

int main(void) {
    test_beeper();
    test_ay();
    test_ula();
    test_sinclair();
    printf(failures ? "%d failure(s)\n" : "ok\n", failures);
    return failures != 0;
}

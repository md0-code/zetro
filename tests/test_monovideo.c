/* The monochrome video's unit check (hw/monovideo.h) against a fake host: the picture against rp_mono_video.sv's rule (a lit pixel is a clear
 * bit, a set bit with the inversion; the blanking dark whatever the inversion), the frame's geometry (320 x 288, the picture 32 pixels and 16
 * lines in), drawing up to a write (what the memory held at each pixel's time), the PRAE's frame interrupt (pixel 0 of line 256, 16 pixels
 * long, once a frame; none when the chip has none) and a state round trip. No SDL, no window. */
#include "monovideo.h"
#include "machine.h"
#include "state.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(what) do { if (!(what)) { printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #what); ++failures; } } while (0)

#define PIXELS ZR_MONOVIDEO_PIXELS
#define TOTAL (ZR_MONOVIDEO_PIXELS * ZR_MONOVIDEO_LINES)
#define TICKS 2u

static struct {
    uint64_t now, deadline;
    int asserts, releases;
    uint64_t first_assert, second_assert, release;
} fake;

static uint64_t f_now(void *ctx) { (void)ctx; return fake.now; }
static void f_schedule(void *ctx, void *dev, uint64_t when) { (void)ctx; (void)dev; fake.deadline = when; }
static void f_line(void *ctx, void *dev, int pin, int state) {
    (void)ctx; (void)dev; (void)pin;
    if (state) {
        if (++fake.asserts == 1) fake.first_assert = fake.now;
        else if (fake.asserts == 2) fake.second_assert = fake.now;
    } else if (++fake.releases == 1) fake.release = fake.now;
}
static const zr_hw_host HOST = {NULL, f_now, f_schedule, f_line, NULL};

/* The guest time at which the raster counter is (frame, line, pixel): the counter is the last pixel of a frame at time 0. */
static uint64_t time_of(unsigned frame, unsigned line, unsigned pixel) { return ((uint64_t)(frame - 1u) * TOTAL + (uint64_t)line * PIXELS + pixel + 1u) * TICKS; }

/* Runs the video to `t`, the interrupt's deadlines on the way. */
static void run_to(zr_monovideo *v, uint64_t t) {
    while (fake.deadline <= t) {
        fake.now = fake.deadline;
        fake.deadline = ZR_NEVER;
        v->dev.ops->event(&v->dev, fake.now);
    }
    fake.now = t;
    zr_monovideo_catch_up(v, t);
}

static uint8_t vram[ZR_MONOVIDEO_VRAM];

static void fill(void) {
    for (unsigned i = 0; i < sizeof vram; ++i) vram[i] = (uint8_t)(i * 37u + (i >> 5) * 11u + 5u);
}

/* Every pixel of the frame against the rule: the picture from the bitmap (bit 7 first, a lit pixel a clear bit unless inverted), the border and
 * the blanking dark. */
static int picture_ok(const zr_frame *f, int invert) {
    for (unsigned y = 0; y < 288u; ++y)
        for (unsigned x = 0; x < 320u; ++x) {
            unsigned expected = 0;
            if (x >= 32u && x < 288u && y >= 16u && y < 272u) {
                const unsigned px = x - 32u, py = y - 16u;
                const unsigned bit = vram[py * 32u + px / 8u] >> (7u - px % 8u) & 1u;
                expected = (bit ^ (unsigned)invert) ? 0u : 1u;
            }
            if (f->pixels[y * f->pitch + x] != expected) return 0;
        }
    return 1;
}

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

int main(void) {
    static zr_monovideo v, w;
    zr_frame f;
    fill();
    fake.deadline = ZR_NEVER;
    /* No interrupt: no deadline. */
    zr_monovideo_init(&v, &HOST, TICKS, vram, 0);
    CHECK(fake.deadline == ZR_NEVER && zr_monovideo_frame_ticks(&v) == 199680u && TOTAL == 99840);
    run_to(&v, time_of(3, 0, 0));
    CHECK(fake.asserts == 0);
    /* The geometry and the picture: frame 3 is complete at its raster line 272. */
    run_to(&v, time_of(3, 272, 0));
    CHECK(zr_monovideo_frame(&v, &f) && f.width == 320 && f.height == 288 && f.pitch == 320 && f.aspect_w == 4 && f.aspect_h == 3 && f.colors == 2);
    CHECK(f.palette[0] == 0u && f.palette[1] == 0xFFFFFFu);
    CHECK(picture_ok(&f, 0));
    /* The inversion lights the set bits and leaves the blanking dark. */
    zr_monovideo_set_invert(&v, 1);
    run_to(&v, time_of(5, 272, 0));
    zr_monovideo_frame(&v, &f);
    CHECK(picture_ok(&f, 1));
    zr_monovideo_set_invert(&v, 0);
    /* Drawing is up to the write: a byte changed at the time of its pixels shows from there on (line 100, byte 3, in frame 7). */
    run_to(&v, time_of(7, 100, 24 + 4));            /* the middle of the byte's pixels */
    vram[100 * 32 + 3] = (uint8_t)~vram[100 * 32 + 3];
    run_to(&v, time_of(7, 272, 0));
    zr_monovideo_frame(&v, &f);
    {
        const uint8_t old = (uint8_t)~vram[100 * 32 + 3];
        for (unsigned px = 24; px < 32; ++px) {
            const unsigned was = old >> (7u - px % 8u) & 1u, now = vram[100 * 32 + 3] >> (7u - px % 8u) & 1u;
            const unsigned shown = f.pixels[(16u + 100u) * f.pitch + 32u + px];
            if (px < 28u) CHECK(shown == (was ? 0u : 1u));          /* before the write: the old bits (the pixels that started before the write are drawn with them) */
            else CHECK(shown == (now ? 0u : 1u));
        }
    }
    /* The palette's lit colour: the green monitor. */
    {
        const uint64_t serial = v.serial;
        zr_monovideo_set_colour(&v, 0x33FF66u);
        zr_monovideo_frame(&v, &f);
        CHECK(f.palette[1] == 0x33FF66u && f.serial != serial);
    }
    /* The PRAE's interrupt: pixel 0 of line 256, 16 pixels long, one frame apart; the first of them at time 2 * (256 * 320 + 1). */
    fake.deadline = ZR_NEVER;
    fake.asserts = fake.releases = 0;
    fake.now = 0;
    zr_monovideo_init(&w, &HOST, TICKS, vram, 1);
    CHECK(fake.deadline == time_of(1, 256, 0));
    run_to(&w, time_of(4, 0, 0));
    CHECK(fake.asserts == 3 && fake.releases == 3);
    CHECK(fake.first_assert == time_of(1, 256, 0) && fake.release - fake.first_assert == (uint64_t)ZR_MONOVIDEO_INT_PIXELS * TICKS);
    CHECK(fake.second_assert - fake.first_assert == 199680u);
    {
        unsigned line, pixel;
        zr_monovideo_position(&w, fake.first_assert, &line, &pixel);
        CHECK(line == 256u && pixel == 0u);
        CHECK(zr_monovideo_frame_number(&w, fake.second_assert) == zr_monovideo_frame_number(&w, fake.first_assert) + 1u);
    }
    /* A state out of one chip and into another gives the same frame, raster and interrupt. */
    run_to(&w, time_of(6, 100, 0));
    fake.deadline = ZR_NEVER;
    zr_monovideo_init(&v, &HOST, TICKS, vram, 1);
    CHECK(transfer(&w.dev, &v.dev));
    CHECK(v.drawn == w.drawn && v.invert == w.invert && v.int_at == w.int_at && v.int_on == w.int_on && !memcmp(v.pixels, w.pixels, sizeof v.pixels));
    CHECK(fake.deadline == w.int_at);
    if (failures) {
        printf("%d checks failed\n", failures);
        return 1;
    }
    printf("test_monovideo: ok\n");
    return 0;
}

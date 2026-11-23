/* The HC-88 video's unit check (hw/hc88video.h) against a fake host: the picture of both modes against what the core's bench
 * (sim/hc88_video_tb.sv) expects of the serializer (the two banks interleaved by byte in the 512-pixel mode, each with its own
 * attributes, BRIGHT, FLASH from the frame number's bit 4, the border), the frame's geometry, the interrupt (128 dots, once a
 * frame at line 248) and a state round trip. No SDL, no window. */
#include "hc88video.h"
#include "machine.h"
#include "state.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(what) do { if (!(what)) { printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #what); ++failures; } } while (0)

#define DOTS ZR_HC88VIDEO_DOTS
#define TOTAL (ZR_HC88VIDEO_DOTS * ZR_HC88VIDEO_LINES)

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

/* The guest time at which the raster counter is (frame, line, dot): the counter is the last dot of a frame at time 0. */
static uint64_t time_of(unsigned frame, unsigned line, unsigned dot) { return (uint64_t)(frame - 1u) * TOTAL + (uint64_t)line * DOTS + dot + 1u; }

/* Runs the video to `t`, the interrupt's deadlines on the way. */
static void run_to(zr_hc88video *v, uint64_t t) {
    while (fake.deadline <= t) {
        fake.now = fake.deadline;
        fake.deadline = ZR_NEVER;
        v->dev.ops->event(&v->dev, fake.now);
    }
    fake.now = t;
    zr_hc88video_catch_up(v, t);
}

static uint8_t vram[ZR_HC88VIDEO_VRAM];

/* The bench's memory: the bitmap bytes A5h in bank 0 and 5Ah in bank 1, the attribute of cell `col` of bank b with ink
 * (col + b) & 7, paper (col + 3 + b) & 7, FLASH when col & 8 and BRIGHT when col & 16 (the attribute area of every row alike). */
static void fill(void) {
    for (unsigned i = 0; i < sizeof vram; ++i) vram[i] = i & 0x2000u ? 0x5Au : 0xA5u;
    for (unsigned b = 0; b < 2u; ++b)
        for (unsigned row = 0; row < 24u; ++row)
            for (unsigned col = 0; col < 32u; ++col)
                vram[b * 0x2000u + 0x1800u + row * 32u + col] = (uint8_t)((col & 8u ? 128u : 0u) | (col & 16u ? 64u : 0u) | (((col + 3u + b) & 7u) << 3) | ((col + b) & 7u));
}

/* Every picture dot of the frame against the bench's formula, FLASH `flash`; the border elsewhere. */
static int picture_ok(const zr_frame *f, int hires, unsigned flash, unsigned border) {
    for (unsigned y = 0; y < 192u; ++y)
        for (unsigned x = 0; x < 512u; ++x) {
            const unsigned px = hires ? x : x / 2u, byte_col = px / 8u;
            const unsigned bank = hires ? byte_col & 1u : 0u, col = hires ? byte_col / 2u : byte_col;
            const unsigned pattern = bank ? 0x5Au : 0xA5u;
            const unsigned ink = (col + bank) & 7u, paper = (col + 3u + bank) & 7u;
            const unsigned on = (pattern >> (7u - px % 8u) & 1u) ^ ((col & 8u) && flash ? 1u : 0u);
            const unsigned expected = (on ? ink : paper) | (col & 16u ? 8u : 0u);
            if (f->pixels[(48u + y) * f->pitch + 96u + x] != expected) return 0;
        }
    for (unsigned x = 0; x < f->width; ++x)
        if (f->pixels[(48u + 192u + 10u) * f->pitch + x] != border || f->pixels[10u * f->pitch + x] != border) return 0;
    for (unsigned y = 50u; y < 238u; ++y)
        if (f->pixels[y * f->pitch + 10u] != border || f->pixels[y * f->pitch + 700u] != border) return 0;
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
    static zr_hc88video v, w;
    zr_frame f;
    fill();
    fake.deadline = ZR_NEVER;
    zr_hc88video_init(&v, &HOST, 1, vram);
    CHECK(fake.deadline != ZR_NEVER && zr_hc88video_frame_ticks(&v) == TOTAL && TOTAL == 279552);
    zr_hc88video_set_border(&v, 6);
    /* 256 pixels: frame 2 has FLASH off, frame 17 FLASH on (a frame's picture is drawn complete by its line 250). */
    run_to(&v, time_of(2, 250, 0));
    CHECK(zr_hc88video_frame(&v, &f) && f.width == 704 && f.height == 288 && f.aspect_w == 4 && f.aspect_h == 3 && f.colors == 16);
    CHECK(picture_ok(&f, 0, 0, 6));
    run_to(&v, time_of(17, 250, 0));
    zr_hc88video_frame(&v, &f);
    CHECK(picture_ok(&f, 0, 1, 6));
    /* 512 pixels, from the next frame on. */
    zr_hc88video_set_hires(&v, 1);
    run_to(&v, time_of(19, 250, 0));
    zr_hc88video_frame(&v, &f);
    CHECK(picture_ok(&f, 1, 1, 6));
    run_to(&v, time_of(34, 250, 0));
    zr_hc88video_frame(&v, &f);
    CHECK(picture_ok(&f, 1, 0, 6));
    /* The palette: the lit component at CDh, BRIGHT at FFh, BRIGHT black black. */
    CHECK(v.palette[1] == 0x0000CDu && v.palette[2] == 0xCD0000u && v.palette[7] == 0xCDCDCDu && v.palette[9] == 0x0000FFu && v.palette[8] == 0u);
    /* The interrupt: 128 dots long, one frame apart, from the first dot of line 248 after the first. */
    CHECK(fake.asserts >= 3 && fake.releases >= 2);
    CHECK(fake.second_assert - fake.first_assert == TOTAL && fake.release - fake.first_assert == ZR_HC88VIDEO_INT_DOTS);
    {
        unsigned line, dot;
        zr_hc88video_position(&v, fake.first_assert, &line, &dot);
        CHECK(line == 248u && dot == 1u);
        CHECK(zr_hc88video_frame_number(&v, fake.second_assert) == zr_hc88video_frame_number(&v, fake.first_assert) + 1u);
    }
    /* A state out of one chip and into another gives the same frame, raster and interrupt. */
    fake.deadline = ZR_NEVER;
    zr_hc88video_init(&w, &HOST, 1, vram);
    CHECK(transfer(&v.dev, &w.dev));
    zr_hc88video_frame(&w, &f);
    CHECK(w.drawn == v.drawn && w.border == v.border && w.hires == v.hires && w.int_at == v.int_at && !memcmp(w.pixels, v.pixels, sizeof v.pixels));
    CHECK(picture_ok(&f, 1, 0, 6));
    if (failures) {
        printf("%d checks failed\n", failures);
        return 1;
    }
    printf("test_hc88video: ok\n");
    return 0;
}

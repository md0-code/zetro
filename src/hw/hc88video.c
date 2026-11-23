/* ZetRo - the HC-88's video (hc88video.h). Written from this project's HC core: docs/hc/hc88.md (the display row and
 * the command latch), rtl/hc88_video.sv (the serializer, the raster, the attribute and FLASH logic, the blanking)
 * and the interrupt of rtl/hc_family_core.sv, with sim/hc88_video_tb.sv as the guide to what the picture is. ZetRo's
 * own code. */
#include "hc88video.h"
#include "machine.h"
#include "state.h"

#include <string.h>

#define W ZR_HC88VIDEO_WIDTH
#define H ZR_HC88VIDEO_HEIGHT
#define DOTS ZR_HC88VIDEO_DOTS
#define LINES ZR_HC88VIDEO_LINES
#define STATE_VERSION 1u

/* The dot of line 248 the interrupt starts at, and the line: the core's x and y counters are registered at each dot, so
 * they read (0, 248) once the first dot of the line has been processed. */
#define INT_DOT 1u
#define INT_LINE 248u
/* The raster counter of the first dot of the frame as the monitor scans it: the first scan line shown is the one of
 * raster line 264 (the vertical blanking, lines 240-263, ends before it), and it begins at dot 800 of the line before
 * (the horizontal blanking, dots 608-799, ends there). */
#define WINDOW_LINE 263u
#define WINDOW_DOT 800u

/* The Spectrum's address of a line's bitmap byte (the first screen's, 0000-17FF), for a character cell. */
static inline unsigned bitmap_offset(unsigned y, unsigned cell) {
    return ((y & 0xC0u) << 5) | ((y & 0x07u) << 8) | ((y & 0x38u) << 2) | cell;
}

/* The colour index (0-15) of the picture dot (x, y), x < 512, y < 192, as hc88_video.sv's serializer gives it: the
 * pixel of bit 7-x[3:1] of the byte of bank 0 in 256-pixel mode, bit 7-x[2:0] of the byte of bank x[3] in 512-pixel
 * mode, each with the attribute of its own bank and cell. */
static inline uint8_t picture_dot(const uint8_t *vram, unsigned x, unsigned y, int hires, unsigned flash) {
    const unsigned cell = x >> 4;
    const unsigned bank = hires ? (x >> 3) & 1u : 0u;
    const unsigned bit = hires ? x & 7u : (x >> 1) & 7u;
    const unsigned base = bank << 13;
    const unsigned bitmap = vram[base + bitmap_offset(y, cell)];
    const unsigned attr = vram[base + 0x1800u + (y >> 3) * 32u + cell];
    const unsigned lit = ((bitmap >> (7u - bit)) ^ ((attr >> 7) & flash)) & 1u;
    const unsigned colour = lit ? attr & 7u : (attr >> 3) & 7u;
    return (uint8_t)(colour | (attr & 0x40u ? 8u : 0u));
}

/* `n` raster dots that stay on one raster line and one scan line of the frame, from raster dot (x, y) and frame dot
 * (wx, wy), wy < H. Dots with wx >= W are blanking and are not drawn. */
static void draw_run(zr_hc88video *v, uint32_t x, uint32_t y, uint32_t wx, uint32_t wy, uint32_t n, unsigned flash) {
    uint8_t *row = v->pixels + (size_t)wy * W;
    for (uint32_t i = 0; i < n; ++i, ++x, ++wx) {
        if (wx >= W) continue;
        const uint8_t colour = (y < 192u && x < 512u) ? picture_dot(v->vram, x, y, v->hires, flash) : v->border;
        if (row[wx] != colour) {
            row[wx] = colour;
            v->changed = 1;
        }
    }
}

void zr_hc88video_position(const zr_hc88video *v, uint64_t t, unsigned *line, unsigned *dot) {
    const uint64_t counter = (t / v->ticks_per_dot + v->start_dot) % v->total;
    if (line) *line = (unsigned)(counter / DOTS);
    if (dot) *dot = (unsigned)(counter % DOTS);
}

uint64_t zr_hc88video_frame_number(const zr_hc88video *v, uint64_t t) {
    return (t / v->ticks_per_dot + v->start_dot) / v->total;
}

void zr_hc88video_catch_up(zr_hc88video *v, uint64_t t) {
    const uint64_t limit = (t + v->ticks_per_dot - 1u) / v->ticks_per_dot;
    while (v->drawn < limit) {
        const uint64_t at = v->drawn + v->start_dot;
        const uint64_t frame = at / v->total;
        const uint32_t counter = (uint32_t)(at % v->total);
        const uint32_t window = (counter + v->total - v->window_origin) % v->total;
        const uint32_t x = counter % DOTS, wx = window % DOTS, wy = window / DOTS;
        uint64_t n = limit - v->drawn;
        if (n > v->total - counter) n = v->total - counter;
        if (n > DOTS - x) n = DOTS - x;
        if (n > DOTS - wx) n = DOTS - wx;
        if (wy < H) draw_run(v, x, counter / DOTS, wx, wy, (uint32_t)n, (unsigned)((frame >> 4) & 1u));
        v->drawn += n;
    }
    if (v->changed) {
        v->changed = 0;
        ++v->serial;
    }
}

void zr_hc88video_set_border(zr_hc88video *v, unsigned colour) { v->border = (uint8_t)(colour & 7u); }
void zr_hc88video_set_hires(zr_hc88video *v, int hires) { v->hires = hires ? 1u : 0u; }

int zr_hc88video_frame(const zr_hc88video *v, zr_frame *f) {
    f->width = W;
    f->height = H;
    f->pitch = W;
    f->pixels = v->pixels;
    f->palette = v->palette;
    f->colors = 16;
    f->aspect_w = 4;
    f->aspect_h = 3;
    f->serial = v->serial;
    f->picture_x = 96;
    f->picture_y = 48;
    f->picture_w = 512;
    f->picture_h = 192;
    return 1;
}

/* --- the frame interrupt ------------------------------------------------------------------ */

/* Guest time of the first assertion at or after `now`: frame f asserts at raster dot f * total + INT_LINE * DOTS +
 * INT_DOT counted from guest time 0 (the counter being start_dot then). */
static uint64_t first_assert(const zr_hc88video *v, uint64_t now) {
    const int64_t total = (int64_t)v->total;
    const int64_t base = (int64_t)INT_LINE * DOTS + INT_DOT - (int64_t)v->start_dot;
    const int64_t first = (int64_t)((now + v->ticks_per_dot - 1u) / v->ticks_per_dot);
    int64_t need = first - base;
    if (need < -base) need = -base;
    const int64_t f = need <= 0 ? 0 : (need + total - 1) / total;
    return (uint64_t)(f * total + base) * v->ticks_per_dot;
}

static uint64_t int_hold(const zr_hc88video *v) { return (uint64_t)ZR_HC88VIDEO_INT_DOTS * v->ticks_per_dot; }

static void arm(zr_hc88video *v) {
    if (v->dev.host) zr_dev_schedule(&v->dev, v->int_at);
}

/* The deadline: the interrupt goes up, or comes down and is set for the next frame. Late is handled in order. */
static void video_event(void *d, uint64_t now) {
    zr_hc88video *v = d;
    for (;;) {
        const uint64_t hold = int_hold(v);
        if (!v->int_on) {
            if (v->int_at > now) break;
            v->int_on = 1;
            zr_dev_line(&v->dev, ZR_HC88VIDEO_INT, 1);
            v->int_at += hold;                  /* now the time of the release */
        } else {
            if (v->int_at > now) break;
            v->int_on = 0;
            zr_dev_line(&v->dev, ZR_HC88VIDEO_INT, 0);
            v->int_at += zr_hc88video_frame_ticks(v) - hold;    /* the next assertion */
        }
    }
    arm(v);
}

/* --- the dev ------------------------------------------------------------------------------- */

static uint8_t video_read(void *d, unsigned space, uint32_t offset) { (void)d; (void)space; (void)offset; return 0xFFu; }
static void video_write(void *d, unsigned space, uint32_t offset, uint8_t value) { (void)d; (void)space; (void)offset; (void)value; }

static void video_reset(void *d) {
    zr_hc88video *v = d;
    const uint64_t now = v->dev.host ? zr_dev_now(&v->dev) : 0u;
    if (v->int_on) {
        v->int_on = 0;
        zr_dev_line(&v->dev, ZR_HC88VIDEO_INT, 0);
    }
    v->border = v->hires = 0;
    v->int_at = first_assert(v, now);
    arm(v);
}

static void video_save(const void *d, struct zr_state_writer *w) {
    const zr_hc88video *v = d;
    zr_sw_u8(w, STATE_VERSION);
    zr_sw_u8(w, v->border);
    zr_sw_u8(w, v->hires);
    zr_sw_u64(w, v->drawn);
    zr_sw_u64(w, v->int_at);
    zr_sw_u8(w, v->int_on);
    zr_sw_u64(w, v->serial);
    zr_sw_packed(w, v->pixels, sizeof v->pixels);
}

static int video_load(void *d, struct zr_state_reader *r) {
    zr_hc88video *v = d;
    if (zr_sr_u8(r) != STATE_VERSION) return 0;
    v->border = zr_sr_u8(r) & 7u;
    v->hires = zr_sr_u8(r) & 1u;
    v->drawn = zr_sr_u64(r);
    v->int_at = zr_sr_u64(r);
    v->int_on = zr_sr_u8(r) & 1u;
    v->serial = zr_sr_u64(r) + 1u;              /* what the shell shows is not this frame */
    zr_sr_packed(r, v->pixels, sizeof v->pixels);
    v->changed = 0;
    arm(v);
    return zr_sr_ok(r);
}

static const zr_dev_ops VIDEO_OPS = {
    .read = video_read, .write = video_write, .event = video_event, .reset = video_reset,
    .save = video_save, .load = video_load,
};

void zr_hc88video_init(zr_hc88video *v, const zr_hw_host *host, uint32_t ticks_per_dot, const uint8_t *vram) {
    memset(v, 0, sizeof *v);
    v->dev.ops = &VIDEO_OPS;
    v->dev.host = host;
    v->dev.name = "hc88video";
    v->vram = vram;
    v->ticks_per_dot = ticks_per_dot ? ticks_per_dot : 1u;
    v->total = DOTS * LINES;
    v->start_dot = v->total - 1u;
    v->window_origin = (WINDOW_LINE * DOTS + WINDOW_DOT) % v->total;
    for (unsigned i = 0; i < 16u; ++i) {
        const uint32_t level = i & 8u ? 0xFFu : 0xCDu;
        v->palette[i] = ((i & 2u) ? level << 16 : 0u) | ((i & 4u) ? level << 8 : 0u) | ((i & 1u) ? level : 0u);
    }
    const uint64_t now = host ? zr_dev_now(&v->dev) : 0u;
    v->drawn = (now + v->ticks_per_dot - 1u) / v->ticks_per_dot;
    video_reset(v);
}

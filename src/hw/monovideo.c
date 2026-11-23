/* ZetRo - the aMIC's and PRAE-M's monochrome bitmap video (monovideo.h). Written from the aMIC manual (I, 3.6: the 640-dot line of 64
 * microseconds with 51.2 of them active, the modulo-312 frame, the serializer and the inversion bit of the 8255), and from this
 * project's rtl/rp_mono_video.sv and rp_native_machine.sv (the address, the blanking, the pixel polarity, the PRAE's interrupt).
 * ZetRo's own code. */
#include "monovideo.h"
#include "machine.h"
#include "state.h"

#include <string.h>

#define W ZR_MONOVIDEO_WIDTH
#define H ZR_MONOVIDEO_HEIGHT
#define PIXELS ZR_MONOVIDEO_PIXELS
#define LINES ZR_MONOVIDEO_LINES
#define PICTURE ZR_MONOVIDEO_PICTURE
#define STATE_VERSION 1u

/* The raster counter of the first pixel of the frame as the monitor scans it: the scan line of raster line 296 (16 lines above the
 * picture, through the wrap of the counter) begins 32 pixels before the line's own start, at pixel 288 of the line before it. */
#define WINDOW_LINE 295u
#define WINDOW_PIXEL 288u
/* The raster position the frame interrupt starts at: pixel 0 of line 256. */
#define INT_LINE 256u

/* `n` raster pixels that stay on one raster line and one scan line of the frame, from raster pixel (x, y) and frame pixel (wx, wy),
 * wy < H: the picture's pixels from the bitmap, the blanking dark. */
static void draw_run(zr_monovideo *v, uint32_t x, uint32_t y, uint32_t wx, uint32_t wy, uint32_t n) {
    uint8_t *row = v->pixels + (size_t)wy * W + wx;
    const unsigned invert = v->invert;
    if (y >= PICTURE) {
        for (uint32_t i = 0; i < n; ++i)
            if (row[i]) {
                row[i] = 0;
                v->changed = 1;
            }
        return;
    }
    const uint8_t *line = v->vram + y * (PICTURE / 8u);
    for (uint32_t i = 0; i < n; ++i, ++x) {
        const uint8_t colour = x < PICTURE ? (uint8_t)(((line[x >> 3] >> (7u - (x & 7u)) & 1u) ^ invert ^ 1u) & 1u) : 0u;
        if (row[i] != colour) {
            row[i] = colour;
            v->changed = 1;
        }
    }
}

void zr_monovideo_position(const zr_monovideo *v, uint64_t t, unsigned *line, unsigned *pixel) {
    const uint64_t counter = (t / v->ticks_per_pixel + v->start_pixel) % v->total;
    if (line) *line = (unsigned)(counter / PIXELS);
    if (pixel) *pixel = (unsigned)(counter % PIXELS);
}

uint64_t zr_monovideo_frame_number(const zr_monovideo *v, uint64_t t) {
    return (t / v->ticks_per_pixel + v->start_pixel) / v->total;
}

void zr_monovideo_catch_up(zr_monovideo *v, uint64_t t) {
    const uint64_t limit = (t + v->ticks_per_pixel - 1u) / v->ticks_per_pixel;
    while (v->drawn < limit) {
        const uint64_t at = v->drawn + v->start_pixel;
        const uint32_t counter = (uint32_t)(at % v->total);
        const uint32_t window = (counter + v->total - v->window_origin) % v->total;
        const uint32_t x = counter % PIXELS, wx = window % PIXELS, wy = window / PIXELS;
        uint64_t n = limit - v->drawn;
        if (n > v->total - counter) n = v->total - counter;
        if (n > PIXELS - x) n = PIXELS - x;
        if (n > PIXELS - wx) n = PIXELS - wx;
        if (wy < H) draw_run(v, x, counter / PIXELS, wx, wy, (uint32_t)n);
        v->drawn += n;
    }
    if (v->changed) {
        v->changed = 0;
        ++v->serial;
    }
}

void zr_monovideo_set_invert(zr_monovideo *v, int on) { v->invert = on ? 1u : 0u; }

void zr_monovideo_set_colour(zr_monovideo *v, uint32_t rgb) {
    if (v->palette[1] == (rgb & 0xFFFFFFu)) return;
    v->palette[1] = rgb & 0xFFFFFFu;
    ++v->serial;
}

int zr_monovideo_frame(const zr_monovideo *v, zr_frame *f) {
    f->width = W;
    f->height = H;
    f->pitch = W;
    f->pixels = v->pixels;
    f->palette = v->palette;
    f->colors = 2;
    f->aspect_w = 4;
    f->aspect_h = 3;
    f->serial = v->serial;
    f->picture_x = 32;
    f->picture_y = 16;
    f->picture_w = ZR_MONOVIDEO_PICTURE;
    f->picture_h = ZR_MONOVIDEO_PICTURE;
    return 1;
}

/* --- the frame interrupt ------------------------------------------------------------------ */

/* Guest time of the first assertion at or after `now`: frame f asserts when the counter reads pixel 0 of line 256, which it does
 * at pixel period f * total + INT_LINE * PIXELS - start_pixel counted from guest time 0 (the counter is start_pixel then). */
static uint64_t first_assert(const zr_monovideo *v, uint64_t now) {
    const int64_t total = (int64_t)v->total;
    const int64_t base = (int64_t)INT_LINE * PIXELS - (int64_t)v->start_pixel;      /* negative: frame 0's is before time 0 */
    const int64_t first = (int64_t)((now + v->ticks_per_pixel - 1u) / v->ticks_per_pixel);
    int64_t need = first - base;
    if (need < -base) need = -base;
    const int64_t f = need <= 0 ? 0 : (need + total - 1) / total;
    return (uint64_t)((f * total + base) * (int64_t)v->ticks_per_pixel);
}

static uint64_t int_hold(const zr_monovideo *v) { return (uint64_t)ZR_MONOVIDEO_INT_PIXELS * v->ticks_per_pixel; }

static void arm(zr_monovideo *v) {
    if (v->dev.host && v->interrupt) zr_dev_schedule(&v->dev, v->int_at);
}

/* The deadline: the interrupt goes up, or comes down and is set for the next frame. Late is handled in order. */
static void video_event(void *d, uint64_t now) {
    zr_monovideo *v = d;
    if (!v->interrupt) return;
    for (;;) {
        const uint64_t hold = int_hold(v);
        if (!v->int_on) {
            if (v->int_at > now) break;
            v->int_on = 1;
            zr_dev_line(&v->dev, ZR_MONOVIDEO_INT, 1);
            v->int_at += hold;                  /* now the time of the release */
        } else {
            if (v->int_at > now) break;
            v->int_on = 0;
            zr_dev_line(&v->dev, ZR_MONOVIDEO_INT, 0);
            v->int_at += zr_monovideo_frame_ticks(v) - hold;    /* the next assertion */
        }
    }
    arm(v);
}

/* --- the dev ------------------------------------------------------------------------------- */

static uint8_t video_read(void *d, unsigned space, uint32_t offset) { (void)d; (void)space; (void)offset; return 0xFFu; }
static void video_write(void *d, unsigned space, uint32_t offset, uint8_t value) { (void)d; (void)space; (void)offset; (void)value; }

static void video_reset(void *d) {
    zr_monovideo *v = d;
    const uint64_t now = v->dev.host ? zr_dev_now(&v->dev) : 0u;
    if (v->int_on) {
        v->int_on = 0;
        if (v->interrupt) zr_dev_line(&v->dev, ZR_MONOVIDEO_INT, 0);
    }
    v->invert = 0;
    v->int_at = first_assert(v, now);
    arm(v);
}

static void video_save(const void *d, struct zr_state_writer *w) {
    const zr_monovideo *v = d;
    zr_sw_u8(w, STATE_VERSION);
    zr_sw_u8(w, v->invert);
    zr_sw_u64(w, v->drawn);
    zr_sw_u64(w, v->int_at);
    zr_sw_u8(w, v->int_on);
    zr_sw_u64(w, v->serial);
    zr_sw_packed(w, v->pixels, sizeof v->pixels);
}

static int video_load(void *d, struct zr_state_reader *r) {
    zr_monovideo *v = d;
    if (zr_sr_u8(r) != STATE_VERSION) return 0;
    v->invert = zr_sr_u8(r) & 1u;
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

void zr_monovideo_init(zr_monovideo *v, const zr_hw_host *host, uint32_t ticks_per_pixel, const uint8_t *vram, int interrupt) {
    memset(v, 0, sizeof *v);
    v->dev.ops = &VIDEO_OPS;
    v->dev.host = host;
    v->dev.name = "monovideo";
    v->vram = vram;
    v->interrupt = interrupt ? 1u : 0u;
    v->ticks_per_pixel = ticks_per_pixel ? ticks_per_pixel : 1u;
    v->total = PIXELS * LINES;
    v->start_pixel = v->total - 1u;
    v->window_origin = (WINDOW_LINE * PIXELS + WINDOW_PIXEL) % v->total;
    v->palette[0] = 0x000000u;
    v->palette[1] = 0xFFFFFFu;
    const uint64_t now = host ? zr_dev_now(&v->dev) : 0u;
    v->drawn = (now + v->ticks_per_pixel - 1u) / v->ticks_per_pixel;
    video_reset(v);
}

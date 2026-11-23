/* ZetRo - the TIM-S Plus's video (timsvideo.h). Written from the TIM-S Plus manual (vol. III: 8.1, the ports of the
 * video, and 10.5) and this project's TIM-S core: shared/romanian/rtl/rp_tims_video.sv (the serializer, the raster,
 * the addresses, the attribute latch and its inhibit, FLASH, the blanking) and the interrupt and the FLASH source of
 * core-tims/rtl/machine_core.sv. ZetRo's own code. */
#include "timsvideo.h"
#include "machine.h"
#include "state.h"

#include <string.h>

#define W ZR_TIMSVIDEO_WIDTH
#define H ZR_TIMSVIDEO_HEIGHT
#define DOTS ZR_TIMSVIDEO_DOTS
#define LINES ZR_TIMSVIDEO_LINES
#define STATE_VERSION 1u

/* The interrupt: the core's x and y counters are registered at each dot, so they read (0, 248) once the first dot of
 * the line has been processed. */
#define INT_DOT 1u
#define INT_LINE 248u
/* The first dot of the frame as the monitor scans it: the scan line of raster line 264 (the vertical blanking, lines
 * 240-263, ends before it), which begins at dot 864 of the line before (the horizontal blanking, dots 672-863, ends
 * there). */
#define WINDOW_LINE 263u
#define WINDOW_DOT 864u
/* FLASH's frame count goes up at the start of line 248 (the falling edge of the vertical sync). */
#define FLASH_LINE 248u

/* The offset in the video RAM the serializer reads at the raster's next cell (nx, ny): the bitmap byte, or the attribute
 * (rp_tims_video.sv's pixel_address, attribute_address and page_bit, with the scroll added to the line). */
static uint32_t fetch_address(const zr_timsvideo *v, unsigned nx, unsigned ny, int attribute) {
    const unsigned sy = (ny + v->scroll) & 0x1FFu;
    const unsigned line = (sy >= 192u ? sy - 192u : sy) & 0xFFu;
    const unsigned lx = v->hires ? nx : nx >> 1;
    const unsigned column = (v->hires ? lx >> 4 : lx >> 3) & 63u;
    unsigned offset;
    if (attribute) offset = 0x1800u | (line >> 3 & 31u) << 5 | (column & 31u);
    else if (column & 32u) offset = 0x1800u | (line & 7u) << 8 | (line >> 3 & 7u) << 5 | (line >> 6 & 3u) << 3 | (column & 7u);
    else offset = (line >> 6 & 3u) << 11 | (line & 7u) << 8 | (line >> 3 & 7u) << 5 | (column & 31u);
    const unsigned page_bit = (v->page & 1u) ^ (v->hires ? lx >> 3 & 1u : 0u);
    return (uint32_t)((v->page >> 1 & 3u) << 14 | page_bit << 13 | offset);
}

uint32_t zr_timsvideo_cell_address(const zr_timsvideo *v, unsigned y, unsigned cell) {
    return fetch_address(v, v->hires ? cell * 8u : cell * 16u, y, 0);
}

/* The byte fetched at raster dot (x, y): from where the raster is a cell later (the next line's first cell at the end
 * of one). */
static uint8_t fetch(const zr_timsvideo *v, unsigned x, unsigned y, int attribute) {
    const unsigned step = v->hires ? 8u : 16u;
    unsigned nx, ny;
    if (x >= DOTS - step) {
        nx = 0;
        ny = y == LINES - 1u ? 0u : y + 1u;
    } else {
        nx = (x + step) & (v->hires ? 0x3F8u : 0x3F0u);
        ny = y;
    }
    return v->vram[fetch_address(v, nx, ny, attribute)];
}

void zr_timsvideo_position(const zr_timsvideo *v, uint64_t t, unsigned *line, unsigned *dot) {
    const uint64_t counter = (t / v->ticks_per_dot + v->start_dot) % v->total;
    if (line) *line = (unsigned)(counter / DOTS);
    if (dot) *dot = (unsigned)(counter % DOTS);
}

/* Bit 4 of the frame count at raster dot `at` (counted from guest time 0's counter, start_dot): one more at each start of
 * line 248. */
static unsigned frame_flash(const zr_timsvideo *v, uint64_t at) {
    const uint64_t first = (uint64_t)FLASH_LINE * DOTS;
    const uint64_t frames = at >= first ? (at - first) / v->total + 1u : 0u;
    return (unsigned)(frames >> 4 & 1u);
}

void zr_timsvideo_catch_up(zr_timsvideo *v, uint64_t t) {
    const uint64_t limit = (t + v->ticks_per_dot - 1u) / v->ticks_per_dot;
    if (v->drawn >= limit) return;
    uint64_t at = v->drawn + v->start_dot;
    const uint32_t counter = (uint32_t)(at % v->total);
    const uint32_t window = (counter + v->total - v->window_origin) % v->total;
    unsigned x = counter % DOTS, y = counter / DOTS, wx = window % DOTS, wy = window / DOTS;
    unsigned flash = v->flash_timer ? v->flash_level & 1u : frame_flash(v, at);
    const unsigned width = v->wide ? 640u : 512u;
    uint8_t bitmap = v->bitmap, shift = v->shift, attribute = v->attribute;
    uint8_t *row = wy < H ? v->pixels + (size_t)wy * W : NULL;
    for (uint64_t n = limit - v->drawn; n; --n) {
        /* The dot: what the registers hold as it starts. */
        if (row && wx < W) {
            uint8_t colour;
            if (y >= 240u && y < 264u) colour = 0;
            else if (y < 192u && x < width) {
                const unsigned lit = ((unsigned)shift >> 7 ^ ((unsigned)attribute >> 7 & flash)) & 1u;
                colour = (uint8_t)((lit ? attribute & 7u : (unsigned)attribute >> 3 & 7u) | (attribute & 0x40u ? 8u : 0u));
            } else colour = v->border;
            if (row[wx] != colour) {
                row[wx] = colour;
                v->changed = 1;
            }
        }
        /* The serializer's clock edge. */
        if (v->hires) {
            const unsigned phase = x & 7u;
            if (phase == 3u) bitmap = fetch(v, x, y, 0);
            if (phase == 7u) {
                shift = bitmap;
                if (!v->inhibit) attribute = fetch(v, x, y, 1);
            } else shift = (uint8_t)(shift << 1);
        } else {
            const unsigned phase = x & 15u;
            if (phase == 7u) bitmap = fetch(v, x, y, 0);
            if (phase == 15u) {
                shift = bitmap;
                if (!v->inhibit) attribute = fetch(v, x, y, 1);
            } else if (x & 1u) shift = (uint8_t)(shift << 1);
        }
        /* On to the next dot. */
        ++at;
        if (++x == DOTS) {
            x = 0;
            if (++y == LINES) y = 0;
            if (y == FLASH_LINE && !v->flash_timer) flash = frame_flash(v, at);
        }
        if (++wx == DOTS) {
            wx = 0;
            if (++wy == LINES) wy = 0;
            row = wy < H ? v->pixels + (size_t)wy * W : NULL;
        }
    }
    v->bitmap = bitmap;
    v->shift = shift;
    v->attribute = attribute;
    v->drawn = limit;
    if (v->changed) {
        v->changed = 0;
        ++v->serial;
    }
}

void zr_timsvideo_set_border(zr_timsvideo *v, unsigned colour) { v->border = (uint8_t)(colour & 7u); }
void zr_timsvideo_set_mode(zr_timsvideo *v, int wide, int hires, int inhibit) {
    v->wide = wide ? 1u : 0u;
    v->hires = hires ? 1u : 0u;
    v->inhibit = inhibit ? 1u : 0u;
}
void zr_timsvideo_set_page(zr_timsvideo *v, unsigned page) { v->page = (uint8_t)(page & 7u); }
void zr_timsvideo_set_scroll(zr_timsvideo *v, unsigned scroll) { v->scroll = (uint8_t)scroll; }
void zr_timsvideo_set_flash_source(zr_timsvideo *v, int timer) { v->flash_timer = timer ? 1u : 0u; }
void zr_timsvideo_set_flash(zr_timsvideo *v, int level) { v->flash_level = level ? 1u : 0u; }

int zr_timsvideo_frame(const zr_timsvideo *v, zr_frame *f) {
    f->width = W;
    f->height = H;
    f->pitch = W;
    f->pixels = v->pixels;
    f->palette = v->palette;
    f->colors = 16;
    f->aspect_w = 4;
    f->aspect_h = 3;
    f->serial = v->serial;
    f->picture_x = 32;
    f->picture_y = 48;
    f->picture_w = v->wide ? 640u : 512u;
    f->picture_h = 192;
    return 1;
}

/* --- the frame interrupt ------------------------------------------------------------------ */

/* Guest time of the first assertion at or after `now`: frame f asserts at raster dot f * total + INT_LINE * DOTS +
 * INT_DOT counted from guest time 0 (the counter being start_dot then). */
static uint64_t first_assert(const zr_timsvideo *v, uint64_t now) {
    const int64_t total = (int64_t)v->total;
    const int64_t base = (int64_t)INT_LINE * DOTS + INT_DOT - (int64_t)v->start_dot;
    const int64_t first = (int64_t)((now + v->ticks_per_dot - 1u) / v->ticks_per_dot);
    int64_t need = first - base;
    if (need < -base) need = -base;
    const int64_t f = need <= 0 ? 0 : (need + total - 1) / total;
    return (uint64_t)(f * total + base) * v->ticks_per_dot;
}

static uint64_t int_hold(const zr_timsvideo *v) { return (uint64_t)ZR_TIMSVIDEO_INT_DOTS * v->ticks_per_dot; }

static void arm(zr_timsvideo *v) {
    if (v->dev.host) zr_dev_schedule(&v->dev, v->int_at);
}

/* The deadline: the interrupt goes up, or comes down and is set for the next frame. Late is handled in order. */
static void video_event(void *d, uint64_t now) {
    zr_timsvideo *v = d;
    for (;;) {
        const uint64_t hold = int_hold(v);
        if (v->int_at > now) break;
        if (!v->int_on) {
            v->int_on = 1;
            zr_dev_line(&v->dev, ZR_TIMSVIDEO_INT, 1);
            v->int_at += hold;                  /* now the time of the release */
        } else {
            v->int_on = 0;
            zr_dev_line(&v->dev, ZR_TIMSVIDEO_INT, 0);
            v->int_at += zr_timsvideo_frame_ticks(v) - hold;   /* the next assertion */
        }
    }
    arm(v);
}

/* --- the dev ------------------------------------------------------------------------------- */

static uint8_t video_read(void *d, unsigned space, uint32_t offset) { (void)d; (void)space; (void)offset; return 0xFFu; }
static void video_write(void *d, unsigned space, uint32_t offset, uint8_t value) { (void)d; (void)space; (void)offset; (void)value; }

static void video_reset(void *d) {
    zr_timsvideo *v = d;
    const uint64_t now = v->dev.host ? zr_dev_now(&v->dev) : 0u;
    if (v->int_on) {
        v->int_on = 0;
        zr_dev_line(&v->dev, ZR_TIMSVIDEO_INT, 0);
    }
    v->int_at = first_assert(v, now);
    arm(v);
}

static void video_save(const void *d, struct zr_state_writer *w) {
    const zr_timsvideo *v = d;
    zr_sw_u8(w, STATE_VERSION);
    zr_sw_u8(w, v->border);
    zr_sw_u8(w, v->wide);
    zr_sw_u8(w, v->hires);
    zr_sw_u8(w, v->inhibit);
    zr_sw_u8(w, v->page);
    zr_sw_u8(w, v->scroll);
    zr_sw_u8(w, v->flash_timer);
    zr_sw_u8(w, v->flash_level);
    zr_sw_u8(w, v->bitmap);
    zr_sw_u8(w, v->shift);
    zr_sw_u8(w, v->attribute);
    zr_sw_u64(w, v->drawn);
    zr_sw_u64(w, v->int_at);
    zr_sw_u8(w, v->int_on);
    zr_sw_u64(w, v->serial);
    zr_sw_packed(w, v->pixels, sizeof v->pixels);
}

static int video_load(void *d, struct zr_state_reader *r) {
    zr_timsvideo *v = d;
    if (zr_sr_u8(r) != STATE_VERSION) return 0;
    v->border = zr_sr_u8(r) & 7u;
    v->wide = zr_sr_u8(r) & 1u;
    v->hires = zr_sr_u8(r) & 1u;
    v->inhibit = zr_sr_u8(r) & 1u;
    v->page = zr_sr_u8(r) & 7u;
    v->scroll = zr_sr_u8(r);
    v->flash_timer = zr_sr_u8(r) & 1u;
    v->flash_level = zr_sr_u8(r) & 1u;
    v->bitmap = zr_sr_u8(r);
    v->shift = zr_sr_u8(r);
    v->attribute = zr_sr_u8(r);
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

void zr_timsvideo_init(zr_timsvideo *v, const zr_hw_host *host, uint32_t ticks_per_dot, const uint8_t *vram) {
    memset(v, 0, sizeof *v);
    v->dev.ops = &VIDEO_OPS;
    v->dev.host = host;
    v->dev.name = "timsvideo";
    v->vram = vram;
    v->ticks_per_dot = ticks_per_dot ? ticks_per_dot : 1u;
    v->total = DOTS * LINES;
    v->start_dot = v->total - 1u;
    v->window_origin = (WINDOW_LINE * DOTS + WINDOW_DOT) % v->total;
    v->attribute = 7;                           /* the core's register after its reset */
    for (unsigned i = 0; i < 16u; ++i) {
        const uint32_t level = i & 8u ? 0xFFu : 0xCDu;
        v->palette[i] = ((i & 2u) ? level << 16 : 0u) | ((i & 4u) ? level << 8 : 0u) | ((i & 1u) ? level : 0u);
    }
    const uint64_t now = host ? zr_dev_now(&v->dev) : 0u;
    v->drawn = (now + v->ticks_per_dot - 1u) / v->ticks_per_dot;
    video_reset(v);
}

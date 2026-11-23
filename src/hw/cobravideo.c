/* ZetRo - the CoBra's video (cobravideo.h). Written from this project's CoBra core: rp_cobra_video.sv (the raster, the
 * serializer's addressing in both modes, the blanking and the sync) and machine_core.sv (the frame pulse, the modes).
 * ZetRo's own code. */
#include "cobravideo.h"
#include "machine.h"
#include "state.h"

#include <string.h>

#define W ZR_COBRAVIDEO_WIDTH
#define H ZR_COBRAVIDEO_HEIGHT
#define PIXELS ZR_COBRAVIDEO_PIXELS
#define LINES ZR_COBRAVIDEO_LINES
#define STATE_VERSION 1u

/* The pulse: the core's registered counters read (0, 248) once the first pixel of line 248 has been clocked. */
#define INT_PIXEL 1u
#define INT_LINE 248u
/* The first scan line shown is the one of raster line 264 (vertical blanking is 240-263). */
#define FIRST_LINE 264u

uint16_t zr_cobravideo_address(int extended, int upper, unsigned x, unsigned y, int attr) {
    unsigned page, column;
    if (extended) {
        const unsigned native = ((x & ~7u) + 512u - 32u) & 511u;
        page = native >> 8;
        column = (native >> 3) & 31u;
    } else {
        page = upper ? 1u : 0u;
        column = (x >> 3) & 31u;
    }
    const unsigned offset = attr ? 0x1800u + (y >> 3) * 32u + column
                                 : ((y & 0xC0u) << 5) | ((y & 0x07u) << 8) | ((y & 0x38u) << 2) | column;
    return (uint16_t)(page << 13 | offset);
}

static inline uint8_t picture_pixel(const zr_cobravideo *v, unsigned x, unsigned y, unsigned flash) {
    const unsigned bitmap = v->vram[zr_cobravideo_address(v->extended, v->upper, x, y, 0)];
    const unsigned attr = v->vram[zr_cobravideo_address(v->extended, v->upper, x, y, 1)];
    const unsigned lit = ((bitmap >> (7u - (x & 7u))) ^ ((attr >> 7) & flash)) & 1u;
    const unsigned colour = lit ? attr & 7u : (attr >> 3) & 7u;
    return (uint8_t)(colour | (attr & 0x40u ? 8u : 0u));
}

void zr_cobravideo_catch_up(zr_cobravideo *v, uint64_t t) {
    const uint64_t limit = (t + v->ticks_per_pixel - 1u) / v->ticks_per_pixel;
    if (v->drawn >= limit) return;
    uint64_t at = v->drawn + v->start_pixel;
    uint64_t frame = at / v->total;
    uint32_t counter = (uint32_t)(at % v->total);
    uint32_t x = counter % PIXELS, y = counter / PIXELS;
    const uint32_t width = v->extended ? 320u : 256u, wstart = v->extended ? 432u : 400u;
    for (; v->drawn < limit; ++v->drawn) {
        uint32_t sy = y, wx;
        if (x >= wstart) {
            sy = y + 1u == LINES ? 0u : y + 1u;
            wx = x - wstart;
        } else wx = x + PIXELS - wstart;
        const uint32_t wy = (sy + LINES - FIRST_LINE) % LINES;
        if (wx < W && wy < H) {
            const uint8_t colour = (y < 192u && x < width) ? picture_pixel(v, x, y, (unsigned)((frame >> 4) & 1u)) : v->border;
            uint8_t *p = v->pixels + (size_t)wy * W + wx;
            if (*p != colour) {
                *p = colour;
                v->changed = 1;
            }
        }
        if (++x == PIXELS) {
            x = 0;
            if (++y == LINES) {
                y = 0;
                ++frame;
            }
        }
    }
    if (v->changed) {
        v->changed = 0;
        ++v->serial;
    }
}

void zr_cobravideo_set_border(zr_cobravideo *v, unsigned colour) { v->border = (uint8_t)(colour & 7u); }
void zr_cobravideo_set_mode(zr_cobravideo *v, int extended, int upper) {
    v->extended = extended ? 1u : 0u;
    v->upper = upper ? 1u : 0u;
}

int zr_cobravideo_frame(const zr_cobravideo *v, zr_frame *f) {
    f->width = W;
    f->height = H;
    f->pitch = W;
    f->pixels = v->pixels;
    f->palette = v->palette;
    f->colors = 16;
    f->aspect_w = 4;
    f->aspect_h = 3;
    f->serial = v->serial;
    f->picture_x = v->extended ? 16u : 48u;
    f->picture_y = 48;
    f->picture_w = v->extended ? 320u : 256u;
    f->picture_h = 192;
    return 1;
}

/* --- the frame pulse ------------------------------------------------------------------------- */

static uint64_t first_assert(const zr_cobravideo *v, uint64_t now) {
    const int64_t total = (int64_t)v->total;
    const int64_t base = (int64_t)INT_LINE * PIXELS + INT_PIXEL - (int64_t)v->start_pixel;
    const int64_t first = (int64_t)((now + v->ticks_per_pixel - 1u) / v->ticks_per_pixel);
    const int64_t need = first - base;
    const int64_t f = need <= 0 ? 0 : (need + total - 1) / total;
    int64_t when = f * total + base;
    while (when < 0) when += total;
    return (uint64_t)when * v->ticks_per_pixel;
}

static uint64_t int_hold(const zr_cobravideo *v) { return (uint64_t)ZR_COBRAVIDEO_INT_PIXELS * v->ticks_per_pixel; }

static void arm(zr_cobravideo *v) {
    if (v->dev.host) zr_dev_schedule(&v->dev, v->int_at);
}

static void video_event(void *d, uint64_t now) {
    zr_cobravideo *v = d;
    for (;;) {
        const uint64_t hold = int_hold(v);
        if (v->int_at > now) break;
        if (!v->int_on) {
            v->int_on = 1;
            v->int_at += hold;
            zr_dev_line(&v->dev, ZR_COBRAVIDEO_INT, 1);
        } else {
            v->int_on = 0;
            v->int_at += zr_cobravideo_frame_ticks(v) - hold;
            zr_dev_line(&v->dev, ZR_COBRAVIDEO_INT, 0);
        }
    }
    arm(v);
}

static uint8_t video_read(void *d, unsigned space, uint32_t offset) { (void)d; (void)space; (void)offset; return 0xFFu; }
static void video_write(void *d, unsigned space, uint32_t offset, uint8_t value) { (void)d; (void)space; (void)offset; (void)value; }

static void video_reset(void *d) {
    zr_cobravideo *v = d;
    const uint64_t now = v->dev.host ? zr_dev_now(&v->dev) : 0u;
    v->border = v->extended = v->upper = 0;
    if (v->int_on) {
        v->int_on = 0;
        zr_dev_line(&v->dev, ZR_COBRAVIDEO_INT, 0);
    }
    v->int_at = first_assert(v, now);
    arm(v);
}

static void video_save(const void *d, struct zr_state_writer *w) {
    const zr_cobravideo *v = d;
    zr_sw_u8(w, STATE_VERSION);
    zr_sw_u8(w, v->border);
    zr_sw_u8(w, v->extended);
    zr_sw_u8(w, v->upper);
    zr_sw_u64(w, v->drawn);
    zr_sw_u64(w, v->int_at);
    zr_sw_u8(w, v->int_on);
    zr_sw_u64(w, v->serial);
    zr_sw_packed(w, v->pixels, sizeof v->pixels);
}

static int video_load(void *d, struct zr_state_reader *r) {
    zr_cobravideo *v = d;
    if (zr_sr_u8(r) != STATE_VERSION) return 0;
    v->border = zr_sr_u8(r) & 7u;
    v->extended = zr_sr_u8(r) & 1u;
    v->upper = zr_sr_u8(r) & 1u;
    v->drawn = zr_sr_u64(r);
    v->int_at = zr_sr_u64(r);
    v->int_on = zr_sr_u8(r) & 1u;
    v->serial = zr_sr_u64(r) + 1u;
    zr_sr_packed(r, v->pixels, sizeof v->pixels);
    v->changed = 0;
    arm(v);
    return zr_sr_ok(r);
}

static const zr_dev_ops VIDEO_OPS = {
    .read = video_read, .write = video_write, .event = video_event, .reset = video_reset,
    .save = video_save, .load = video_load,
};

void zr_cobravideo_init(zr_cobravideo *v, const zr_hw_host *host, uint32_t ticks_per_pixel, const uint8_t *vram) {
    memset(v, 0, sizeof *v);
    v->dev.ops = &VIDEO_OPS;
    v->dev.host = host;
    v->dev.name = "cobravideo";
    v->vram = vram;
    v->ticks_per_pixel = ticks_per_pixel ? ticks_per_pixel : 2u;
    v->total = PIXELS * LINES;
    v->start_pixel = v->total - 1u;
    for (unsigned i = 0; i < 16u; ++i) {
        const uint32_t level = i & 8u ? 0xFFu : 0xC0u;
        v->palette[i] = ((i & 2u) ? level << 16 : 0u) | ((i & 4u) ? level << 8 : 0u) | ((i & 1u) ? level : 0u);
    }
    const uint64_t now = host ? zr_dev_now(&v->dev) : 0u;
    v->drawn = (now + v->ticks_per_pixel - 1u) / v->ticks_per_pixel;
    video_reset(v);
}

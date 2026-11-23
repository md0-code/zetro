/* ZetRo - the CUB Z's video (cubzvideo.h). Written from this project's CUB Z core: cubz_video.sv (the raster, the column-major
 * addressing with the scroll offset, the enable, the blanking) and hardware.md (the dot clock, the WAIT). ZetRo's own code. */
#include "cubzvideo.h"
#include "machine.h"
#include "state.h"

#include <string.h>

#define W ZR_CUBZVIDEO_WIDTH
#define H ZR_CUBZVIDEO_HEIGHT
#define DOTS ZR_CUBZVIDEO_DOTS
#define LINES ZR_CUBZVIDEO_LINES
#define BORDER_X 16u                    /* dots of border left and right of the picture */
#define BORDER_Y 16u                    /* lines above and below */
#define STATE_VERSION 1u

void zr_cubzvideo_catch_up(zr_cubzvideo *v, uint64_t t) {
    if (v->drawn >= t) return;
    uint32_t pos = (uint32_t)(v->drawn % (DOTS * LINES));
    uint32_t x = pos % DOTS, y = pos / DOTS;
    for (; v->drawn < t; ++v->drawn) {
        /* Where the dot is in the frame: the last 16 dots of a line are the left border of the next one. */
        uint32_t wx, wy;
        if (x >= DOTS - BORDER_X) {
            wx = x - (DOTS - BORDER_X);
            wy = (y + 1u == LINES ? 0u : y + 1u) + BORDER_Y;
        } else {
            wx = x + BORDER_X;
            wy = y + BORDER_Y;
        }
        if (wy >= LINES) wy -= LINES;
        if (wx < W && wy < H) {
            uint8_t lit = 0;
            if (v->enable && x < 512u && y < 256u) lit = (uint8_t)(v->vram[zr_cubzvideo_address(x, y, v->scroll)] >> (7u - (x & 7u)) & 1u);
            uint8_t *p = v->pixels + (size_t)wy * W + wx;
            if (*p != lit) {
                *p = lit;
                v->changed = 1;
            }
        }
        if (++x == DOTS) {
            x = 0;
            if (++y == LINES) y = 0;
        }
    }
    if (v->changed) {
        v->changed = 0;
        ++v->serial;
    }
}

void zr_cubzvideo_set_scroll(zr_cubzvideo *v, uint8_t scroll) { v->scroll = scroll; }
void zr_cubzvideo_set_enable(zr_cubzvideo *v, int enable) { v->enable = enable ? 1u : 0u; }

int zr_cubzvideo_frame(const zr_cubzvideo *v, zr_frame *f) {
    f->width = W;
    f->height = H;
    f->pitch = W;
    f->pixels = v->pixels;
    f->palette = v->palette;
    f->colors = 2;
    f->aspect_w = 4;
    f->aspect_h = 3;
    f->serial = v->serial;
    f->picture_x = 16;
    f->picture_y = 16;
    f->picture_w = 512;
    f->picture_h = 256;
    return 1;
}

static uint8_t video_read(void *d, unsigned space, uint32_t offset) { (void)d; (void)space; (void)offset; return 0xFFu; }
static void video_write(void *d, unsigned space, uint32_t offset, uint8_t value) { (void)d; (void)space; (void)offset; (void)value; }
static void video_event(void *d, uint64_t now) { (void)d; (void)now; }

static void video_reset(void *d) {
    zr_cubzvideo *v = d;
    v->scroll = 0;
    v->enable = 0;
}

static void video_save(const void *d, struct zr_state_writer *w) {
    const zr_cubzvideo *v = d;
    zr_sw_u8(w, STATE_VERSION);
    zr_sw_u8(w, v->scroll);
    zr_sw_u8(w, v->enable);
    zr_sw_u64(w, v->drawn);
    zr_sw_u64(w, v->serial);
    zr_sw_packed(w, v->pixels, sizeof v->pixels);
}

static int video_load(void *d, struct zr_state_reader *r) {
    zr_cubzvideo *v = d;
    if (zr_sr_u8(r) != STATE_VERSION) return 0;
    v->scroll = zr_sr_u8(r);
    v->enable = zr_sr_u8(r) & 1u;
    v->drawn = zr_sr_u64(r);
    v->serial = zr_sr_u64(r) + 1u;
    zr_sr_packed(r, v->pixels, sizeof v->pixels);
    for (size_t i = 0; i < sizeof v->pixels; ++i) v->pixels[i] &= 1u;
    v->changed = 0;
    return zr_sr_ok(r);
}

static const zr_dev_ops VIDEO_OPS = {
    .read = video_read, .write = video_write, .event = video_event, .reset = video_reset,
    .save = video_save, .load = video_load,
};

void zr_cubzvideo_init(zr_cubzvideo *v, const zr_hw_host *host, const uint8_t *vram, unsigned monitor) {
    memset(v, 0, sizeof *v);
    v->dev.ops = &VIDEO_OPS;
    v->dev.host = host;
    v->dev.name = "cubzvideo";
    v->vram = vram;
    v->palette[0] = 0x000000u;
    /* The core's monitor colours (CUBZ.sv): white, or green 30FF30h. */
    v->palette[1] = monitor == ZR_CUBZVIDEO_GREEN ? 0x30FF30u : 0xFFFFFFu;
    v->drawn = host ? zr_dev_now(&v->dev) : 0u;
    video_reset(v);
}

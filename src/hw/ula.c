/* ZetRo - the Spectrum-compatible video and keyboard (see ula.h). Written
 * from the HC-85 manual's display and keyboard sections as this project's
 * own RTL (rp_spectrum_video.sv, rp_spectrum_keyboard.sv) and the JET and
 * HC-85 hardware notes describe them. ZetRo's own code. */
#include "ula.h"
#include "machine.h"
#include "state.h"

#include <string.h>

#define W ZR_ULA_WIDTH
#define H ZR_ULA_HEIGHT
#define STATE_VERSION 1u

void zr_ula_default_config(zr_ula_config *c) {
    memset(c, 0, sizeof *c);
    c->ticks_per_pixel = 2;                 /* a 7 MHz pixel from the 14 MHz master clock */
    c->ticks_per_tstate = 4;                /* the 3.5 MHz CPU */
    c->pixels_per_line = 448;
    c->lines_per_frame = 312;
    c->start_pixel = 448u * 312u - 1u;      /* the core resets its counters to the last pixel of the frame */
    c->picture_x = 0;
    c->picture_y = 0;
    c->int_x = 0;
    c->int_y = 248;
    c->int_tstates = 32;
    c->flash_frames = 16;
    c->bright = 1;
    c->level_normal = 0xCD;
    c->level_bright = 0xFF;
}

/* The Sinclair machines: T-states in a line, lines, the first contended T-state
 * and the interrupt's length. 48K: the comp.sys.sinclair FAQ's 48K reference (224 x 312 = 69888, 14335); 128K: its 128K
 * reference (228 x 311 = 70908, 14361), the interrupt of 36 T-states after the emulation notes that give it (Chris
 * Smith's 32 is the 48K's). The fetches begin 3 T-states after the first contended one (the Sinclair Wiki's floating-bus
 * table: 14338 and 14364); a cell is shown 4 T-states after its fetch began, which puts the first byte of the picture
 * where the FAQ's border changes show it (an OUT ending 14339-14342 after the interrupt changes the border at the
 * picture's first byte on the 48K, 14365-14368 on the 128K). */
void zr_ula_sinclair_config(zr_ula_config *c, unsigned which) {
    uint32_t per_line, lines, first, hold;
    if (which == ZR_ULA_SINCLAIR_48K) { per_line = 224; lines = 312; first = 14335; hold = 32; }
    else if (which == ZR_ULA_SINCLAIR_128K) { per_line = 228; lines = 311; first = 14361; hold = 36; }
    else return;
    if (!c->ticks_per_pixel) c->ticks_per_pixel = 2;
    if (!c->ticks_per_tstate) c->ticks_per_tstate = 2 * c->ticks_per_pixel;
    const uint32_t per_tstate = c->ticks_per_tstate / c->ticks_per_pixel;   /* pixels: 2 */
    c->sinclair = (uint8_t)which;
    c->pixels_per_line = per_line * per_tstate;
    c->lines_per_frame = lines;
    c->contention_start = first;
    c->int_x = c->int_y = 0;
    c->int_tstates = hold;
    const uint32_t picture = (first + 7u) * per_tstate;
    c->picture_x = picture % c->pixels_per_line;
    c->picture_y = picture / c->pixels_per_line;
    /* Guest time 0 on the line after the visible frame's last, so that a frame the shell shows is one frame of the ULA. */
    c->start_pixel = (c->picture_y + 240u) * c->pixels_per_line;
}

/* --- the raster -------------------------------------------------------------------------- */

void zr_ula_position(const zr_ula *u, uint64_t t, unsigned *line, unsigned *pixel) {
    const uint64_t counter = (t / u->cfg.ticks_per_pixel + u->cfg.start_pixel) % u->total;
    if (line) *line = (unsigned)(counter / u->cfg.pixels_per_line);
    if (pixel) *pixel = (unsigned)(counter % u->cfg.pixels_per_line);
}

uint64_t zr_ula_frame_number(const zr_ula *u, uint64_t t) {
    return (t / u->cfg.ticks_per_pixel + u->cfg.start_pixel) / u->total;
}

/* --- Sinclair timing ------------------------------------------------------------------------- */

static uint16_t bitmap_offset(unsigned y, unsigned cell);
static uint16_t attribute_offset(unsigned y, unsigned cell);

uint32_t zr_ula_tstate(const zr_ula *u, uint64_t t) {
    const uint64_t counter = (t / u->cfg.ticks_per_pixel + u->cfg.start_pixel) % u->total;
    const uint64_t pixel = (counter + u->total - u->int_counter) % u->total;
    return (uint32_t)(pixel * u->cfg.ticks_per_pixel / u->cfg.ticks_per_tstate);
}

uint8_t zr_ula_floating(const zr_ula *u, uint64_t t) {
    const uint32_t tstate = zr_ula_tstate(u, t), first = u->cfg.contention_start + 3u;
    if (tstate < first) return 0xFFu;
    const uint32_t line = (tstate - first) / u->tstates_per_line, x = (tstate - first) % u->tstates_per_line;
    if (line >= 192u || x >= 128u || (x & 7u) >= 4u) return 0xFFu;
    const unsigned cell = (x >> 3) * 2u + ((x & 7u) >> 1);
    return u->vram(u->vram_ctx, x & 1u ? attribute_offset(line, cell) : bitmap_offset(line, cell));
}

/* The delays of the frame: 6,5,4,3,2,1,0,0 from the first contended T-state, for 128 T-states of each picture line. */
static void build_contention(zr_ula *u) {
    static const uint8_t PATTERN[8] = {6, 5, 4, 3, 2, 1, 0, 0};
    memset(u->contention, 0, sizeof u->contention);
    if (!u->cfg.sinclair) return;
    for (uint32_t line = 0; line < 192u; ++line)
        for (uint32_t x = 0; x < 128u; ++x) {
            const uint32_t t = u->cfg.contention_start + line * u->tstates_per_line + x;
            if (t < u->tstates && t < ZR_ULA_SINCLAIR_TSTATES) u->contention[t] = PATTERN[x & 7u];
        }
}

/* --- drawing ----------------------------------------------------------------------------- */

static uint16_t bitmap_offset(unsigned y, unsigned cell) {
    return (uint16_t)(((y & 0xC0u) << 5) | ((y & 0x07u) << 8) | ((y & 0x38u) << 2) | cell);
}

static uint16_t attribute_offset(unsigned y, unsigned cell) {
    return (uint16_t)(0x1800u + (y >> 3) * 32u + cell);
}

/* `n` raster pixels that stay on one picture line and one scan line of the
 * frame: from picture pixel (px, py) and frame pixel (wx, wy). */
static void draw_run(zr_ula *u, uint32_t px, uint32_t py, uint32_t wx, uint32_t wy, uint32_t n, unsigned flash) {
    if (wy >= H) return;                    /* blanking: nothing is shown, and the picture is never there */
    const zr_ula_config *c = &u->cfg;
    const uint32_t ppl = c->pixels_per_line;
    const uint32_t next_row = (py + 1u) % c->lines_per_frame;
    uint8_t *row = u->pixels + (size_t)wy * W;
    for (uint32_t i = 0; i < n; i++, px++, wx++) {
        /* The pixel on the screen, from the registers as the last pixel left them. */
        uint8_t colour = u->border;
        if (py < 192u && px < 256u) {
            const unsigned lit = ((unsigned)(u->shift >> 7) ^ ((u->attr >> 7) & flash)) & 1u;
            colour = lit ? (u->attr & 7u) : ((u->attr >> 3) & 7u);
            if (c->bright && (u->attr & 0x40u)) colour |= 8u;
        }
        if (wx < W && row[wx] != colour) {
            row[wx] = colour;
            u->changed = 1;
        }
        /* The pipeline: the bitmap byte of the next cell is read at pixel 3 of a cell, its attribute at
         * pixel 7, when the shift register takes the bitmap; the next cell of the last cell of a line
         * is the first of the next picture line. */
        const unsigned phase = px & 7u;
        if (phase == 3u || phase == 7u) {
            unsigned cell, y;
            if (px >= ppl - 8u) { cell = 0; y = next_row; }
            else { cell = (px >> 3) + 1u; y = py; }
            const int shown = cell < 32u && y < 192u;
            if (phase == 3u) {
                if (shown) u->bitmap = u->vram(u->vram_ctx, bitmap_offset(y, cell));
            } else {
                u->shift = u->bitmap;
                if (shown) u->attr = u->vram(u->vram_ctx, attribute_offset(y, cell));
                continue;
            }
        }
        u->shift = (uint8_t)(u->shift << 1);
    }
}

/* The same with Sinclair timing. The picture's coordinates count from the first pixel shown: a pair of cells (2k, 2k+1)
 * is fetched in the four T-states from 8 pixels before cell 2k is shown (bitmap, attribute, bitmap, attribute, one
 * T-state each: 2 pixels) into latches; cell 2k is shown from its latches at pixel 16k, cell 2k+1 at 16k + 8. The border
 * is taken at the first pixel of each 8-pixel unit. */
static void draw_run_sinclair(zr_ula *u, uint32_t px, uint32_t py, uint32_t wx, uint32_t wy, uint32_t n, unsigned flash) {
    const zr_ula_config *c = &u->cfg;
    const uint32_t ppl = c->pixels_per_line;
    if (wy >= H) {                          /* nothing shown and nothing fetched; the border is taken all the same */
        if (!(px & 7u) || (px & 7u) + n > 8u) u->border_shown = u->border;
        return;
    }
    const uint32_t next_row = (py + 1u) % c->lines_per_frame;
    uint8_t *row = u->pixels + (size_t)wy * W;
    for (uint32_t i = 0; i < n; i++, px++, wx++) {
        /* A fetch at this pixel: 8 pixels before a pair's first, every other pixel for four. */
        const uint32_t ahead = px >= ppl - 8u ? px + 8u - ppl : px + 8u;
        const uint32_t y = px >= ppl - 8u ? next_row : py;
        if (ahead < 256u && !(ahead & 1u) && (ahead & 15u) < 8u && y < 192u) {
            const unsigned slot = (ahead & 7u) >> 1, cell = (ahead >> 4) * 2u + (slot >> 1);
            u->fetched[slot] = u->vram(u->vram_ctx, slot & 1u ? attribute_offset(y, cell) : bitmap_offset(y, cell));
        }
        const int picture = py < 192u && px < 256u;
        if (!(px & 7u)) {
            u->border_shown = u->border;
            if (picture) {
                const unsigned k = (px >> 3) & 1u;
                u->shift = u->fetched[2u * k];
                u->attr = u->fetched[2u * k + 1u];
            }
        }
        uint8_t colour = u->border_shown;
        if (picture) {
            const unsigned lit = ((unsigned)(u->shift >> 7) ^ ((u->attr >> 7) & flash)) & 1u;
            colour = lit ? (u->attr & 7u) : ((u->attr >> 3) & 7u);
            if (c->bright && (u->attr & 0x40u)) colour |= 8u;
            u->shift = (uint8_t)(u->shift << 1);
        }
        if (wx < W && row[wx] != colour) {
            row[wx] = colour;
            u->changed = 1;
        }
    }
}

void zr_ula_catch_up(zr_ula *u, uint64_t t) {
    const zr_ula_config *c = &u->cfg;
    const uint32_t ppl = c->pixels_per_line;
    const uint64_t limit = (t + c->ticks_per_pixel - 1u) / c->ticks_per_pixel;
    while (u->drawn < limit) {
        const uint64_t at = u->drawn + c->start_pixel;
        const uint64_t frame = at / u->total;
        const uint32_t counter = (uint32_t)(at % u->total);
        const uint32_t q = (counter + u->total - u->picture_origin) % u->total;     /* in picture coordinates */
        const uint32_t v = (counter + u->total - u->window_origin) % u->total;      /* in the frame's */
        uint64_t n = limit - u->drawn;
        if (n > u->total - counter) n = u->total - counter;
        if (n > ppl - q % ppl) n = ppl - q % ppl;
        if (n > ppl - v % ppl) n = ppl - v % ppl;
        const unsigned flash = c->flash_frames ? (unsigned)((frame / c->flash_frames) & 1u) : 0u;
        if (c->sinclair) draw_run_sinclair(u, q % ppl, q / ppl, v % ppl, v / ppl, (uint32_t)n, flash);
        else draw_run(u, q % ppl, q / ppl, v % ppl, v / ppl, (uint32_t)n, flash);
        u->drawn += n;
    }
    if (u->changed) {
        u->changed = 0;
        ++u->serial;
    }
}

void zr_ula_set_border(zr_ula *u, unsigned colour) { u->border = (uint8_t)(colour & 7u); }

int zr_ula_frame(const zr_ula *u, zr_frame *f) {
    f->width = W;
    f->height = H;
    f->pitch = W;
    f->pixels = u->pixels;
    f->palette = u->palette;
    f->colors = 16;
    f->aspect_w = 4;
    f->aspect_h = 3;
    f->serial = u->serial;
    f->picture_x = ZR_ULA_PICTURE_X;
    f->picture_y = ZR_ULA_PICTURE_Y;
    f->picture_w = 256;
    f->picture_h = 192;
    return 1;
}

/* --- the keyboard ------------------------------------------------------------------------ */

void zr_ula_key(zr_ula *u, unsigned row, unsigned column, int down) {
    if (row >= 8u || column >= 5u) return;
    if (down) u->keys[row] |= (uint8_t)(1u << column);
    else u->keys[row] &= (uint8_t)~(1u << column);
}

uint8_t zr_ula_keys(const zr_ula *u, uint8_t high) {
    uint8_t bits = 0x1Fu;
    for (unsigned r = 0; r < 8u; r++)
        if (!((high >> r) & 1u)) bits &= (uint8_t)~u->keys[r];
    return bits & 0x1Fu;
}

/* --- the frame interrupt ----------------------------------------------------------------- */

/* Guest time of the first assertion at or after `now`: frame f asserts at
 * raster pixel f * total + int_y * pixels_per_line + int_x, counted from
 * guest time 0 (the counter being start_pixel then). */
static uint64_t first_assert(const zr_ula *u, uint64_t now) {
    const zr_ula_config *c = &u->cfg;
    const int64_t total = (int64_t)u->total;
    const int64_t base = (int64_t)c->int_y * (int64_t)c->pixels_per_line + (int64_t)c->int_x - (int64_t)c->start_pixel;
    const int64_t first = (int64_t)((now + c->ticks_per_pixel - 1u) / c->ticks_per_pixel);
    int64_t need = first - base;
    if (need < -base) need = -base;
    const int64_t f = need <= 0 ? 0 : (need + total - 1) / total;
    return (uint64_t)(f * total + base) * c->ticks_per_pixel;
}

static uint64_t int_hold(const zr_ula *u) { return (uint64_t)u->cfg.int_tstates * u->cfg.ticks_per_tstate; }

static void arm(zr_ula *u) {
    if (u->dev.host) zr_dev_schedule(&u->dev, u->int_at);
}

/* The deadline: the interrupt goes up, or comes down and is set for the next frame. Late is handled in order. */
static void ula_event(void *d, uint64_t now) {
    zr_ula *u = d;
    for (;;) {
        const uint64_t hold = int_hold(u);
        if (!u->int_on) {
            if (u->int_at > now) break;
            u->int_on = 1;
            zr_dev_line(&u->dev, ZR_ULA_INT, 1);
            u->int_at += hold;                  /* now the time of the release */
        } else {
            if (u->int_at > now) break;
            u->int_on = 0;
            zr_dev_line(&u->dev, ZR_ULA_INT, 0);
            u->int_at += zr_ula_frame_ticks(u) - hold;      /* the next assertion */
        }
    }
    arm(u);
}

/* --- the dev ----------------------------------------------------------------------------- */

static uint8_t ula_read(void *d, unsigned space, uint32_t offset) { (void)d; (void)space; (void)offset; return 0xFFu; }
static void ula_write(void *d, unsigned space, uint32_t offset, uint8_t value) { (void)d; (void)space; (void)offset; (void)value; }

static void ula_reset(void *d) {
    zr_ula *u = d;
    const uint64_t now = u->dev.host ? zr_dev_now(&u->dev) : 0u;
    if (u->int_on) {
        u->int_on = 0;
        zr_dev_line(&u->dev, ZR_ULA_INT, 0);
    }
    u->border = u->bitmap = u->shift = u->attr = u->border_shown = 0;
    memset(u->fetched, 0, sizeof u->fetched);
    u->int_at = first_assert(u, now);
    arm(u);
}

static void ula_save(const void *d, struct zr_state_writer *w) {
    const zr_ula *u = d;
    zr_sw_u8(w, STATE_VERSION);
    zr_sw_u8(w, u->border);
    zr_sw_u8(w, u->bitmap);
    zr_sw_u8(w, u->shift);
    zr_sw_u8(w, u->attr);
    zr_sw_bytes(w, u->keys, sizeof u->keys);
    zr_sw_u64(w, u->drawn);
    zr_sw_u64(w, u->int_at);
    zr_sw_u8(w, u->int_on);
    zr_sw_u64(w, u->serial);
    zr_sw_packed(w, u->pixels, sizeof u->pixels);
    if (u->cfg.sinclair) {
        zr_sw_u8(w, u->cfg.sinclair);
        zr_sw_bytes(w, u->fetched, sizeof u->fetched);
        zr_sw_u8(w, u->border_shown);
    }
}

static int ula_load(void *d, struct zr_state_reader *r) {
    zr_ula *u = d;
    if (zr_sr_u8(r) != STATE_VERSION) return 0;
    u->border = zr_sr_u8(r) & 7u;
    u->bitmap = zr_sr_u8(r);
    u->shift = zr_sr_u8(r);
    u->attr = zr_sr_u8(r);
    zr_sr_bytes(r, u->keys, sizeof u->keys);
    for (unsigned i = 0; i < 8u; i++) u->keys[i] &= 0x1Fu;
    u->drawn = zr_sr_u64(r);
    u->int_at = zr_sr_u64(r);
    u->int_on = zr_sr_u8(r) & 1u;
    u->serial = zr_sr_u64(r) + 1u;              /* what the shell shows is not this frame */
    zr_sr_packed(r, u->pixels, sizeof u->pixels);
    if (u->cfg.sinclair) {
        /* A state of the other timing has another raster: refused. */
        if (zr_sr_u8(r) != u->cfg.sinclair) return 0;
        zr_sr_bytes(r, u->fetched, sizeof u->fetched);
        u->border_shown = zr_sr_u8(r) & 7u;
    }
    u->changed = 0;
    arm(u);
    return zr_sr_ok(r);
}

static const zr_dev_ops ULA_OPS = {
    .read = ula_read, .write = ula_write, .event = ula_event, .reset = ula_reset,
    .save = ula_save, .load = ula_load,
};

void zr_ula_init(zr_ula *u, const zr_hw_host *host, const zr_ula_config *config, uint8_t (*vram)(void *ctx, uint16_t offset), void *vram_ctx) {
    memset(u, 0, sizeof *u);
    u->dev.ops = &ULA_OPS;
    u->dev.host = host;
    u->dev.name = "ula";
    if (config) u->cfg = *config;
    else zr_ula_default_config(&u->cfg);
    u->vram = vram;
    u->vram_ctx = vram_ctx;
    const zr_ula_config *c = &u->cfg;
    u->total = c->pixels_per_line * c->lines_per_frame;
    u->picture_origin = (c->picture_y * c->pixels_per_line + c->picture_x) % u->total;
    u->window_origin = (u->picture_origin + u->total - ZR_ULA_PICTURE_Y * c->pixels_per_line - ZR_ULA_PICTURE_X) % u->total;
    u->int_counter = (c->int_y * c->pixels_per_line + c->int_x) % u->total;
    u->tstates = (uint32_t)((uint64_t)u->total * c->ticks_per_pixel / c->ticks_per_tstate);
    u->tstates_per_line = c->pixels_per_line * c->ticks_per_pixel / c->ticks_per_tstate;
    build_contention(u);
    for (unsigned i = 0; i < 16u; i++) {
        const uint32_t level = i & 8u ? c->level_bright : c->level_normal;
        u->palette[i] = ((i & 2u) ? level << 16 : 0u) | ((i & 4u) ? level << 8 : 0u) | ((i & 1u) ? level : 0u);
    }
    const uint64_t now = host ? zr_dev_now(&u->dev) : 0u;
    u->drawn = (now + c->ticks_per_pixel - 1u) / c->ticks_per_pixel;
    ula_reset(u);
}

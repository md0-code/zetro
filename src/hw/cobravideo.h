/* ZetRo - the CoBra's video (this project's CoBra core: MiSTerRO/docs/cobra/hardware.md, core-cobra/rtl/machine_core.sv and
 * the shared rp_cobra_video.sv; the reconstructed schematic's video sheets): the Spectrum-compatible serializer of the
 * HC-85 family with the CoBra's 80-column modification, reading a 16 KiB video memory the board gives (the 80K's own
 * video RAM, or the 64K's main RAM at physical 4000h-7FFFh).
 *
 * Raster: a pixel is two ticks of the 14 MHz master clock (7 MHz), 448 pixels a line, 312 lines a frame (69888 CPU
 * T-states, 50.08 Hz), the picture at the raster's origin (pixel 0, line 0), 192 lines high and 256 pixels wide, or 320
 * in the extended (80-column) mode. Horizontal blanking is pixels 304-399 (extended 336-431), vertical lines 240-263;
 * the rest of the raster is border. The frame shows 352 x 288 pixels as the monitor scans them: the scan line starts
 * 48 pixels before the picture (16 in the extended mode, whose sync comes 32 pixels later: the wider picture extends
 * the normal one by 32 pixels on each side on the screen), its left border from the end of the raster line before.
 *
 * The picture: each 8-pixel cell is a bitmap byte (bit 7 first) and its attribute, the Spectrum's (ink 0-2, paper 3-5,
 * BRIGHT 6, FLASH 7, which swaps ink and paper for 16 frames in 32), read from an 8 KiB screen of the video memory: the
 * bitmap in the Spectrum's interleaved order at 0000h-17FFh, the attributes at 1800h-1AFFh. Normal mode reads the screen
 * `upper` names (0: 0000h, 1: 2000h; the board sets it in CP/M). The extended mode reads cell c of a line at the
 * native column x = 8c - 32 (modulo 512): its screen is bit 8 of x, its column bits 7-3 of x, so the 32 pixels left of
 * the normal picture are columns 28-31 of the upper screen, the middle 256 the lower screen, the 32 right of it columns
 * 0-3 of the upper screen. The border is the colour of port C's low three bits, never bright. Colour index 0-7 as the
 * colour bits (bit 0 blue, 1 red, 2 green), 8-15 their BRIGHT versions (levels 0xC0 and 0xFF, the core's).
 *
 * Drawing is incremental and in step with the guest (ula.h's way): zr_cobravideo_catch_up(t) draws every pixel that
 * starts before guest time t reading the video memory as it is then, so the board calls it before the memory, the
 * border or the mode changes, and once more at the end of each run. The raster is a function of guest time alone: the
 * counter is at the last pixel of a frame at guest time 0 (the core resets its counters so) and nothing resets it.
 *
 * The frame pulse (VINT) is the output pin ZR_COBRAVIDEO_INT of host->line: high from pixel 0 of line 248 for 64 pixels
 * (32 CPU T-states), every frame, from the chip's single deadline. The board wires it where it goes.
 *
 * Portable C11. ZetRo's own code, written from rp_cobra_video.sv and machine_core.sv. */
#ifndef ZR_HW_COBRAVIDEO_H
#define ZR_HW_COBRAVIDEO_H

#include "hw.h"

struct zr_frame;

enum { ZR_COBRAVIDEO_INT };

#define ZR_COBRAVIDEO_PIXELS 448            /* pixels in a raster line */
#define ZR_COBRAVIDEO_LINES 312
#define ZR_COBRAVIDEO_WIDTH 352             /* the frame as shown */
#define ZR_COBRAVIDEO_HEIGHT 288
#define ZR_COBRAVIDEO_VRAM 0x4000u
#define ZR_COBRAVIDEO_INT_PIXELS 64

typedef struct zr_cobravideo {
    zr_dev dev;
    const uint8_t *vram;                    /* the ZR_COBRAVIDEO_VRAM bytes the serializer reads (the board's) */
    uint32_t ticks_per_pixel;               /* 2 */
    uint32_t total;                         /* pixels in a frame */
    uint32_t start_pixel;                   /* the raster counter at guest time 0 */
    uint8_t border, extended, upper, changed;
    uint64_t drawn;                         /* raster pixels drawn since guest time 0 */
    uint64_t int_at;                        /* guest time of the pulse's next rise, or of its fall while high */
    uint8_t int_on;
    uint64_t serial;
    uint32_t palette[16];
    uint8_t pixels[ZR_COBRAVIDEO_WIDTH * ZR_COBRAVIDEO_HEIGHT];
} zr_cobravideo;

/* Sets the video on `host` (its deadline armed here) and resets it; it reads `vram` whenever it draws. */
void zr_cobravideo_init(zr_cobravideo *v, const zr_hw_host *host, uint32_t ticks_per_pixel, const uint8_t *vram);
/* The memory the serializer reads (a board that switches it calls catch_up first). */
static inline void zr_cobravideo_set_vram(zr_cobravideo *v, const uint8_t *vram) { v->vram = vram; }
/* The border colour, the extended (320-pixel) mode and the screen of normal mode: call catch_up first. */
void zr_cobravideo_set_border(zr_cobravideo *v, unsigned colour);
void zr_cobravideo_set_mode(zr_cobravideo *v, int extended, int upper);
void zr_cobravideo_catch_up(zr_cobravideo *v, uint64_t t);
/* The frame as drawn: 352 x 288 indexed pixels, 16 colours, aspect 4:3. Always 1. */
int zr_cobravideo_frame(const zr_cobravideo *v, struct zr_frame *frame);
static inline uint64_t zr_cobravideo_frame_ticks(const zr_cobravideo *v) { return (uint64_t)v->total * v->ticks_per_pixel; }
/* The level of the frame pulse now. */
static inline int zr_cobravideo_int(const zr_cobravideo *v) { return v->int_on; }
/* The video memory offset of the bitmap byte (attribute: attr 1) the serializer reads for picture pixel x of line y in
 * the given mode (x < 256, or < 320 extended). */
uint16_t zr_cobravideo_address(int extended, int upper, unsigned x, unsigned y, int attr);

/* The ops table (hw.h): `event` the pulse's deadline; `reset` the border, the mode and the pulse back to the start (the
 * frame keeps what it shows); `save` and `load` the border, the mode, the raster position drawn to, the pulse and the
 * frame (packed); `load` arms the deadline again. */

#endif /* ZR_HW_COBRAVIDEO_H */

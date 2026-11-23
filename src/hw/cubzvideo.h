/* ZetRo - the CUB Z's video (this project's CUB Z core: MiSTerRO/docs/cubz/hardware.md, the manual's pages 23-25 and the main
 * board's sheets 8-14; core-cubz/rtl/cubz_video.sv and cubz_core.sv): a 512 x 256 monochrome bitmap from a 16 KiB video RAM
 * of its own, read column by column.
 *
 * Raster: one dot per tick of the 10 MHz master clock (the dot clock), 640 dots a line (64 us), 313 lines a frame (20.03
 * ms). The picture is dots 0-511 of lines 0-255; the rest of each line and lines 256-312 are blanking (horizontal sync dots
 * 544-591 and vertical sync lines 276-280 as the core gives them out; the original PROM's horizontal pulse is 15.2 us long).
 * The raster is a function of guest time alone: dot 0 of line 0 at guest time 0.
 *
 * The picture: dot x of line y is bit 7 - (x & 7) of the video RAM byte {column x >> 3 (6 bits), line (y + scroll) & 255 (8
 * bits)}: the video RAM is column-major, 256 bytes a column of 8 dots, and `scroll` (port F2) moves the first line shown.
 * Nothing is shown while the display is not enabled (PIO-B0). The CPU's accesses to the video RAM are held by WAIT in the
 * second half of every eight-dot window (zr_cubzvideo_wait), when the serializer has the RAM.
 *
 * The frame shows 544 x 288 dots at 4:3: the picture with 16 dots of black border left and right (the left border from the
 * end of the line before) and 16 lines above and below (the ones above from the end of the frame before). Colour 0 is black,
 * 1 the lit dot: white or green (the monitor, an option).
 *
 * Drawing is incremental and in step with the guest: zr_cubzvideo_catch_up(t) draws every dot that starts before guest time t
 * reading the video RAM as it is then, so the board calls it before the video RAM, the scroll register or the enable change,
 * and once more at the end of each run.
 *
 * Portable C11. ZetRo's own code, written from cubz_video.sv and hardware.md. */
#ifndef ZR_HW_CUBZVIDEO_H
#define ZR_HW_CUBZVIDEO_H

#include "hw.h"

struct zr_frame;

#define ZR_CUBZVIDEO_DOTS 640u              /* dots in a raster line */
#define ZR_CUBZVIDEO_LINES 313u
#define ZR_CUBZVIDEO_WIDTH 544u             /* the frame as shown */
#define ZR_CUBZVIDEO_HEIGHT 288u
#define ZR_CUBZVIDEO_VRAM 0x4000u

enum { ZR_CUBZVIDEO_WHITE, ZR_CUBZVIDEO_GREEN };

typedef struct zr_cubzvideo {
    zr_dev dev;
    const uint8_t *vram;                    /* the ZR_CUBZVIDEO_VRAM bytes the serializer reads (the board's) */
    uint8_t scroll, enable, changed;
    uint64_t drawn;                         /* raster dots drawn since guest time 0 */
    uint64_t serial;
    uint32_t palette[2];
    uint8_t pixels[ZR_CUBZVIDEO_WIDTH * ZR_CUBZVIDEO_HEIGHT];
} zr_cubzvideo;

/* Sets the video up on `host` and resets it; it reads `vram` whenever it draws; `monitor` ZR_CUBZVIDEO_*. */
void zr_cubzvideo_init(zr_cubzvideo *v, const zr_hw_host *host, const uint8_t *vram, unsigned monitor);
/* The scroll register and the display enable: call catch_up first. */
void zr_cubzvideo_set_scroll(zr_cubzvideo *v, uint8_t scroll);
void zr_cubzvideo_set_enable(zr_cubzvideo *v, int enable);
void zr_cubzvideo_catch_up(zr_cubzvideo *v, uint64_t t);
/* The frame as drawn: 544 x 288 indexed dots, 2 colours, aspect 4:3. Always 1. */
int zr_cubzvideo_frame(const zr_cubzvideo *v, struct zr_frame *frame);
static inline uint64_t zr_cubzvideo_frame_ticks(const zr_cubzvideo *v) { (void)v; return (uint64_t)ZR_CUBZVIDEO_DOTS * ZR_CUBZVIDEO_LINES; }
/* The WAIT a CPU cycle on the video RAM sees when its WAIT sample falls at guest time t: 1 while the dot counter is in the
 * second half of its eight-dot window (dot bit 2 set), the serializer's half. */
static inline int zr_cubzvideo_wait(uint64_t t) { return (int)((t % ZR_CUBZVIDEO_DOTS) >> 2 & 1u); }
/* The video RAM offset of the byte shown at picture dot x (0-511) of screen line y (0-255) with the scroll offset. */
static inline uint16_t zr_cubzvideo_address(unsigned x, unsigned y, uint8_t scroll) {
    return (uint16_t)(((x >> 3) & 63u) << 8 | ((y + scroll) & 255u));
}

/* The ops table (hw.h): `reset` the scroll and the enable back to 0 (the frame keeps what it shows); `save` and `load` the
 * scroll, the enable, the dot drawn to and the frame (packed). The chip has no deadline. */

#endif /* ZR_HW_CUBZVIDEO_H */

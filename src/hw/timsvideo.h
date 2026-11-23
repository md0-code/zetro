/* ZetRo - the TIM-S Plus's video (the "automat video" of the TIM-S Plus manual, vol. III 8.1 and 10.5, and this
 * project's TIM-S core: shared/romanian/rtl/rp_tims_video.sv, core-tims/rtl/machine_core.sv): a serializer that reads
 * 64 KiB of video RAM of its own, eight 8 KiB pages of the Spectrum's layout, at a 14 MHz dot clock.
 *
 * Raster: 896 dots a line, 312 lines a frame (279552 dots, 50.08 Hz). The picture starts at the raster's origin (dot 0,
 * line 0) and is 192 lines high and 512 dots wide, or 640 in the wide window; the rest is border, except the blanking:
 * dots 672-863 of every line and lines 240-263, so a frame shows 704 x 288 dots, each scan line beginning 32 dots before
 * its raster line (its left border is the end of the line before; the blanked dots there are black). The window is
 * the core's: 32 dots of border on the left, 160 on the right of a 512-dot picture and 32 of a 640-dot one, 48 lines
 * above and below. One dot is `ticks_per_dot` ticks of the master clock.
 *
 * The picture, as rp_tims_video.sv draws it (the board sets the inputs: the manual's PPI port A and B8, the scroll
 * register, the border latch, the FLASH source):
 *  - the 8 KiB page read: `page` (0-7: the manual's PA4, PA3 and port 7FFD's bit 3);
 *  - the resolution: single (each pixel two dots: 256 or 320 pixels) or double (one dot: 512 or 640 pixels); in the
 *    double resolution the bytes of a line alternate between the page and the page with bit 0 inverted;
 *  - the window: narrow (32 cells of 8 pixels, or 64 in the double resolution) or wide (40, or 80); the cells 32-39 of
 *    a line are read from 1800h-1FFFh of the page (line bits 2-0, 5-3 and 7-6, then the cell's low three bits), and
 *    their attributes are those of the cells 0-7;
 *  - the hardware scroll: the picture's line y shows the memory line (y + scroll) taken modulo 192 once (the register is
 *    eight bits, the sum nine; the core's arithmetic, exactly);
 *  - the attributes: the Spectrum's (ink, paper, BRIGHT, FLASH), latched with each cell; with the inhibit set the
 *    latch keeps the attribute it last took, for every cell;
 *  - FLASH: bit 4 of a count of the frames (it counts at the start of line 248), or a level the board gives (the 8253's
 *    counter 1 toggles it, once a program has programmed that counter: zr_timsvideo_set_flash_source);
 *  - the colours: index 0-7 the colour bits (0 blue, 1 red, 2 green), 8-15 their BRIGHT versions, levels 0xCD and 0xFF
 *    (the Spectrum-class machines' nominal ones); the border never bright, the blanking black.
 * The serializer runs as the core's does, dot by dot: the bitmap byte of the next cell is fetched in the middle of a
 * cell and its attribute at the cell's end, from where the raster will be then (the next cell, the next line at the
 * end of one); the shift register shifts every dot (double) or every second dot (single). A fetch reads the video RAM
 * as it is when the dot is drawn.
 *
 * Drawing is incremental and in step with the guest: zr_timsvideo_catch_up(t) draws every dot that starts before guest
 * time t, so the board calls it before the video RAM or an input changes, and at the end of each run. The raster is a
 * function of guest time alone: the counter is the last dot of the frame at guest time 0 (the core resets its counters
 * so) and nothing resets it afterwards.
 *
 * The frame interrupt is the output pin ZR_TIMSVIDEO_INT of host->line: asserted once line 248's first dot has been
 * processed and released 128 dots (32 T-states of 3.5 MHz) later, every frame (the core's INT_n = !(y == 248 && x <
 * 128)), from the chip's single deadline.
 *
 * Portable C11. ZetRo's own code. */
#ifndef ZR_HW_TIMSVIDEO_H
#define ZR_HW_TIMSVIDEO_H

#include "hw.h"

struct zr_frame;

/* The output pin of host->line. */
enum { ZR_TIMSVIDEO_INT };

#define ZR_TIMSVIDEO_DOTS 896               /* dots in a raster line */
#define ZR_TIMSVIDEO_LINES 312              /* lines in a frame */
#define ZR_TIMSVIDEO_WIDTH 704              /* the frame as shown */
#define ZR_TIMSVIDEO_HEIGHT 288
#define ZR_TIMSVIDEO_VRAM 0x10000u          /* bytes of video RAM */
#define ZR_TIMSVIDEO_PAGE 0x2000u           /* bytes of a page */
#define ZR_TIMSVIDEO_INT_DOTS 128           /* how long the frame interrupt lasts */
#define ZR_TIMSVIDEO_SYNC_LINE 248          /* the vertical sync: lines 248-251 */

typedef struct zr_timsvideo {
    zr_dev dev;
    const uint8_t *vram;                    /* the ZR_TIMSVIDEO_VRAM bytes the serializer reads (the board's) */
    uint32_t ticks_per_dot;
    uint32_t total;                         /* dots in a frame */
    uint32_t start_dot;                     /* the raster counter at guest time 0: the last dot of the frame */
    uint32_t window_origin;                 /* the raster counter of the frame's first dot */
    /* the inputs (saved) */
    uint8_t border, wide, hires, inhibit, page, scroll;
    uint8_t flash_timer, flash_level;       /* FLASH from the board's level instead of the frame count */
    /* the serializer's registers (saved) */
    uint8_t bitmap, shift, attribute;
    uint8_t changed;
    uint64_t drawn;                         /* raster dots drawn since guest time 0 */
    uint64_t int_at;                        /* guest time of the interrupt's next assertion, or of its release when asserted */
    uint8_t int_on;
    uint64_t serial;
    uint32_t palette[16];
    uint8_t pixels[ZR_TIMSVIDEO_WIDTH * ZR_TIMSVIDEO_HEIGHT];
} zr_timsvideo;

/* Sets the video on `host` (its schedule is armed here) and resets it: the chip reads `vram` (64 KiB, the board's)
 * whenever dots are drawn; `ticks_per_dot` ticks of the master clock make a dot. */
void zr_timsvideo_init(zr_timsvideo *v, const zr_hw_host *host, uint32_t ticks_per_dot, const uint8_t *vram);
/* The inputs; call catch_up first. The border colour (0-7); the window and resolution (wide: 40 cells, hires: double)
 * and the attribute inhibit; the page (0-7) and the scroll register; FLASH from the board (timer 1) or from the frame
 * count, and the board's level. */
void zr_timsvideo_set_border(zr_timsvideo *v, unsigned colour);
void zr_timsvideo_set_mode(zr_timsvideo *v, int wide, int hires, int inhibit);
void zr_timsvideo_set_page(zr_timsvideo *v, unsigned page);
void zr_timsvideo_set_scroll(zr_timsvideo *v, unsigned scroll);
void zr_timsvideo_set_flash_source(zr_timsvideo *v, int timer);
void zr_timsvideo_set_flash(zr_timsvideo *v, int level);
/* Draws the raster up to guest time t. */
void zr_timsvideo_catch_up(zr_timsvideo *v, uint64_t t);
/* The frame as drawn: 704 x 288 indexed dots, the 16-colour palette, aspect 4:3, a serial that changes when pixels do.
 * Always 1. */
int zr_timsvideo_frame(const zr_timsvideo *v, struct zr_frame *frame);
/* The frame's length in guest-time ticks. */
static inline uint64_t zr_timsvideo_frame_ticks(const zr_timsvideo *v) { return (uint64_t)v->total * v->ticks_per_dot; }
/* The raster position at guest time t: the line (0-311) and the dot (0-895) in the core's counters. */
void zr_timsvideo_position(const zr_timsvideo *v, uint64_t t, unsigned *line, unsigned *dot);
/* The offset in the video RAM of the byte the serializer reads for cell `cell` (0-79 in the double resolution's wide
 * window; each cell is 8 pixels) of the picture's line y (0-191) with the inputs as they are now: the scroll, the page,
 * the resolution and the window, as the core computes it. The text read-back uses it. */
uint32_t zr_timsvideo_cell_address(const zr_timsvideo *v, unsigned y, unsigned cell);

/* The ops table of the dev (hw.h): `event` is the interrupt's deadline; `reset` releases the interrupt and sets it for
 * the next frame (the inputs, the serializer and the FLASH source are the board's to reset); `read` and `write` answer
 * nothing; `save` and `load` carry the inputs, the registers, the raster position drawn to, the interrupt and the
 * frame (packed), and `load` arms the deadline again. */

#endif /* ZR_HW_TIMSVIDEO_H */

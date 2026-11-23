/* ZetRo - the HC-88's video (drawing 869.300.350, as this project's HC core documents it: MiSTerRO/docs/hc/hc88.md and
 * core-hc/rtl/hc88_video.sv): a serializer that reads a 16 KiB video RAM of its own, two Spectrum-format screens
 * of 8 KiB side by side (bitmap at 0000-17FF, attributes at 1800-1AFF of each), at the 14 MHz dot clock.
 *
 * Raster: 896 dots a line, 312 lines a frame (a frame is 279552 dots, 50.08 Hz). The picture is 512 dots wide and
 * 192 lines high at the raster's origin (dot 0, line 0); the rest of the raster is border, except the blanking and
 * the sync that are not drawn: dots 608-799 of every line and lines 240-263 (the beam's retrace), so a frame shows
 * 704 x 288 dots: the picture, a border of 96 dots left and right and 48 lines above and below, as the monitor
 * scans them (a scan line is the 704 dots that begin 96 before the raster line's own, so its left border comes from
 * the end of the line before it). One dot is `ticks_per_dot` ticks of the master clock (1 when the master clock is
 * the dot clock). The frame is always 704 x 288 dots, in both modes, aspect 4:3.
 *
 * Two modes, selected by the command latch's MOD bit (set_hires). 256 pixels (MOD clear): each pixel is two dots,
 * the 32 cells of a line are read from the first screen (bank 0), the bitmap byte of cell c at the Spectrum's
 * address for the line, bit 7 first, its attribute from the first screen's attribute area. 512 pixels (MOD set):
 * one pixel is one dot and the two screens are interleaved by byte: the 16 dots of a cell are the cell's byte of
 * bank 0 (8 dots) and then of bank 1 (VRAM address + 2000h, 8 dots), each with its own attribute byte, so the
 * two banks' bitmaps and attributes are independent (the 80-column terminal draws 6-dot characters over both).
 * An attribute is the Spectrum's: ink in bits 0-2, paper in 3-5, BRIGHT in 6 (the HC-88 connects it: BRIGHT black
 * is black) and FLASH in 7, which swaps ink and paper for 16 frames in 32 (the counter is the frame number's bit
 * 4). The border is the colour of port FE's low three bits, never bright. Colour index 0-7 as the colour bits
 * (bit 0 blue, 1 red, 2 green), 8-15 their BRIGHT versions; the two levels (0xCD and 0xFF) are the Spectrum-class
 * machines' nominal ones (the HC-88's analogue levels are not measured).
 *
 * Drawing is incremental and in step with the guest, as ula.h's: zr_hc88video_catch_up(t) draws every dot that
 * starts before guest time t reading the video RAM as it is then, so the board calls it first (at the cycle's time)
 * before the video RAM, the border or the mode changes, and once more at the end of each run. A change takes effect
 * from the first dot that starts at or after its time. The raster is a function of guest time alone (the counter
 * is the last dot of a frame at guest time 0, as the core resets its counters): nothing resets it afterwards.
 *
 * The frame interrupt is the output pin ZR_HC88VIDEO_INT of host->line: asserted when line 248's first dot has
 * been processed (dot 1: the core's registered x counter reads 0 from there) and released 128 dots (32 CPU
 * T-states) later, in either mode, every frame, from the chip's single deadline.
 *
 * Portable C11. ZetRo's own code, written from hc88.md and the core's hc88_video.sv and hc88_video_tb.sv. */
#ifndef ZR_HW_HC88VIDEO_H
#define ZR_HW_HC88VIDEO_H

#include "hw.h"

struct zr_frame;

/* The output pin of host->line. */
enum { ZR_HC88VIDEO_INT };

#define ZR_HC88VIDEO_DOTS 896               /* dots in a raster line */
#define ZR_HC88VIDEO_LINES 312              /* lines in a frame */
#define ZR_HC88VIDEO_WIDTH 704              /* the frame as shown */
#define ZR_HC88VIDEO_HEIGHT 288
#define ZR_HC88VIDEO_VRAM 0x4000u           /* bytes of video RAM */
#define ZR_HC88VIDEO_INT_DOTS 128           /* how long the frame interrupt lasts */

typedef struct zr_hc88video {
    zr_dev dev;
    const uint8_t *vram;                    /* the ZR_HC88VIDEO_VRAM bytes the serializer reads (the board's memory) */
    uint32_t ticks_per_dot;                 /* 1 */
    uint32_t total;                         /* dots in a frame */
    uint32_t start_dot;                     /* the raster counter at guest time 0: the last dot of the frame */
    uint32_t window_origin;                 /* the raster counter of the frame's first dot */
    uint8_t border, hires, changed;
    uint64_t drawn;                         /* raster dots drawn since guest time 0 */
    uint64_t int_at;                        /* guest time of the interrupt's next assertion, or of its release when asserted */
    uint8_t int_on;
    uint64_t serial;
    uint32_t palette[16];
    uint8_t pixels[ZR_HC88VIDEO_WIDTH * ZR_HC88VIDEO_HEIGHT];
} zr_hc88video;

/* Sets the video on `host` (its schedule is armed here) and resets it: the chip reads `vram` (16 KiB, the board's)
 * whenever dots are drawn; `ticks_per_dot` ticks of the master clock make a dot. */
void zr_hc88video_init(zr_hc88video *v, const zr_hw_host *host, uint32_t ticks_per_dot, const uint8_t *vram);
/* The border colour (the low 3 bits of port FE) and the serializer width (MOD: 1 for 512 pixels): call catch_up first. */
void zr_hc88video_set_border(zr_hc88video *v, unsigned colour);
void zr_hc88video_set_hires(zr_hc88video *v, int hires);
/* Draws the raster up to guest time t. */
void zr_hc88video_catch_up(zr_hc88video *v, uint64_t t);
/* The frame as drawn (not drawing anything itself): 704 x 288 indexed dots, the 16-colour palette, aspect 4:3, a
 * serial that changes when pixels do. Always 1. */
int zr_hc88video_frame(const zr_hc88video *v, struct zr_frame *frame);
/* The frame's length in guest-time ticks. */
static inline uint64_t zr_hc88video_frame_ticks(const zr_hc88video *v) { return (uint64_t)v->total * v->ticks_per_dot; }
/* The raster position at guest time t: the line (0-311) and the dot (0-895) in the core's own counters, where the
 * picture starts at (0, 0), and the number of the frame (0 at guest time 0, one more at each wrap of the counter). */
void zr_hc88video_position(const zr_hc88video *v, uint64_t t, unsigned *line, unsigned *dot);
uint64_t zr_hc88video_frame_number(const zr_hc88video *v, uint64_t t);

/* The ops table of the dev (hw.h): `event` is the interrupt's deadline; `reset` brings the border, the mode and the
 * interrupt back to the start (the frame keeps what it shows); `read` and `write` answer nothing (the board decodes
 * the ports); `save` and `load` carry the border, the mode, the raster position drawn to, the interrupt and the
 * frame (packed), and `load` arms the deadline again (the board has restored its time by then). */

#endif /* ZR_HW_HC88VIDEO_H */

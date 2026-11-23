/* ZetRo - the monochrome bitmap video of the aMIC and the PRAE-M: 256 x 256 dots in an 8 KiB memory of their own, 32 bytes a line, the
 * most significant bit on the left (aMIC manual I, 3.6 "Interfata cu televizorul"; MiSTerRO/src/shared/romanian/rtl/rp_mono_video.sv, which
 * both machines use, and rp_native_machine.sv for what each does with it).
 *
 * Raster: 640 dots a line at the 10 MHz master clock (a 64 microsecond line), 312 lines a frame; two dots make one logical pixel, so a
 * line is 320 pixels and a frame 99840 (50.08 Hz). The picture is the first 256 pixels of the first 256 lines of the raster, the rest is
 * blanking (the beam is off and black whatever the inversion): horizontal blanking pixels 256-319 (12.8 microseconds of the line),
 * vertical blanking lines 256-311.
 *
 * A pixel is lit (colour index 1) when its memory bit is 0 and the inversion is off, or its bit is 1 and the inversion is on; the
 * blanking is dark (index 0). The inversion is the aMIC's port C bit 5 ("video invers pentru intregul ecran"); the PRAE has none. The
 * lit colour is the monitor's phosphor (white, or green): the palette's second entry, chosen by the board.
 *
 * The frame as shown is 320 x 288 pixels at 4:3: the picture with a border of 32 pixels left and right and 16 lines above and below,
 * which is all black blanking (the monitor's overscan hides some of it on a television; the border is ZetRo's choice, not a
 * measurement). A scan line of the frame is the 320 pixels that begin 32 before the raster line's own (so the left border comes from
 * the end of the line before it); the first scan line is that of raster line 296, 16 lines before the picture.
 *
 * Drawing is incremental and in step with the guest, as hc88video.h's: zr_monovideo_catch_up(t) draws every pixel that starts before
 * guest time t reading the memory as it is then, so the board calls it first (at the cycle's time) before the video memory or the
 * inversion changes, and once more at the end of each run. A change takes effect from the first pixel that starts at or after its time.
 * The raster is a function of guest time alone (the counter is the last pixel of a frame at guest time 0): nothing resets it.
 *
 * The PRAE's frame interrupt (enabled at init): the output pin ZR_MONOVIDEO_INT of host->line, asserted when the raster counter reads
 * pixel 0 of line 256 and released 16 pixels (32 ticks: 8 CPU T-states) later, every frame: rp_native_machine.sv's
 * `int_n=!(PRAE&&video_y==256&&video_x<16)`.
 *
 * Portable C11. ZetRo's own code, written from the aMIC manual, rp_mono_video.sv and rp_native_machine.sv. */
#ifndef ZR_HW_MONOVIDEO_H
#define ZR_HW_MONOVIDEO_H

#include "hw.h"

struct zr_frame;

/* The output pin of host->line. */
enum { ZR_MONOVIDEO_INT };

#define ZR_MONOVIDEO_PIXELS 320             /* pixels in a raster line (640 dots) */
#define ZR_MONOVIDEO_LINES 312              /* lines in a frame */
#define ZR_MONOVIDEO_WIDTH 320              /* the frame as shown */
#define ZR_MONOVIDEO_HEIGHT 288
#define ZR_MONOVIDEO_PICTURE 256            /* the picture is 256 x 256 */
#define ZR_MONOVIDEO_VRAM 0x2000u           /* bytes of bitmap */
#define ZR_MONOVIDEO_INT_PIXELS 16          /* how long the frame interrupt lasts */

typedef struct zr_monovideo {
    zr_dev dev;
    const uint8_t *vram;                    /* the ZR_MONOVIDEO_VRAM bytes the serializer reads (the board's memory) */
    uint32_t ticks_per_pixel;               /* 2 */
    uint32_t total;                         /* pixels in a frame */
    uint32_t start_pixel;                   /* the raster counter at guest time 0: the last pixel of the frame */
    uint32_t window_origin;                 /* the raster counter of the frame's first pixel */
    uint8_t invert, interrupt, changed;
    uint64_t drawn;                         /* raster pixels drawn since guest time 0 */
    uint64_t int_at;                        /* guest time of the interrupt's next assertion, or of its release when asserted */
    uint8_t int_on;
    uint64_t serial;
    uint32_t palette[2];
    uint8_t pixels[ZR_MONOVIDEO_WIDTH * ZR_MONOVIDEO_HEIGHT];
} zr_monovideo;

/* Sets the video on `host` (its schedule is armed here when `interrupt`) and resets it: the chip reads `vram` (8 KiB, the board's)
 * whenever pixels are drawn; `ticks_per_pixel` ticks of the master clock make a pixel; `interrupt` 1: the frame interrupt. The lit
 * colour starts white. */
void zr_monovideo_init(zr_monovideo *v, const zr_hw_host *host, uint32_t ticks_per_pixel, const uint8_t *vram, int interrupt);
/* The inversion (port C bit 5 on the aMIC): call catch_up first. */
void zr_monovideo_set_invert(zr_monovideo *v, int on);
/* The colour of a lit pixel, 0x00RRGGBB (the monitor's phosphor); the picture is redrawn at once. */
void zr_monovideo_set_colour(zr_monovideo *v, uint32_t rgb);
/* Draws the raster up to guest time t. */
void zr_monovideo_catch_up(zr_monovideo *v, uint64_t t);
/* The frame as drawn (not drawing anything itself): 320 x 288 indexed pixels, the 2-colour palette, aspect 4:3, a serial that changes
 * when pixels do. Always 1. */
int zr_monovideo_frame(const zr_monovideo *v, struct zr_frame *frame);
/* The frame's length in guest-time ticks. */
static inline uint64_t zr_monovideo_frame_ticks(const zr_monovideo *v) { return (uint64_t)v->total * v->ticks_per_pixel; }
/* The raster position at guest time t: the line (0-311) and the pixel (0-319), where the picture starts at (0, 0), and the number
 * of the frame (0 at guest time 0, one more at each wrap of the counter). */
void zr_monovideo_position(const zr_monovideo *v, uint64_t t, unsigned *line, unsigned *pixel);
uint64_t zr_monovideo_frame_number(const zr_monovideo *v, uint64_t t);

/* The ops table of the dev (hw.h): `event` is the interrupt's deadline; `reset` brings the inversion and the interrupt back to the
 * start (the frame keeps what it shows); `read` and `write` answer nothing (the board decodes the ports); `save` and `load` carry
 * the inversion, the raster position drawn to, the interrupt, the colour and the frame (packed), and `load` arms the deadline again
 * (the board has restored its time by then). */

#endif /* ZR_HW_MONOVIDEO_H */

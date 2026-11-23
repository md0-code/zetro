/* ZetRo - the Spectrum-compatible video and keyboard, as the clones of the
 * Romanian machines build them from TTL (HC-85 sheets 4-8, JET): the raster
 * counters, the serial display pipeline with attributes, BRIGHT (where it is
 * connected) and FLASH, the border, the frame interrupt and the 8 x 5 key
 * matrix. The clones have no memory contention and no floating bus; the
 * optional Sinclair timing below adds them (the board applies the delays).
 *
 * Raster: one pixel is `ticks_per_pixel` ticks of the master clock; a line
 * is `pixels_per_line` pixels, a frame `lines_per_frame` lines. The raster
 * counter is line * pixels_per_line + pixel; it is `start_pixel` at guest
 * time 0 and runs on from there, so the raster is a function of guest time
 * alone (a machine RESET does not move it: nothing resets the counters on a
 * real board either). The 256 x 192 picture's first pixel is at raster
 * (picture_x, picture_y); the picture's own coordinates (px, py) count
 * from it, line after line without a break, so what is fetched before
 * pixel 0 of a line (see below) is the end of the line before.
 *
 * The visible frame is 352 x 288 pixels: the picture at (48, 48) with a
 * border of 48 pixels left, right and above and 48 below, as one monitor
 * scan line after another: a scan line is the 352 raster pixels that begin
 * 48 before the picture's line, so its left border comes from the end of
 * the raster line before it. The rest of the raster is blanking and sync
 * and is not drawn. The colours are the Spectrum's: index 0-7 as the colour
 * bits (bit 0 blue, 1 red, 2 green), 8-15 their BRIGHT versions; `bright`
 * says whether attribute bit 6 reaches the colour circuit (the HC-85 does
 * not connect it), and the two levels are the voltages of a lit colour
 * component, normal and bright (BRIGHT black is black).
 *
 * Drawing is incremental and in step with the guest: zr_ula_catch_up(t)
 * draws every raster pixel that starts before guest time t, reading video
 * memory as the circuit does, so that a change of the border or a write to
 * video memory lands at the right pixel when the board calls catch_up(t)
 * first (before the change, at the guest time of the cycle) and once more
 * at the end of each frame. A change takes effect from the first pixel that
 * starts at or after its time. Video memory is read through `vram` with an
 * offset 0..1AFFh: the bitmap's 6144 bytes in the Spectrum's interleaved
 * order at 0000-17FF, the attributes at 1800-1AFF, so a board with paged or
 * relocated screens supplies its own. The display pipeline reads it the way
 * the circuit does: the bitmap byte of a character cell 5 pixels and its
 * attribute 1 pixel before the cell's first pixel.
 *
 * The frame interrupt is the output pin ZR_ULA_INT of host->line: asserted
 * at raster (int_x, int_y) and released `int_tstates` CPU T-states later
 * (a T-state is `ticks_per_tstate` ticks), every frame, from the chip's
 * single deadline (the board calls ops->event when it comes).
 *
 * Keyboard: zr_ula_key sets a key of the matrix (row 0-7, column 0-4);
 * zr_ula_keys is what port FE reads in bits 0-4 for a high address byte.
 * Decoding the port, EAR, the speaker and MIC are the board's.
 *
 * Sinclair timing (not original, `sinclair` in the config; 0 is the clone's
 * timing above): the frame, the picture's place, the interrupt, memory
 * contention and the floating bus of Sinclair's own ULA, 48K or 128K, as the
 * published documentation gives them.
 * Times are T-states of the frame, 0 the first T-state the interrupt is low.
 * The 48K: 312 lines of 224 T-states (69888), the interrupt 32 T-states
 * long; from `contention_start` (14335) on each of the 192 picture lines
 * the CPU is held 6,5,4,3,2,1,0,0 T-states for an access in contended
 * memory, for 128 T-states; the ULA fetches four bytes of the picture in
 * the first four T-states of each eight from contention_start + 3 (14338):
 * bitmap, attribute, bitmap and attribute of the next cell; nothing in the
 * other four. The 128K: 311 lines of 228 (70908), contention from 14361, the
 * interrupt 36 T-states. zr_ula_sinclair_config sets these. The renderer
 * then draws each cell from the bytes fetched at those T-states (a cell is
 * shown from 4 T-states after its pair's fetch began, the second of the
 * pair 4 T-states later) and the border in 8-pixel units aligned with the
 * cells (a border change shows from the first unit that starts at or after
 * it); contention[] holds the delay for each T-state of the frame;
 * zr_ula_floating gives the byte on the ULA's data bus at a T-state.
 *
 * Portable C11. ZetRo's own code, written from the HC-85 manual's display
 * and keyboard sections and the JET core's documented values (this
 * project's rp_spectrum_video.sv and rp_spectrum_keyboard.sv); the Sinclair
 * timing from the comp.sys.sinclair FAQ's 48K and 128K references and the
 * Sinclair Wiki's "Contended memory" and "Floating bus" pages. */
#ifndef ZR_HW_ULA_H
#define ZR_HW_ULA_H

#include "hw.h"

struct zr_frame;

/* The output pin of host->line. */
enum { ZR_ULA_INT };

#define ZR_ULA_WIDTH 352
#define ZR_ULA_HEIGHT 288
#define ZR_ULA_PICTURE_X 48                 /* where the picture is in the frame */
#define ZR_ULA_PICTURE_Y 48

typedef struct zr_ula_config {
    uint32_t ticks_per_pixel;               /* 2 */
    uint32_t ticks_per_tstate;              /* 4 */
    uint32_t pixels_per_line;               /* 448 */
    uint32_t lines_per_frame;               /* 312 */
    uint32_t start_pixel;                   /* the raster counter at guest time 0: the last pixel of the frame, as the core resets */
    uint32_t picture_x, picture_y;          /* the raster position of the picture's first pixel: 0, 0 */
    uint32_t int_x, int_y;                  /* where the frame interrupt starts: pixel 0 of line 248 */
    uint32_t int_tstates;                   /* how long it lasts: 32 (64 pixels) */
    uint32_t flash_frames;                  /* the FLASH half-period in frames: 16 (0: no FLASH) */
    uint8_t bright;                         /* attribute bit 6 is connected: 1 */
    uint8_t level_normal, level_bright;     /* the lit component's level: 0xCD, 0xFF */
    uint8_t sinclair;                       /* ZR_ULA_CLONE (the values above as they are), ZR_ULA_SINCLAIR_48K or _128K */
    uint32_t contention_start;              /* Sinclair timing: the first contended T-state of the frame */
} zr_ula_config;

/* The timings a config may have. */
enum { ZR_ULA_CLONE, ZR_ULA_SINCLAIR_48K, ZR_ULA_SINCLAIR_128K };
#define ZR_ULA_SINCLAIR_TSTATES 70908u      /* the longest Sinclair frame, the 128K's */

/* The values the JET core documents (and its HC-85 sibling's raster). */
void zr_ula_default_config(zr_ula_config *config);
/* The raster, the interrupt and the contention of Sinclair's ULA (`which`
 * ZR_ULA_SINCLAIR_48K or _128K) over a config, keeping its clock, colours
 * and FLASH; ZR_ULA_CLONE leaves it as it is. The picture's first pixel is
 * contention_start + 7 T-states after the interrupt; the counter is at the
 * end of the visible frame at guest time 0. */
void zr_ula_sinclair_config(zr_ula_config *config, unsigned which);

typedef struct zr_ula {
    zr_dev dev;
    zr_ula_config cfg;
    uint8_t (*vram)(void *ctx, uint16_t offset);
    void *vram_ctx;
    uint32_t total;                         /* pixels in a frame */
    uint32_t picture_origin, window_origin; /* the raster counter of the picture's first pixel and of the frame's */
    uint8_t border, bitmap, shift, attr;    /* the border colour (0-7), the pipeline's latches */
    uint8_t keys[8];                        /* the matrix: bit n of row r set while the key at column n is down */
    uint8_t changed;
    uint64_t drawn;                         /* raster pixels drawn since guest time 0 */
    uint64_t int_at;                        /* guest time of the interrupt's next assertion, or of its release when asserted */
    uint8_t int_on;
    uint64_t serial;
    uint32_t palette[16];
    uint8_t pixels[ZR_ULA_WIDTH * ZR_ULA_HEIGHT];
    /* Sinclair timing */
    uint32_t tstates;                       /* T-states in a frame */
    uint32_t tstates_per_line;
    uint32_t int_counter;                   /* the raster counter of the interrupt's start */
    uint8_t fetched[4];                     /* the latches of a pair of cells: bitmap, attribute, bitmap, attribute */
    uint8_t border_shown;                   /* the border as the current 8-pixel unit shows it */
    uint8_t contention[ZR_ULA_SINCLAIR_TSTATES];    /* the CPU's delay at each T-state of the frame */
} zr_ula;

/* Sets the video on `host` (its schedule is armed here) and resets it.
 * `config` NULL: the default. `vram` is read whenever pixels are drawn. */
void zr_ula_init(zr_ula *u, const zr_hw_host *host, const zr_ula_config *config, uint8_t (*vram)(void *ctx, uint16_t offset), void *vram_ctx);
/* The border colour (the low 3 bits of port FE): call catch_up first. */
void zr_ula_set_border(zr_ula *u, unsigned colour);
/* Draws the raster up to guest time t. */
void zr_ula_catch_up(zr_ula *u, uint64_t t);
/* The frame as it is drawn (not drawing anything itself): indexed pixels,
 * the 16-colour palette, aspect 4:3, a serial that changes when pixels do.
 * Always 1. */
int zr_ula_frame(const zr_ula *u, struct zr_frame *frame);
/* The frame's length in guest-time ticks. */
static inline uint64_t zr_ula_frame_ticks(const zr_ula *u) { return (uint64_t)u->total * u->cfg.ticks_per_pixel; }

/* A key of the matrix. */
void zr_ula_key(zr_ula *u, unsigned row, unsigned column, int down);
/* Bits 0-4 of what port FE reads: the column bits, 0 for a pressed key, of
 * every row whose address line is low in `high` (bit r is address line
 * A8 + r); bits 5-7 are 0. */
uint8_t zr_ula_keys(const zr_ula *u, uint8_t high);

/* The raster position at guest time t: the line and the pixel within it
 * (in the frame's own counter, where the picture starts at picture_y,
 * picture_x), and the number of the frame (0 at guest time 0, then one
 * more at every wrap of the counter). For the contention and floating-bus
 * logic of the board. */
void zr_ula_position(const zr_ula *u, uint64_t t, unsigned *line, unsigned *pixel);
uint64_t zr_ula_frame_number(const zr_ula *u, uint64_t t);

/* Sinclair timing. The T-state of the frame at guest time t (0 the
 * interrupt's first T-state; t on a T-state's boundary). */
uint32_t zr_ula_tstate(const zr_ula *u, uint64_t t);
/* The CPU's delay for an access to contended memory starting at T-state
 * `tstate` of the frame (any value: taken modulo the frame). */
static inline unsigned zr_ula_delay(const zr_ula *u, uint32_t tstate) {
    return u->contention[tstate < u->tstates ? tstate : tstate % u->tstates];
}
/* The byte on the ULA's data bus at guest time t: the bitmap or attribute
 * byte it is fetching then, FFh between fetches and outside the picture
 * (what a read of an unattached port gives). Read through `vram`. */
uint8_t zr_ula_floating(const zr_ula *u, uint64_t t);
/* The 128K's RAM pages (0-7) that the ULA contends: 1, 3, 5 and 7. */
static inline int zr_ula_page_contended(unsigned page) { return (page & 1u) != 0; }

/* The ops table of the dev (hw.h): `event` is the interrupt's deadline;
 * `reset` brings the border, the pipeline and the interrupt back to the
 * start (the frame keeps what it shows, the keys stay); `read` and `write`
 * answer nothing (the board decodes port FE); `save` and `load` carry the
 * border, the pipeline, the keys, the raster position drawn to, the
 * interrupt and the frame (packed), with Sinclair timing also the fetch
 * latches and the border shown (at the chunk's end; a clone-timed chunk is
 * as before), and `load` arms the deadline again with host->schedule (the
 * board has restored its time by then). */

#endif /* ZR_HW_ULA_H */

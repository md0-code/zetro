/* ZetRo - the CUB Z's keyboard and its serial link (this project's CUB Z core: the manual's sections 2.3.3, "Paralel I/O",
 * and 3.4, "Tastatura"; hardware.md; core-cubz/rtl/cubz_keyboard.sv; the keyboard's own EPROM, keyboard.bin, for the codes).
 *
 * The keyboard. Its keys are the positions of its 16 x 16 matrix (0-255), and the modifiers SHIFT (either), CTRL, CAPS LOCK
 * (a latch: each press toggles it) and REPEAT. A key pressed is looked up in the keyboard's code table (its EPROM: one page
 * of codes for no modifier, CAPS LOCK, SHIFT and CTRL, CTRL first, then SHIFT, then CAPS LOCK) and the code goes into a
 * 32-byte buffer (the core's; the real keyboard serialises one code at a time and a host types faster than a hand); a
 * position the table has no code for sends nothing. KBCLK (busy, active low) is low for 100 us after a code is taken
 * in, while it goes into the serialising register; the link answers 1 meanwhile. A key held repeats: after 400 ms at 10
 * codes a second (hardware.md and the core), or at once at 10 a second while REPEAT is held (the manual).
 *
 * The link: one open-collector line, KBDATA, which the processor interrogates (PIO-B7) and reads (PIO-B6). Each
 * interrogation is a short pulse; when it ends the keyboard puts its answer on the line: to the first, the start bit (0
 * when a code is ready, else 1, the idle level), then, after a start bit of 0, the eight bits of the code, least
 * significant first, one an interrogation; then the code leaves the buffer. While the interrogation is high the line reads
 * 0. The firmware (the monitor's routine at EBD8h) makes all nine interrogations.
 *
 * Pins (hw.h). Input (ops->input): ZR_CUBZKBD_IN_INTERROGATE, the level PIO-B7 drives (1 high). Outputs (host->line, state
 * the level on the wire, 1 high): ZR_CUBZKBD_OUT_DATA (PIO-B6) and ZR_CUBZKBD_OUT_CLOCK (-KBCLK, PIO-B5, and the CTC's
 * CLK/TRG3), one call at every change; zr_cubzkbd_data and zr_cubzkbd_clock give the levels.
 *
 * Time: one deadline (the end of KBCLK's busy time, the next repeat). Save and load: everything; load makes no host call
 * except arming the deadline.
 *
 * Portable C11. ZetRo's own code. */
#ifndef ZR_HW_CUBZKBD_H
#define ZR_HW_CUBZKBD_H

#include "hw.h"

enum { ZR_CUBZKBD_IN_INTERROGATE };
enum { ZR_CUBZKBD_OUT_DATA, ZR_CUBZKBD_OUT_CLOCK };

/* The keys that are not matrix positions (zr_cubzkbd_key). */
enum { ZR_CUBZKBD_SHIFT_LEFT = 256, ZR_CUBZKBD_SHIFT_RIGHT, ZR_CUBZKBD_CTRL, ZR_CUBZKBD_CAPS, ZR_CUBZKBD_REPEAT };

#define ZR_CUBZKBD_BUFFER 32u

typedef struct zr_cubzkbd {
    zr_dev dev;
    uint32_t clock_hz;
    uint8_t shift_l, shift_r, ctrl, caps, rep;
    int16_t held;                           /* the matrix position held for repeat, -1 none */
    uint8_t held_code;
    uint64_t repeat_at;                     /* the next repeat, ZR_NEVER none */
    uint8_t fifo[ZR_CUBZKBD_BUFFER];
    uint8_t head, count;
    uint64_t busy_until;                    /* KBCLK low until then */
    uint8_t interrogate;                    /* the input's level */
    uint8_t bitpos;                         /* 0 waiting for a start bit, 1-8 the data bit the next interrogation reads */
    uint8_t shifter, response;
    uint8_t data_reported, clock_reported;
    uint32_t overflow;                      /* codes lost to a full buffer */
} zr_cubzkbd;

/* Sets the keyboard up on `host` with the master clock at clock_hz; the board then calls dev.ops->reset. */
void zr_cubzkbd_init(zr_cubzkbd *k, const zr_hw_host *host, const char *name, uint32_t clock_hz);
/* A key (a matrix position 0-255 or ZR_CUBZKBD_*) pressed (down 1) or released. */
void zr_cubzkbd_key(zr_cubzkbd *k, unsigned key, int down);
/* The code a matrix position sends with the modifiers held now; -1 when it sends none. */
int zr_cubzkbd_code(const zr_cubzkbd *k, unsigned position);
/* The levels of the outputs. */
int zr_cubzkbd_data(const zr_cubzkbd *k);
int zr_cubzkbd_clock(const zr_cubzkbd *k);

/* The ops table (hw.h): `reset` empties the buffer, lets go of every key and the CAPS LOCK latch; `event` the busy time's end
 * and the repeat; `input` the interrogation; `save`, `load`. */

#endif /* ZR_HW_CUBZKBD_H */

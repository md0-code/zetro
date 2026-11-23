/* ZetRo - the General Instrument AY-3-8912 programmable sound generator
 * (the AY-3-8910 with one I/O port): three square-wave tone generators, a
 * noise generator, a mixer and a 16-level logarithmic DAC for each channel,
 * and an envelope generator, all set through 16 registers.
 *
 * Registers (decimal) and the bits they keep, from the data manual:
 *   R0 R1 / R2 R3 / R4 R5   tone period A / B / C, 12 bits (R1, R3, R5 keep 4 bits)
 *   R6                      noise period, 5 bits
 *   R7                      mixer: bits 0-2 tone off A B C, bits 3-5 noise off A B C,
 *                           bit 6 port A is an output, bit 7 port B is an output (the 8912 has no port B)
 *   R8 R9 R10               amplitude A B C: bits 0-3 level, bit 4 take the envelope (5 bits)
 *   R11 R12                 envelope period, 16 bits
 *   R13                     envelope shape: bit 3 CONT, 2 ATT, 1 ALT, 0 HOLD (4 bits); a write restarts it
 *   R14 R15                 port A and port B data; R15 only stores (the 8912 has no port B pins)
 * A register reads back what it keeps; a bit it does not keep reads 0.
 *
 * Clocks (from the data manual): the chip divides its clock by 8 for the
 * three tones, which toggle every TP counts (f = fclk / 16 TP, half a
 * period of 8 TP clocks), and by 16 for the noise, which shifts its 17-bit
 * register every 16 NP clocks (f = fclk / 16 NP), and by 256 for the
 * envelope, whose 16 steps each last 16 EP clocks (f = fclk / 256 EP). A
 * period of 0 acts as 1. The chip's clock rate is given at init, relative
 * to the master clock the board counts guest time in (1.75 MHz from the
 * 14 MHz of the Spectrum-compatible boards: a tick of the chip's /8 divider
 * every 64 master-clock ticks); the ratio need not be an integer.
 *
 * Bus: the board decodes the ports (FFFD and BFFD on the 128K Spectrum) and
 * calls zr_ay_select (latch address), zr_ay_read (read the register latched)
 * and zr_ay_write (write it); the ops table's read and write do the same
 * with offset 0 for the address port (a write latches, a read reads the
 * register) and offset 1 for the data port (a write writes). Like the real
 * chip, which answers only to the address code 0000 in DA7-DA4, an address
 * with other high bits deselects it: data writes are lost and reads give
 * FFh until a valid address is latched.
 *
 * Port A: an output when R7 bit 6 is set, driving the pins with R14, else
 * an input whose pins (pulled up inside, so FFh when nothing drives them)
 * are read through `in`. `out` is called whenever the value the pins carry
 * changes (a write of R14 or of R7, a reset; 0FFh when the port is an
 * input), at the guest time of the access (the board's now()). The CIP-04
 * uses the port for its serial lines.
 *
 * Sound: the output of each channel is its DAC level when its mixer output
 * is high, else 0 (a channel with tone and noise both off is held high, as
 * the data manual notes), mixed into the two channels of the frame as the
 * mode says, and averaged over each output frame's period (not sampled at a
 * point, so the tones and the noise above the audio band fold back far
 * less than point sampling would let them), then high-passed at about 30 Hz
 * (the coupling capacitor of the audio output: a held level decays to
 * silence). Output
 * frames are derived from guest time exactly as the beeper's (beeper.h):
 * frame n covers the master-clock times from ceil(n * clock / rate) to the
 * next frame's; they lag guest time by the same ZR_AY_LATENCY frames, with
 * the same backlog rule, so a board that calls both with the same time and
 * buffer size gets the same frames from each and may add them.
 *
 * Portable C11. ZetRo's own code, written from the General Instrument
 * AY-3-8910/8912 Programmable Sound Generator Data Manual. No emulator
 * source consulted. */
#ifndef ZR_HW_AY8912_H
#define ZR_HW_AY8912_H

#include "hw.h"

#include <stddef.h>

/* How the three channels reach the two output channels. */
enum {
    ZR_AY_MONO,                 /* each channel to both sides */
    ZR_AY_ABC,                  /* A left, B centre, C right */
    ZR_AY_ACB                   /* A left, C centre, B right */
};

/* Port A. Either callback may be NULL (`in`: the pins read FFh). */
typedef struct zr_ay_io {
    void *ctx;
    uint8_t (*in)(void *ctx);
    void (*out)(void *ctx, uint8_t value);
} zr_ay_io;

#define ZR_AY_LATENCY 16u           /* the beeper's (beeper.h) */
#define ZR_AY_RING 8192u            /* frames kept between the shell's pulls (a power of two) */
#define ZR_AY_BACKLOG 8000u         /* the most a pull hands out; earlier frames are dropped */

typedef struct zr_ay {
    zr_dev dev;
    zr_ay_io io;
    int mode;
    uint32_t num, den;                      /* frames per master-clock tick, reduced */
    uint32_t tick_int, tick_rem, tick_den;  /* master-clock ticks per tick of the /8 divider: int + rem / den */
    uint8_t reg[16];
    uint8_t selected, valid;                /* the latched register; 0 when the last address was not the chip's */
    uint16_t tone_cnt[3];
    uint8_t tone_out[3];
    uint8_t noise_cnt;
    uint32_t lfsr;                          /* the 17-bit noise register */
    uint32_t env_cnt;
    uint8_t env, env_up, env_hold;          /* the envelope level, counting up, held */
    uint8_t pins;                           /* what the port A pins carry */
    int32_t out_l, out_r;                   /* the mixed output now, in quarters of a DAC unit */
    uint64_t time;                          /* guest time the chip has run to */
    uint64_t tick_at;                       /* guest time of the next tick of the /8 divider */
    uint32_t tick_acc;
    uint64_t frame;                         /* the frame being summed */
    uint64_t fstart, bound;                 /* its first guest time and the first guest time of the next frame */
    uint64_t done;                          /* the next frame to be handed out */
    int64_t sum_l, sum_r;                   /* output times ticks over the frame so far */
    double hp_keep, hp_x[2], hp_y[2];
    int16_t ring[ZR_AY_RING][2];            /* the finished frames from `done` to `frame` */
} zr_ay;

/* Sets the chip up on `host` for a master clock of `master_hz`, a chip clock
 * of `chip_hz`, output frames at `rate` Hz and a mixing mode, and resets
 * it. `io` may be NULL (the port has nothing connected). */
void zr_ay_init(zr_ay *a, const zr_hw_host *host, uint32_t master_hz, uint32_t chip_hz, uint32_t rate, int mode, const zr_ay_io *io);
void zr_ay_set_mode(zr_ay *a, int mode);
/* Bus access at the host's now(). */
void zr_ay_select(zr_ay *a, uint8_t address);
void zr_ay_write(zr_ay *a, uint8_t value);
uint8_t zr_ay_read(zr_ay *a);
/* The index of the next frame the render call will produce (see
 * zr_beeper_position). */
static inline uint64_t zr_ay_position(const zr_ay *a) { return a->done; }
/* As zr_beeper_render and zr_beeper_render_add: the chip runs to `until`
 * first. */
size_t zr_ay_render(zr_ay *a, uint64_t until, int16_t *stereo, size_t frames);
size_t zr_ay_render_add(zr_ay *a, uint64_t until, int16_t *stereo, size_t frames);

/* The ops table of the dev (hw.h): read, write as above; `reset` is the
 * RESET pin (every register 0, the counters and the envelope cleared; the
 * sound in flight is kept); `save` and `load` carry the registers, the
 * counters, the sound in flight and the filter, not the configuration. */

#endif /* ZR_HW_AY8912_H */

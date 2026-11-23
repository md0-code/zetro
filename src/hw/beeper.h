/* ZetRo - the 1-bit sound of these machines: the loudspeaker (bit 4 of port
 * FE), the MIC output (bit 3) and the tape's EAR signal heard through the
 * speaker, with room for one more 1-bit source (a board's own).
 *
 * Each input is a logic level with a weight, the voltage it adds to the
 * mixed output when high, in output units (the 16-bit range of the sound
 * the board hands to the shell). The board sets an input with
 * zr_beeper_set(b, input, level, time) whenever the guest changes it,
 * stamped with the guest time (master-clock ticks) of the change. The chip
 * has no registers and no deadline.
 *
 * Every change of the mixed level becomes a band-limited step in the
 * output: a windowed-sinc step response (Blackman window, 32 taps, -6 dB at
 * 17 kHz at 48 kHz) summed at the edge's exact position between two output
 * frames, so a 2 kHz beep, a tape's 3 kHz pulses and a loader's 10 kHz
 * carrier alike make no aliasing: whatever is above the filter is simply
 * gone. The output is the cone's motion: the mixed level high-passed at
 * about 30 Hz (a loudspeaker and its coupling capacitor ignore DC), so a
 * held level decays to silence and each change clicks as it does on the
 * real machine.
 *
 * Output format = machine.h's sound: interleaved stereo 16-bit frames (both
 * channels equal) at the rate given at init, derived exactly from guest
 * time with integer arithmetic: frame n is the signal at master-clock time
 * n * clock / rate (rate and clock are reduced by their common factor, so
 * 48000 Hz from 14 MHz is 3 frames per 875 ticks). The board's audio pull
 * calls zr_beeper_render with the machine's time.
 *
 * A frame is final only ZR_BEEPER_LATENCY frames after the last time the
 * board has told the beeper about (the kernel reaches that far ahead of an
 * edge), so the output lags guest time by 0.33 ms at 48 kHz. The AY
 * (ay8912.h) lags the same number of frames and keeps the same backlog
 * rule, so a board that calls both with the same time and the same buffer
 * size gets the same frames from each and may add them (zr_*_render_add).
 *
 * Portable C11. ZetRo's own code. */
#ifndef ZR_HW_BEEPER_H
#define ZR_HW_BEEPER_H

#include "hw.h"

#include <stddef.h>

/* The inputs (the pins of ops->input; the state is the level). */
enum { ZR_BEEPER_SPEAKER, ZR_BEEPER_MIC, ZR_BEEPER_EAR, ZR_BEEPER_AUX, ZR_BEEPER_INPUTS };

/* The band-limiting kernel reaches this many frames either side of an edge. */
#define ZR_BEEPER_LATENCY 16u
/* Frames kept between the shell's pulls (a power of two), and how many of
 * them a pull may hand out at most: when the shell is late, the sound
 * before the last ZR_BEEPER_BACKLOG frames is dropped (0.17 s at 48 kHz), so
 * the shell must pull at least that often (a frame loop does). */
#define ZR_BEEPER_RING 8192u
#define ZR_BEEPER_BACKLOG 8000u
#define ZR_BEEPER_STEP_RES 64u              /* step table entries per frame */
#define ZR_BEEPER_TABLE (2u * ZR_BEEPER_LATENCY * ZR_BEEPER_STEP_RES + 1u)

typedef struct zr_beeper {
    zr_dev dev;
    uint32_t clock_hz, rate;
    uint32_t num, den;                      /* frames per master-clock tick, reduced */
    int32_t weight[ZR_BEEPER_INPUTS];
    uint8_t level[ZR_BEEPER_INPUTS];
    uint64_t done;                          /* the next frame to be produced */
    uint64_t top;                           /* one past the last frame that has a pending change */
    int32_t naive;                          /* the unfiltered mixed level at frame `done` */
    double hp_keep, hp_x, hp_y;             /* the high-pass: its pole, the last input and output */
    int32_t step[ZR_BEEPER_RING];           /* net level change that takes effect at a frame (the unfiltered square wave) */
    double acc[ZR_BEEPER_RING];             /* band-limiting corrections summed for a frame */
    double table[ZR_BEEPER_TABLE];          /* the band-limited step response over 2 * LATENCY frames */
} zr_beeper;

/* Sets the beeper up on `host` (its now() is used by ops->input and reset),
 * for a master clock of `clock_hz` and output frames at `rate` Hz, and
 * resets it. The weights start as: speaker 10000, MIC 1500, EAR 3000, AUX 0
 * (not measured on a machine: the board sets its own). The state file
 * carries the levels and the sound in flight, not the weights. */
void zr_beeper_init(zr_beeper *b, const zr_hw_host *host, uint32_t clock_hz, uint32_t rate);
/* The weight of an input: the output units it adds when high. */
void zr_beeper_set_weight(zr_beeper *b, unsigned input, int32_t weight);
/* The input changed to `level` (0 or 1) at guest time `t`. */
void zr_beeper_set(zr_beeper *b, unsigned input, int level, uint64_t t);
/* The index of the next frame the render call will produce (frame n is at
 * master-clock time n * clock / rate): for a board that wants to line two
 * sound sources up after the shell fell behind. */
static inline uint64_t zr_beeper_position(const zr_beeper *b) { return b->done; }
/* Produces up to `frames` stereo frames, in order, from where the last call
 * stopped, only the frames final at guest time `until`: fewer than asked
 * when the machine has not run that far. Returns the frames written (2
 * int16_t each). _add adds them, saturating, to what the buffer holds
 * (another source's frames) instead of overwriting it. */
size_t zr_beeper_render(zr_beeper *b, uint64_t until, int16_t *stereo, size_t frames);
size_t zr_beeper_render_add(zr_beeper *b, uint64_t until, int16_t *stereo, size_t frames);

/* The ops table of the dev (hw.h): `input` takes the ZR_BEEPER_* input as
 * the pin and the level as the state, stamped with the host's now();
 * `reset` is silence from now on (the inputs keep their levels); `save` and
 * `load` carry the levels, the filter and the changes not yet rendered. */

#endif /* ZR_HW_BEEPER_H */

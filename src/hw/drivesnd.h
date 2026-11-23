/* ZetRo - the sound of the diskette drives, synthesised, from nothing but the
 * drives' own mechanical events (hw.h: zr_drive_hook): every sound is a few
 * damped resonances, a burst of noise and a slow oscillator, summed. The resonances, their decays, their levels and
 * their timing are those found in recordings of an Alps Electric DF354H148F
 * 3.5-inch drive; no sample of a
 * recording is played.
 *
 *  - A step is about 40 ms of sound, most of it between 400 Hz and 1.6 kHz:
 *    a low knock (405 and 553 Hz) under a rasp of noise, the motor's ring
 *    (1.4 and 1.7 kHz) 7 ms later, the strike on the frame's resonance near
 *    811 Hz at 13 ms, and the carriage settling on it at 23 and 32 ms. Each
 *    step is a little different (pitch and level drawn from a fixed pseudo-
 *    random sequence); a step outwards is a little softer, one against the
 *    end stop heavier, and the settling of a step is dropped when the next
 *    step comes first, so a seek is a run of knocks that ends in one settle.
 *  - The motor starts with a low kick and, 43 ms later, the bright click of
 *    the mechanism engaging (2.1 to 2.5 kHz, three times a step's peak). The
 *    head load of a drive with a head-load solenoid is that click at a third
 *    of the level, the unload softer and lower, a hold time after the last
 *    command that used the head (the controller's HUT).
 *  - The running motor is the tones of a 240 Hz series and one near 816 Hz
 *    over a band of noise, swelling with the disk's turn (5 Hz); it is at
 *    speed within a tenth of a second and runs down in pulses in three.
 *  Drive n sits a little to the left (even n) or right (odd) of the middle.
 *  Levels are well below the speaker's: a step peaks at about 4500 of 32768 at
 *  volume 100, the motor's click at about 15000, and a running motor has an
 *  RMS of about 100; the default volume is 50.
 *
 * Time. Events come in stamped with guest time (periods of the board's master
 * clock) from the drive and the controller, and are played at the output
 * frame of their time. The output follows the conventions of the beeper
 * (beeper.h): interleaved stereo 16-bit frames at a rate given at init, frame
 * n at master-clock time n * clock / rate, a frame final only
 * ZR_DRIVESND_LATENCY frames after the time the board asks for and the same
 * backlog rule, so a board that calls zr_beeper_render_add, zr_ay_render_add
 * and zr_drivesnd_render_add with the same time and the same buffer gets the
 * same frames from each. The drives do no per-sample work; a source that is
 * disabled keeps no hook on the drives.
 *
 * Deterministic: the same events and the same frames give the same samples,
 * whatever the size of the pulls. No C library math (the sines and decays are
 * summed from series). Portable C11. ZetRo's own code. */
#ifndef ZR_DRIVESND_H
#define ZR_DRIVESND_H

#include "hw.h"

#include <stddef.h>

#define ZR_DRIVESND_DRIVES 4u           /* diskette drives the source tells apart (drive numbers above it are ignored) */
#define ZR_DRIVESND_LATENCY 16u         /* the beeper's (beeper.h) */
#define ZR_DRIVESND_BACKLOG 8000u       /* the most a pull hands out; earlier frames are dropped */
#define ZR_DRIVESND_QUEUE 512u          /* events kept between pulls (the oldest are played at once beyond it) */
#define ZR_DRIVESND_RES 128u            /* damped resonances sounding at once */
#define ZR_DRIVESND_BURSTS 48u          /* noise bursts sounding at once */
#define ZR_DRIVESND_TABLE 1024u         /* the oscillator's sine table */

typedef struct zr_drivesnd_item {
    uint64_t t;
    uint32_t a, b;
    uint8_t drive, kind, dead;
} zr_drivesnd_item;

typedef struct zr_drivesnd_res {        /* a damped resonance: x(n) = A r^n cos(n w) as a rotating phasor */
    double x, y, c, s, r, left, right;
    uint8_t live;
} zr_drivesnd_res;

typedef struct zr_drivesnd_burst {      /* a decaying burst of low-passed noise */
    double env, decay, lp, k, left, right;
    uint8_t live;
} zr_drivesnd_burst;

typedef struct zr_drivesnd_spin {       /* a diskette motor: its speed fraction (0 to 1) and its oscillators */
    double level, target, lp1, lp2;
    uint32_t phase[4];                  /* the 240 Hz series, the disk's turn, the 816 Hz tone, the run-down's pulses */
} zr_drivesnd_spin;

typedef struct zr_drivesnd {
    int enabled;
    double gain;                        /* volume / 100 */
    uint32_t clock_hz, rate;
    uint32_t num, den;                  /* frames per master-clock tick, reduced */
    double inc;                         /* a 32-bit phase's increment for 1 Hz */
    uint64_t done;                      /* the next frame to be produced */
    uint32_t rng;                       /* the pseudo-random sequence */
    zr_drivesnd_item q[ZR_DRIVESND_QUEUE];
    unsigned qh, qn;                    /* the queue's first and one past its last, sorted by time */
    uint64_t next_frame;                /* the first queued event's frame; UINT64_MAX when none */
    uint64_t hold_t[ZR_DRIVESND_DRIVES];/* the pending head unload's time, ZR_NEVER when none */
    uint8_t loaded;                     /* bit n: drive n's head is loaded (as far as the events say) */
    uint8_t motor;                      /* bit n: drive n's motor is on (as far as the events say) */
    zr_drivesnd_spin spin[ZR_DRIVESND_DRIVES];
    uint8_t spin_mask;                  /* the drives whose motor sounds (bit n: drive n) */
    zr_drivesnd_res res[ZR_DRIVESND_RES];
    zr_drivesnd_burst burst[ZR_DRIVESND_BURSTS];
    unsigned res_next, burst_next, nres, nburst;
    double atk, rel;                    /* the motor's smoothing per sample */
    double table[ZR_DRIVESND_TABLE + 1u];
} zr_drivesnd;

/* Sets the source up for a master clock of `clock_hz` and output frames at `rate` Hz, disabled, with
 * the first frame at guest time 0 (zr_drivesnd_reset moves it). */
void zr_drivesnd_init(zr_drivesnd *s, uint32_t clock_hz, uint32_t rate);
/* Switches the sound on or off and sets its volume (0 to 100 percent; 0 is off). A change of the state
 * clears everything that sounds or waits. The drives need a hook only while it is enabled. */
void zr_drivesnd_set(zr_drivesnd *s, int enabled, unsigned volume);
static inline int zr_drivesnd_enabled(const zr_drivesnd *s) { return s->enabled; }
/* Clears everything that sounds or waits (a reset, a state load) and puts the next frame at guest time `t`. */
void zr_drivesnd_reset(zr_drivesnd *s, uint64_t t);
/* The hook for the drives (hw.h: zr_drive_hook; zr_fdd_set_hook); `ctx` is the source. Ignored while disabled. */
void zr_drivesnd_event(void *ctx, unsigned drive, unsigned event, uint64_t t, uint32_t a, uint32_t b);
/* The index of the next frame the render call will produce (see zr_beeper_position). */
static inline uint64_t zr_drivesnd_position(const zr_drivesnd *s) { return s->done; }
/* As zr_beeper_render and zr_beeper_render_add: produces up to `frames` stereo frames, in order, from where
 * the last call stopped, only the frames final at guest time `until`: fewer than asked when the machine has
 * not run that far. Returns the frames written (2 int16_t each). _add adds them, saturating, to what the
 * buffer holds (the beeper's frames) instead of overwriting it. A disabled source writes silence (_add:
 * leaves the buffer) and keeps its position. */
size_t zr_drivesnd_render(zr_drivesnd *s, uint64_t until, int16_t *stereo, size_t frames);
size_t zr_drivesnd_render_add(zr_drivesnd *s, uint64_t until, int16_t *stereo, size_t frames);
/* Something sounds or is waiting to (tests, and the idle shortcut). */
int zr_drivesnd_busy(const zr_drivesnd *s);

#endif /* ZR_DRIVESND_H */

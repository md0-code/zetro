/* The desktop loop's timing: what to do next
 * to keep guest time in step with wall time. No SDL, no OS call and no clock
 * of its own: the shell passes the monotonic clock's ticks in, so a test
 * drives it with a fake clock. ZetRo's own code.
 *
 * Paced, the machine owes guest time at the master clock's rate from an anchor
 * (a clock reading and the guest time then): it is run in slices up to what
 * wall time has earned, and a picture is shown on a schedule of its own
 * (one frame period of the display adapter, re-read from the machine at
 * every picture, so a CRTC reprogrammed to another rate is followed). The
 * schedule is kept exactly (fractions of a clock tick are carried), so
 * neither the guest time nor the pictures drift against the clock. A host
 * that stalls (a moved window, a debugger) leaves the machine behind by
 * more than the catch-up bound: the lag is dropped, not raced through.
 * Unthrottled, the machine runs back to back and one picture in a given
 * number is shown; the clock is not read at all. */
#ifndef ZR_PACE_H
#define ZR_PACE_H

#include <stdint.h>

/* Pictures shown one in this many frame periods when unthrottled. */
#define ZR_PACE_FREE_FRAMES 10u
/* The longest slice of guest time run at once while paced (ticks of guest time: 1 ms). */
#define ZR_PACE_SLICE_MS 1u

typedef enum zr_pace_action {
    ZR_PACE_RUN,        /* run the machine for `ticks` ticks of guest time, then pump keys, sound and ports */
    ZR_PACE_PRESENT,    /* show a picture now */
    ZR_PACE_WAIT        /* nothing to do for `ticks` clock ticks: sleep (less than a millisecond: just poll again) */
} zr_pace_action;

typedef struct zr_pace_step {
    zr_pace_action action;
    uint64_t ticks;
} zr_pace_step;

typedef struct zr_pace {
    uint64_t freq;              /* clock ticks per second */
    double osc_per_tick;        /* guest ticks of guest time per clock tick */
    uint64_t catch_up;          /* the lag (ticks of guest time) that is caught up; a longer one is dropped */
    uint64_t slice;             /* ticks of guest time of the longest run */
    uint64_t anchor, anchor_guest;
    double next_frame;          /* the clock reading the next picture is due at */
    unsigned free_ran;          /* unthrottled: frame periods run since the last picture */
    int late;                   /* 1: a picture was just asked for; 2: its cost is known, the machine has its turn */
    uint64_t pic_at, pic_cost;  /* when that picture (then the machine's turn) began; the clock ticks it took */
} zr_pace;

/* `freq` clock ticks a second; the machine's master clock runs at `clock_hz` (machine.h: zr_machine_clock);
 * lags up to `catch_up_ms` of wall time are caught up. */
void zr_pace_init(zr_pace *p, uint64_t freq, uint32_t clock_hz, unsigned catch_up_ms);
/* The pacing starts again: guest time `guest` is now (after a pause, a
 * machine that was replaced, a mode change). */
void zr_pace_reset(zr_pace *p, uint64_t now, uint64_t guest);
/* The machine is not running (paused, the setup screen): its guest time `guest` is now, so nothing
 * is owed when it runs again; the pictures keep their schedule. */
void zr_pace_hold(zr_pace *p, uint64_t now, uint64_t guest);
/* The next thing to do at clock reading `now` with the machine at guest time
 * `guest` whose frame period is `period` ticks of guest time; `free_running` is
 * unthrottled. */
zr_pace_step zr_pace_next(zr_pace *p, int free_running, uint64_t now, uint64_t guest, uint64_t period);
/* The guest time wall time has earned by `now`, as zr_pace_next counts it. */
uint64_t zr_pace_due(const zr_pace *p, uint64_t now);

#endif /* ZR_PACE_H */

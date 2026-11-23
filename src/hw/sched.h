/* ZetRo - the board's event scheduler.
 *
 * Each chip has at most one deadline (hw.h: zr_hw_host.schedule arms it,
 * ZR_NEVER cancels, re-arming replaces it), in OSC periods. The board asks
 * the scheduler to run every deadline up to a time; each falls due in time
 * order (ties in the order the chips first scheduled) and calls the chip's
 * ops->event with the deadline as `now`. While an event runs, zr_sched_now
 * gives that deadline, so whatever the chip does then (an edge it drives, a
 * new deadline it arms) is stamped with its own time, not the CPU's.
 *
 * Nothing runs per clock: a run with no deadline due costs one comparison.
 * A deadline armed in the past runs at once, at the time of the event that
 * armed it (time never goes backwards inside a run).
 *
 * Portable C11. ZetRo's own code. */
#ifndef ZR_SCHED_H
#define ZR_SCHED_H

#include "hw.h"

#define ZR_SCHED_MAX 32

typedef struct zr_sched {
    zr_dev *dev[ZR_SCHED_MAX];
    uint64_t when[ZR_SCHED_MAX];
    unsigned count;
    uint64_t next;              /* the earliest deadline (ZR_NEVER: none) */
    uint64_t event_time;        /* the deadline of the event running */
    int in_event;               /* an event is running */
} zr_sched;

void zr_sched_init(zr_sched *s);
/* Arms dev's deadline (ZR_NEVER cancels). 0 when the table is full. */
int zr_sched_set(zr_sched *s, zr_dev *dev, uint64_t when);
/* dev's deadline, ZR_NEVER when none. */
uint64_t zr_sched_get(const zr_sched *s, const zr_dev *dev);
/* Runs every deadline at or before `until`, in time order. */
void zr_sched_run(zr_sched *s, uint64_t until);
/* Inline check for the hot paths: anything due at `t`? */
static inline int zr_sched_due(const zr_sched *s, uint64_t t) { return s->next <= t; }

/* --- the CPU's clock and the board's run loop ------------------------------------------
 *
 * Guest time is ticks of the board's master clock; the Z80 counts its own
 * T-states (zr_z80.t). The CPU runs at the master clock divided by a whole
 * number, ticks_per_tstate (4 for 3.5 MHz from 14 MHz). A board whose CPU
 * clock can change at run time (a speed switch) calls zr_tclock_set_rate
 * at the T-state of the change: the clock takes that instant as its new
 * origin, so guest time stays continuous; a board with several CPU rates
 * picks a master clock all of them divide (time is only counted in it).
 * Chips stamp everything with zr_tclock_time of the T-state their cycle
 * starts at. */
struct zr_z80;
struct zr_state_writer;
struct zr_state_reader;

typedef struct zr_tclock {
    uint64_t ticks0, t0;                /* the origin: guest time ticks0 is T-state t0 */
    uint32_t ticks_per_tstate;
} zr_tclock;

static inline void zr_tclock_init(zr_tclock *c, uint32_t ticks_per_tstate, uint64_t t, uint64_t ticks) {
    c->ticks0 = ticks;
    c->t0 = t;
    c->ticks_per_tstate = ticks_per_tstate;
}
/* The guest time of T-state t (t at or after the origin). */
static inline uint64_t zr_tclock_time(const zr_tclock *c, uint64_t t) { return c->ticks0 + (t - c->t0) * c->ticks_per_tstate; }
static inline void zr_tclock_set_rate(zr_tclock *c, uint64_t t, uint32_t ticks_per_tstate) {
    c->ticks0 = zr_tclock_time(c, t);
    c->t0 = t;
    c->ticks_per_tstate = ticks_per_tstate;
}
/* The clock in the fields of a chunk the board has begun, and back. */
void zr_tclock_save(const zr_tclock *c, struct zr_state_writer *w);
void zr_tclock_load(zr_tclock *c, struct zr_state_reader *r);

/* The board's run: instructions and deadlines interleaved until guest time
 * reaches `until` (the last instruction may end past it; the next run
 * starts from there). Deadlines run between instructions: after an
 * instruction ends at or past a deadline, the deadline's event runs, with
 * its own time, before the next instruction starts, so an interrupt raised
 * during an instruction is taken at its end, as the Z80 samples INT then.
 * A deadline armed by a cycle (an OUT that starts a timer) is seen at once.
 * Returns the guest time reached. The scheduler's deadlines live in the
 * chips (each chip's load arms its own again), so the scheduler itself has
 * no state to save; the clock has (zr_tclock_save). */
uint64_t zr_sched_run_z80(zr_sched *s, struct zr_z80 *z, const zr_tclock *c, uint64_t until);

#endif /* ZR_SCHED_H */

/* ZetRo - the board's event scheduler (sched.h). ZetRo's own code. */
#include "sched.h"
#include "state.h"
#include "z80.h"

#include <string.h>

static void recompute(zr_sched *s) {
    uint64_t best = ZR_NEVER;
    for (unsigned i = 0; i < s->count; ++i)
        if (s->when[i] < best) best = s->when[i];
    s->next = best;
}

void zr_sched_init(zr_sched *s) {
    memset(s, 0, sizeof *s);
    s->next = ZR_NEVER;
}

int zr_sched_set(zr_sched *s, zr_dev *dev, uint64_t when) {
    unsigned i = 0;
    while (i < s->count && s->dev[i] != dev) ++i;
    if (i == s->count) {
        if (when == ZR_NEVER) return 1;
        if (s->count == ZR_SCHED_MAX) return 0;
        s->dev[s->count++] = dev;
    }
    /* Time never goes backwards inside a run: a deadline already past is
     * due at the time of the event that arms it. */
    if (s->in_event && when < s->event_time) when = s->event_time;
    const uint64_t old = s->when[i];
    s->when[i] = when;
    if (when <= s->next) s->next = when;
    else if (old == s->next) recompute(s);
    return 1;
}

uint64_t zr_sched_get(const zr_sched *s, const zr_dev *dev) {
    for (unsigned i = 0; i < s->count; ++i)
        if (s->dev[i] == dev) return s->when[i];
    return ZR_NEVER;
}

void zr_sched_run(zr_sched *s, uint64_t until) {
    if (s->in_event) return;              /* an event's own catch-up: the run in progress continues */
    while (s->next <= until) {
        unsigned first = 0;
        for (unsigned i = 1; i < s->count; ++i)
            if (s->when[i] < s->when[first]) first = i;
        const uint64_t when = s->when[first];
        zr_dev *dev = s->dev[first];
        s->when[first] = ZR_NEVER;
        recompute(s);
        s->in_event = 1;
        s->event_time = when;
        dev->ops->event(dev, when);
        s->in_event = 0;
    }
}

void zr_tclock_save(const zr_tclock *c, zr_state_writer *w) {
    zr_sw_u64(w, c->ticks0);
    zr_sw_u64(w, c->t0);
    zr_sw_u32(w, c->ticks_per_tstate);
}

void zr_tclock_load(zr_tclock *c, zr_state_reader *r) {
    c->ticks0 = zr_sr_u64(r);
    c->t0 = zr_sr_u64(r);
    c->ticks_per_tstate = zr_sr_u32(r);
    if (!c->ticks_per_tstate) c->ticks_per_tstate = 1;
}

uint64_t zr_sched_run_z80(zr_sched *s, zr_z80 *z, const zr_tclock *c, uint64_t until) {
    uint64_t now = zr_tclock_time(c, z->t);
    zr_sched_run(s, now);
    while (now < until) {
        /* Instructions up to the earlier of the next deadline and the end. */
        do {
            zr_z80_step(z);
            now = zr_tclock_time(c, z->t);
        } while (now < until && now < s->next);
        zr_sched_run(s, now);
    }
    return now;
}

/* The desktop loop's timing (pace.h). ZetRo's own code. */
#include "pace.h"

void zr_pace_init(zr_pace *p, uint64_t freq, uint32_t clock_hz, unsigned catch_up_ms) {
    const double osc = (double)clock_hz;
    p->freq = freq ? freq : 1000000u;
    p->osc_per_tick = osc / (double)p->freq;
    p->catch_up = (uint64_t)(osc * catch_up_ms / 1000.0);
    p->slice = (uint64_t)(osc * ZR_PACE_SLICE_MS / 1000.0);
    p->anchor = p->anchor_guest = 0;
    p->next_frame = 0;
    p->free_ran = 0;
    p->late = 0;
    p->pic_at = p->pic_cost = 0;
}

void zr_pace_reset(zr_pace *p, uint64_t now, uint64_t guest) {
    p->anchor = now;
    p->anchor_guest = guest;
    p->next_frame = (double)now;
    p->free_ran = 0;
}

void zr_pace_hold(zr_pace *p, uint64_t now, uint64_t guest) {
    p->anchor = now;
    p->anchor_guest = guest;
}

uint64_t zr_pace_due(const zr_pace *p, uint64_t now) {
    return p->anchor_guest + (uint64_t)((double)(now - p->anchor) * p->osc_per_tick);
}

zr_pace_step zr_pace_next(zr_pace *p, int free_running, uint64_t now, uint64_t guest, uint64_t period) {
    zr_pace_step s = {ZR_PACE_WAIT, 0};
    if (free_running) {
        if (p->free_ran >= ZR_PACE_FREE_FRAMES) {
            p->free_ran = 0;
            s.action = ZR_PACE_PRESENT;
        } else {
            ++p->free_ran;
            s.action = ZR_PACE_RUN;
            s.ticks = period;
        }
        return s;
    }
    const double frame_ticks = (double)period / p->osc_per_tick;
    if (p->late == 1) {                 /* back from a picture: what it cost, and the machine's turn starts */
        p->pic_cost = now - p->pic_at;
        p->pic_at = now;
        p->late = 2;
    }
    /* A picture a whole period overdue comes first, so a host too slow to catch up still shows the machine; but
     * only after the machine has had as much time as the last picture took: a display that takes longer than two
     * periods to show one (a remote desktop, a 30 Hz screen with vsync) would otherwise leave the machine none. */
    if ((double)now >= p->next_frame + frame_ticks && (!p->late || now - p->pic_at >= p->pic_cost)) {
        p->next_frame = (double)now + frame_ticks;
        p->late = 1;
        p->pic_at = now;
        s.action = ZR_PACE_PRESENT;
        return s;
    }
    /* Run what wall time owes, a whole slice at a time (so the machine is a little ahead of the clock as often as behind
     * it, and a picture finds it caught up); a lag beyond the bound is cut to the bound, so the machine still
     * advances when every picture takes longer than the bound. */
    uint64_t due = zr_pace_due(p, now);
    if (due > guest + p->catch_up) {
        p->anchor = now;
        p->anchor_guest = guest + p->catch_up;
        due = p->anchor_guest;
    }
    const uint64_t owed = due > guest ? due - guest : 0;
    if (owed >= p->slice) {
        s.action = ZR_PACE_RUN;
        s.ticks = p->slice;
        return s;
    }
    /* Within a slice of the clock: a picture when its time has come, else wait for it or for the next slice. */
    if ((double)now >= p->next_frame) {
        /* A picture later than a period (a slow display) is counted from now, not owed again. */
        p->next_frame = (double)now >= p->next_frame + frame_ticks ? (double)now + frame_ticks : p->next_frame + frame_ticks;
        p->late = 1;
        p->pic_at = now;
        s.action = ZR_PACE_PRESENT;
        return s;
    }
    const uint64_t to_frame = (uint64_t)(p->next_frame - (double)now) + 1u;
    const uint64_t to_slice = (uint64_t)((double)(p->slice - owed) / p->osc_per_tick) + 1u;
    s.ticks = to_frame < to_slice ? to_frame : to_slice;
    return s;
}

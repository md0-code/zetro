/* The sound output's clock sync (audiosync.h). ZetRo's own code. */
#include "audiosync.h"

#include <string.h>

#define MAX_NUDGE 0.005
/* Proportional gain: a queue 100 % off the target asks for this nudge. */
#define GAIN 0.01
/* The queue's length is read in chunks the size of the device's buffer: smoothed over about this many pumps. */
#define SMOOTH 0.01

void zr_audiosync_init(zr_audiosync *a, uint32_t in_rate, uint32_t out_rate) {
    memset(a, 0, sizeof *a);
    a->in_rate = in_rate ? in_rate : 48000u;
    a->out_rate = out_rate ? out_rate : 48000u;
    a->target = a->out_rate * ZR_AUDIOSYNC_TARGET_MS / 1000.0;
    a->level = a->target;
}

void zr_audiosync_observe(zr_audiosync *a, size_t queued) {
    a->level += ((double)queued - a->level) * SMOOTH;
    double nudge = (a->level - a->target) / a->target * GAIN;     /* too full: pass input faster */
    if (nudge > MAX_NUDGE) nudge = MAX_NUDGE;
    if (nudge < -MAX_NUDGE) nudge = -MAX_NUDGE;
    a->adjust = nudge;
}

/* Input frames each output frame takes: the rate ratio, shortened when the queue is too full. */
static double step_of(const zr_audiosync *a) { return a->in_rate / a->out_rate * (1.0 + a->adjust); }

size_t zr_audiosync_capacity(const zr_audiosync *a, size_t in) {
    return (size_t)((double)in / (a->in_rate / a->out_rate * (1.0 - MAX_NUDGE))) + 4u;
}

size_t zr_audiosync_convert(zr_audiosync *a, const int16_t *in, size_t frames, int16_t *out, unsigned gain) {
    const double step = step_of(a);
    size_t n = 0;
    for (size_t i = 0; i < frames; ++i) {
        if (!a->primed) {
            a->prev[0] = in[0];
            a->prev[1] = in[1];
            a->primed = 1;
            a->pos = 0.0;
            continue;
        }
        const int16_t *cur = in + 2 * i;
        /* Output frames that fall between the previous input frame and this one. */
        while (a->pos < 1.0) {
            for (unsigned c = 0; c < 2; ++c) {
                const double v = a->prev[c] + (cur[c] - a->prev[c]) * a->pos;
                out[2 * n + c] = (int16_t)((long)(v < 0 ? v - 0.5 : v + 0.5) * (long)gain / 256);
            }
            ++n;
            a->pos += step;
        }
        a->pos -= 1.0;
        a->prev[0] = cur[0];
        a->prev[1] = cur[1];
    }
    return n;
}

size_t zr_audiosync_refill(const zr_audiosync *a, size_t queued) {
    return (double)queued < a->target ? (size_t)(a->target - (double)queued) : 0u;
}

size_t zr_audiosync_wanted(const zr_audiosync *a, size_t queued, size_t limit) {
    if ((double)queued >= a->target) return 0;
    size_t in = (size_t)((a->target - (double)queued) * a->in_rate / a->out_rate) + 1u;
    return in < limit ? in : limit;
}

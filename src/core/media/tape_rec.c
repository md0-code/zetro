/* Recording: what the machine's SAVE put on the MIC output becomes new
 * blocks of the tape. ZetRo's own code.
 *
 * The deck gives the T-states between the edges. A pulse longer than 20 ms is
 * silence (a loader's pulses are all a few milliseconds at most): it ends the
 * block before it, as that block's pause. Each run of pulses between two
 * silences is looked at as a block of the ROM's own speed - a pilot tone of
 * about 2168 T-states, two sync pulses of about 667 and 735, then pairs of
 * pulses of about 855 (a 0 bit) or 1710 (a 1 bit) making whole bytes - and
 * kept as a standard-speed block when it is one; any other run is kept as the
 * pulses it was, so that nothing is lost. */
#include "tape_impl.h"

#include <stdlib.h>
#include <string.h>

#define SILENCE 70000u                  /* T-states: 20 ms */

static int within(uint32_t v, uint32_t low, uint32_t high) { return v >= low && v <= high; }

/* Reads the pulses as a ROM-speed block; the bytes in `out` (at most 65535) and their count in *len. */
static int decode_standard(const uint32_t *q, size_t m, uint8_t *out, size_t *len) {
    size_t i = 0;
    while (i < m && within(q[i], 1626, 2710)) ++i;
    if (i < 256) return 0;                                                  /* a pilot tone of some length */
    if (i + 2 > m || !within(q[i], 500, 834) || !within(q[i + 1], 551, 919)) return 0;
    i += 2;
    const size_t pairs = (m - i) / 2;                                       /* a pulse left over is the end of the tone */
    if (!pairs || pairs % 8 || pairs / 8 > 0xFFFF) return 0;
    for (size_t k = 0; k < pairs; ++k, i += 2) {
        unsigned bit;
        if (within(q[i], 640, 1070) && within(q[i + 1], 640, 1070)) bit = 0;
        else if (within(q[i], 1280, 2140) && within(q[i + 1], 1280, 2140)) bit = 1;
        else return 0;
        if (k % 8 == 0) out[k / 8] = 0;
        out[k / 8] = (uint8_t)(out[k / 8] << 1 | bit);
    }
    *len = pairs / 8;
    return 1;
}

static unsigned to_ms(uint64_t tstates) {
    const uint64_t ms = (tstates + ZR_TAPE_TSTATE_HZ / 2000u) / (ZR_TAPE_TSTATE_HZ / 1000u);
    return ms < 1 ? 1 : ms > 65535 ? 65535 : (unsigned)ms;
}

/* One run of pulses, the first at `level`, as a block. */
static int add_run(zr_tape *t, const uint32_t *q, size_t m, uint8_t level, unsigned pause) {
    uint8_t *bytes = malloc(m / 16 + 1);
    size_t len = 0;
    if (!bytes) return 0;
    int ok = 0;
    if (decode_standard(q, m, bytes, &len)) {
        size_t off;
        if (zr_tape_put(t, bytes, len, &off)) {
            zr_tape_block *b = zr_tape_add_block(t, ZR_TB_STANDARD);
            if (b && zr_tape_fill_standard(t, b, off, len, pause)) { zr_tape_measure(t, b); ok = 1; }
            else if (b) --t->n;
        }
    } else {
        const uint32_t first = (uint32_t)t->runs_n;
        ok = 1;
        for (size_t i = 0; ok && i < m; ++i) ok = zr_tape_run(t, first, q[i], 1);
        if (ok) {
            zr_tape_block *b = zr_tape_add_block(t, ZR_TB_PULSES);
            if (b) {
                b->flags = ZR_TF_LEVEL;
                b->level = level;
                b->pause = pause;
                b->u.r.first = first;
                b->u.r.count = (uint32_t)(t->runs_n - first);
                zr_tape_measure(t, b);
            } else {
                ok = 0;
            }
        }
        if (!ok) t->runs_n = first;
    }
    free(bytes);
    return ok;
}

int zr_tape_add_recording(zr_tape *t, const uint32_t *pulses, size_t n, uint8_t first_level, uint64_t tail) {
    if (!n && !tail) return 0;
    /* Two edges at the same time are no pulse at all: they are taken out. */
    uint32_t *q = malloc((n ? n : 1) * sizeof *q);
    if (!q) return 0;
    size_t m = 0;
    uint8_t level = first_level & 1u;
    for (size_t i = 0; i < n; ++i) {
        if (pulses[i] == 0) {
            if (m == 0) level ^= 1u;
            else if (i + 1 < n) {
                const uint64_t sum = (uint64_t)q[m - 1] + pulses[i + 1];
                q[m - 1] = sum > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)sum;
                ++i;
            }
            continue;
        }
        q[m++] = pulses[i];
    }
    int added = 0;
    size_t a = 0;
    while (a < m) {
        size_t b = a;
        while (b < m && q[b] < SILENCE) ++b;
        unsigned pause;
        size_t next;
        if (b < m) { pause = to_ms(q[b]); next = b + 1; }
        else { pause = to_ms(tail < 1000u * (ZR_TAPE_TSTATE_HZ / 1000u) ? 1000u * (ZR_TAPE_TSTATE_HZ / 1000u) : tail); next = m; }
        if (b > a) {
            if (!add_run(t, q + a, b - a, (uint8_t)(level ^ (a & 1u)), pause)) break;
            added = 1;
        }
        a = next;
    }
    free(q);
    if (added) t->modified = 1;
    return added;
}

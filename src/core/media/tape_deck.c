/* The tape deck (tape.h): plays a tape as the EAR level in guest time, and
 * records the MIC output into new blocks. ZetRo's own code.
 *
 * The generator (tape_gen.c) yields stretches of constant level, in T-states
 * of 3.5 MHz. The deck turns each into master-clock ticks with
 * ticks = (carry + T-states * clock) / 3.5 MHz, keeping the remainder as the
 * next carry, so the sum of any run of stretches is exact whatever the clock.
 *
 * The stream is held as: the current `level` until `edge`, and one pending
 * stretch of the opposite level after it. Stretches of the same level are
 * merged into the current one as they are fetched, so `edge` is always the
 * time of a real change (or the end of the tape), which is what
 * zr_tape_deck_next_edge needs. */
#include "tape_impl.h"

#include <stdlib.h>
#include <string.h>

#define SPIN_LIMIT (1u << 20)
#define MERGE_LIMIT 65536u              /* stretches of one level merged before the deck gives a bound instead of an edge */
#define REC_LIMIT (1u << 23)            /* pulses of one recording */

void zr_tape_deck_init(zr_tape_deck *d, uint32_t clock_hz) {
    memset(d, 0, sizeof *d);
    d->clock = clock_hz ? clock_hz : ZR_TAPE_TSTATE_HZ;
    d->gen.block = ZR_TAPE_NO_BLOCK;
}

void zr_tape_deck_release(zr_tape_deck *d) {
    free(d->rec);
    d->rec = NULL;
    d->rec_n = d->rec_cap = 0;
    d->recording = 0;
}

/* The stream back to the start of `block`, stopped, with nothing generated yet. */
static void reset_stream(zr_tape_deck *d, uint32_t block) {
    memset(&d->flow, 0, sizeof d->flow);
    d->flow.next = block;
    memset(&d->gen, 0, sizeof d->gen);
    d->gen.block = ZR_TAPE_NO_BLOCK;
    d->gen_level = 0;
    d->level = 0;
    d->end = 0;
    d->spin = 0;
    d->rem = 0;
    d->fresh = 1;
    d->has_pend = 0;
    d->pend_level = 0;
    d->rest = 0;
    d->edge = 0;
    d->shown_block = block;
    d->shown_elapsed = 0;
}

void zr_tape_deck_set_tape(zr_tape_deck *d, zr_tape *tape) {
    if (d->recording) zr_tape_deck_record(d, 0, d->now);
    d->playing = 0;
    d->tape = tape;
    reset_stream(d, 0);
}

void zr_tape_deck_set_48k(zr_tape_deck *d, int is_48k) {
    d->is_48k = is_48k != 0;
}

/* T-states to ticks of the master clock, exact over any run of calls. */
static uint64_t to_ticks(zr_tape_deck *d, uint64_t tstates) {
    const uint64_t v = d->rem + tstates * d->clock;
    d->rem = v % ZR_TAPE_TSTATE_HZ;
    return v / ZR_TAPE_TSTATE_HZ;
}

/* The next stretch of the tape, with the block and the T-states into it it belongs to. 0 at a stop
 * or the end (d->end says which). */
static int next_segment(zr_tape_deck *d, zr_seg *s, uint32_t *block, uint64_t *before) {
    const zr_tape *t = d->tape;
    for (;;) {
        if (d->gen.block != ZR_TAPE_NO_BLOCK) {
            *block = d->gen.block;
            *before = d->gen.elapsed;
            if (zr_tape_gen_next(t, &d->gen, &d->gen_level, s)) {
                d->gen.elapsed += s->tstates;
                if (s->tstates) d->spin = 0;
                else if (++d->spin > SPIN_LIMIT) { d->end = 2; return 0; }
                return 1;
            }
            d->gen.block = ZR_TAPE_NO_BLOCK;
        }
        uint32_t next;
        switch (zr_tape_flow_step(t, &d->flow, d->is_48k ? ZR_FLOW_PLAY48K : ZR_FLOW_PLAY, &d->gen_level, &d->spin, &next)) {
        case ZR_FLOW_BLOCK:
            zr_tape_gen_begin(t, &d->gen, next, &d->gen_level);
            break;
        case ZR_FLOW_STOP:
            d->end = 1;
            return 0;
        default:
            d->end = 2;
            return 0;
        }
    }
}

/* Fetches stretches until a pending one of the opposite level is found or the tape is over. */
static void refill(zr_tape_deck *d) {
    unsigned merged = 0;
    while ((!d->has_pend || (!d->pend_ticks && d->pend_level != d->level)) && !d->end) {
        zr_seg s;
        uint32_t block;
        uint64_t before;
        if (!next_segment(d, &s, &block, &before)) break;
        const uint64_t ticks = to_ticks(d, s.tstates);
        if (d->fresh) { d->shown_block = block; d->shown_elapsed = before; d->fresh = 0; }
        if (d->has_pend) {
            /* The pending stretch has no length yet: a level held for no time is no change at all,
             * so what follows it either goes on at its level or is back at the current one. */
            if (s.level == d->pend_level) d->pend_ticks += ticks;
            else { d->has_pend = 0; d->edge += ticks; }
        } else if (s.level == d->level) {
            d->edge += ticks;
            if (++merged > MERGE_LIMIT) {
                /* A bound, not an edge: the level goes on, ask again at this time. */
                d->has_pend = 1;
                d->pend_level = d->level;
                d->pend_ticks = 0;
                d->pend_block = block;
                d->pend_elapsed = before + s.tstates;
            }
        } else {
            d->has_pend = 1;
            d->pend_level = s.level;
            d->pend_ticks = ticks;
            d->pend_block = block;
            d->pend_elapsed = before;
        }
    }
}

int zr_tape_deck_play(zr_tape_deck *d, uint64_t time) {
    if (!d->tape || d->recording) return 0;
    if (d->playing) return 1;
    d->now = time;
    d->edge = time + d->rest;
    d->rest = 0;
    d->playing = 1;
    refill(d);
    if (!d->has_pend && d->end == 2 && d->edge == time) {
        d->playing = 0;
        d->shown_block = d->tape->n;
        d->shown_elapsed = 0;
        return 0;
    }
    return 1;
}

void zr_tape_deck_advance(zr_tape_deck *d, uint64_t time) {
    if (!d->playing) return;
    if (time < d->now) time = d->now;
    for (;;) {
        if (time < d->edge) break;
        if (d->has_pend) {
            d->level = d->pend_level;
            d->edge += d->pend_ticks;
            d->has_pend = 0;
            d->shown_block = d->pend_block;
            d->shown_elapsed = d->pend_elapsed;
            refill(d);
            continue;
        }
        if (!d->end) {
            refill(d);
            if (d->has_pend) continue;
            if (!d->end) d->end = 2;
        }
        /* The stream is over: the tape stopped at `edge`. */
        d->playing = 0;
        d->rest = 0;
        d->shown_elapsed = 0;
        if (d->end == 1) {
            d->shown_block = d->flow.next;
            d->end = 0;                 /* a stop block: playing again goes on after it */
        } else {
            d->shown_block = d->tape->n;
        }
        break;
    }
    d->now = time;
}

void zr_tape_deck_stop(zr_tape_deck *d, uint64_t time) {
    if (!d->playing) return;
    zr_tape_deck_advance(d, time);
    if (!d->playing) return;
    d->rest = d->edge > time ? d->edge - time : 0;
    d->playing = 0;
}

void zr_tape_deck_rewind(zr_tape_deck *d, uint64_t time) {
    (void)time;
    d->playing = 0;
    reset_stream(d, 0);
}

void zr_tape_deck_seek(zr_tape_deck *d, uint32_t block, uint64_t time) {
    if (!d->tape || d->recording) return;
    const int was_playing = d->playing;
    const uint32_t count = d->tape->n;
    d->playing = 0;
    reset_stream(d, block > count ? count : block);
    if (was_playing) zr_tape_deck_play(d, time);
}

void zr_tape_deck_next(zr_tape_deck *d, uint64_t time) {
    zr_tape_deck_advance(d, time);
    zr_tape_deck_seek(d, d->shown_block + 1, time);
}

void zr_tape_deck_previous(zr_tape_deck *d, uint64_t time) {
    zr_tape_deck_advance(d, time);
    zr_tape_deck_seek(d, d->shown_block ? d->shown_block - 1 : 0, time);
}

int zr_tape_deck_playing(const zr_tape_deck *d) { return d->playing; }
uint32_t zr_tape_deck_block(const zr_tape_deck *d) { return d->shown_block; }
uint64_t zr_tape_deck_position(const zr_tape_deck *d) { return d->shown_elapsed; }
int zr_tape_deck_recording(const zr_tape_deck *d) { return d->recording; }

uint32_t zr_tape_deck_find_data(const zr_tape_deck *d) {
    if (!d->tape) return ZR_TAPE_NO_BLOCK;
    for (uint32_t i = d->shown_block + (d->shown_elapsed ? 1u : 0u); i < d->tape->n; ++i)
        if (zr_tape_block_is_rom_data(d->tape, &d->tape->blocks[i])) return i;
    return ZR_TAPE_NO_BLOCK;
}

int zr_tape_deck_level(zr_tape_deck *d, uint64_t time) {
    zr_tape_deck_advance(d, time);
    return d->level;
}

uint64_t zr_tape_deck_next_edge(zr_tape_deck *d) {
    if (!d->playing) return ZR_TAPE_NEVER;
    zr_tape_deck_advance(d, d->now);
    if (!d->playing) return ZR_TAPE_NEVER;
    return d->has_pend ? d->edge : ZR_TAPE_NEVER;
}

/* --- the edges, read ahead ---------------------------------------------------------------------- */

void zr_tape_edges_begin(zr_tape_edges *e, const zr_tape *tape, uint32_t clock_hz, int is_48k) {
    zr_tape_deck_init(&e->deck, clock_hz);
    zr_tape_deck_set_48k(&e->deck, is_48k);
    zr_tape_deck_set_tape(&e->deck, (zr_tape *)tape);       /* borrowed and only read: nothing is recorded through this deck */
    e->time = 0;
    e->running = zr_tape_deck_play(&e->deck, 0);
}

int zr_tape_edges_next(zr_tape_edges *e, int *level, uint64_t *ticks) {
    while (e->running) {
        const int lv = zr_tape_deck_level(&e->deck, e->time);
        uint64_t span = 0;
        for (;;) {
            const uint64_t edge = zr_tape_deck_next_edge(&e->deck);
            if (edge == ZR_TAPE_NEVER) {
                e->running = 0;
                return 0;
            }
            span += edge - e->time;
            e->time = edge;
            /* After 65536 merged stretches the deck gives a bound, not an edge: the level goes on. */
            if (zr_tape_deck_level(&e->deck, edge) != lv) break;
        }
        if (!span) continue;                                /* the level held for no time at the start of the tape */
        *level = lv;
        *ticks = span;
        return 1;
    }
    return 0;
}

/* --- recording --------------------------------------------------------------------------------- */

int zr_tape_deck_record(zr_tape_deck *d, int on, uint64_t time) {
    if (on) {
        if (!d->tape) return 0;
        if (d->recording) return 1;
        zr_tape_deck_stop(d, time);
        d->recording = 1;
        d->rec_n = 0;
        d->rec_started = 0;
        d->rec_overflow = 0;
        return 1;
    }
    if (!d->recording) return 1;
    d->recording = 0;
    int added = 0;
    if (d->rec_started && d->tape) {
        uint64_t dt = time > d->rec_time ? time - d->rec_time : 0;
        if (dt > ((uint64_t)1 << 40)) dt = (uint64_t)1 << 40;
        const uint64_t tail = (d->rec_rem + dt * ZR_TAPE_TSTATE_HZ) / d->clock;
        added = zr_tape_add_recording(d->tape, d->rec, d->rec_n, d->rec_first_level, tail);
    }
    d->rec_n = 0;
    d->rec_started = 0;
    if (d->tape) reset_stream(d, d->tape->n);
    return added;
}

void zr_tape_deck_output(zr_tape_deck *d, int level, uint64_t time) {
    if (!d->recording || d->rec_overflow) return;
    const uint8_t lv = (uint8_t)(level & 1);
    if (!d->rec_started) {
        d->rec_started = 1;
        d->rec_level = d->rec_first_level = lv;
        d->rec_time = time;
        d->rec_rem = 0;
        return;
    }
    if (lv == d->rec_level) return;
    uint64_t dt = time > d->rec_time ? time - d->rec_time : 0;
    if (dt > ((uint64_t)1 << 40)) dt = (uint64_t)1 << 40;
    const uint64_t v = d->rec_rem + dt * ZR_TAPE_TSTATE_HZ;
    uint64_t tstates = v / d->clock;
    d->rec_rem = v % d->clock;
    if (tstates > 0xFFFFFFFFu) tstates = 0xFFFFFFFFu;
    if (d->rec_n >= d->rec_cap) {
        if (d->rec_cap >= REC_LIMIT) { d->rec_overflow = 1; return; }
        const size_t cap = d->rec_cap ? d->rec_cap * 2 : 4096;
        uint32_t *p = realloc(d->rec, cap * sizeof *p);
        if (!p) { d->rec_overflow = 1; return; }
        d->rec = p;
        d->rec_cap = cap;
    }
    d->rec[d->rec_n++] = (uint32_t)tstates;
    d->rec_level = lv;
    d->rec_time = time;
}

/* --- states ------------------------------------------------------------------------------------ */

#define DECK_STATE_VERSION 1

void zr_tape_deck_save(const zr_tape_deck *d, zr_state_writer *w) {
    zr_sw_u8(w, DECK_STATE_VERSION);
    zr_sw_u8(w, (uint8_t)((d->playing ? 1 : 0) | (d->has_pend ? 2 : 0) | (d->fresh ? 4 : 0)));
    zr_sw_u8(w, d->level);
    zr_sw_u8(w, d->pend_level);
    zr_sw_u8(w, d->gen_level);
    zr_sw_u8(w, d->end);
    zr_sw_u32(w, d->flow.next);
    zr_sw_u32(w, d->flow.depth);
    for (unsigned i = 0; i < ZR_TAPE_STACK; ++i) {
        zr_sw_u32(w, d->flow.frame[i].block);
        zr_sw_u32(w, d->flow.frame[i].left);
        zr_sw_u8(w, d->flow.frame[i].call);
    }
    zr_sw_u32(w, d->gen.block);
    zr_sw_u8(w, d->gen.phase);
    zr_sw_u32(w, d->gen.i);
    zr_sw_u32(w, d->gen.j);
    zr_sw_u32(w, d->gen.k);
    zr_sw_u64(w, d->gen.elapsed);
    zr_sw_u64(w, d->rem);
    zr_sw_u64(w, d->now);
    zr_sw_u64(w, d->playing ? d->edge : d->rest);   /* absolute while the tape runs, the part left otherwise */
    zr_sw_u64(w, d->pend_ticks);
    zr_sw_u64(w, d->pend_elapsed);
    zr_sw_u32(w, d->pend_block);
    zr_sw_u32(w, d->shown_block);
    zr_sw_u64(w, d->shown_elapsed);
}

int zr_tape_deck_load(zr_tape_deck *d, zr_state_reader *r) {
    zr_tape_deck n;
    memcpy(&n, d, sizeof n);
    const unsigned version = zr_sr_u8(r);
    const unsigned flags = zr_sr_u8(r);
    n.level = zr_sr_u8(r) & 1u;
    n.pend_level = zr_sr_u8(r) & 1u;
    n.gen_level = zr_sr_u8(r) & 1u;
    n.end = zr_sr_u8(r);
    n.flow.next = zr_sr_u32(r);
    n.flow.depth = zr_sr_u32(r);
    for (unsigned i = 0; i < ZR_TAPE_STACK; ++i) {
        n.flow.frame[i].block = zr_sr_u32(r);
        n.flow.frame[i].left = zr_sr_u32(r);
        n.flow.frame[i].call = zr_sr_u8(r) & 1u;
    }
    n.gen.block = zr_sr_u32(r);
    n.gen.phase = zr_sr_u8(r);
    n.gen.i = zr_sr_u32(r);
    n.gen.j = zr_sr_u32(r);
    n.gen.k = zr_sr_u32(r);
    n.gen.elapsed = zr_sr_u64(r);
    n.rem = zr_sr_u64(r);
    n.now = zr_sr_u64(r);
    const uint64_t edge = zr_sr_u64(r);
    n.pend_ticks = zr_sr_u64(r);
    n.pend_elapsed = zr_sr_u64(r);
    n.pend_block = zr_sr_u32(r);
    n.shown_block = zr_sr_u32(r);
    n.shown_elapsed = zr_sr_u64(r);
    n.playing = flags & 1u ? 1 : 0;
    n.has_pend = flags & 2u ? 1 : 0;
    n.fresh = flags & 4u ? 1 : 0;
    n.spin = 0;
    if (n.playing) { n.edge = edge; n.rest = 0; } else { n.rest = edge; n.edge = 0; }
    n.rem %= ZR_TAPE_TSTATE_HZ;
    n.recording = 0;
    n.rec_n = 0;
    n.rec_started = 0;

    /* The fields must fit the tape that is in the deck, or the deck is rewound. */
    const uint32_t count = d->tape ? d->tape->n : 0;
    int ok = zr_sr_ok(r) && version == DECK_STATE_VERSION && n.end <= 2 && n.flow.depth <= ZR_TAPE_STACK;
    ok = ok && n.flow.next <= count && n.shown_block <= count && (!n.has_pend || n.pend_block < count || !count);
    for (unsigned i = 0; ok && i < n.flow.depth; ++i) ok = n.flow.frame[i].block < count;
    ok = ok && (n.gen.block == ZR_TAPE_NO_BLOCK || n.gen.block < count);
    if (!d->tape) {
        d->playing = 0;
        return ok;
    }
    if (!ok) {
        d->playing = 0;
        reset_stream(d, 0);
        return 0;
    }
    memcpy(d, &n, sizeof *d);
    return 1;
}

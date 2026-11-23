/* The tape as a stream of pulses: which block plays next (jumps, loops and
 * calls) and, inside a block, the stretches of constant level it makes. The
 * deck plays them and the writers read them. ZetRo's own code, written from
 * the TZX 1.20 block descriptions and the PZX 1.0 block layouts.
 *
 * A stretch has a level and a length in T-states. The generator keeps the
 * level of the next pulse: every pulse of the standard, turbo, tone, pulse
 * sequence and pure data blocks is at that level and flips it, so each pulse
 * ends in an edge. Silence is different: it lasts one millisecond at the
 * level the next pulse would have had (the closing edge of the data before
 * it) and then goes low, and the next pulse after it is low too. */
#include "tape_impl.h"

#include <string.h>

#define GUARD (1u << 22)                /* steps in one call that make no pulse, before the block is given up */
#define MS ((uint64_t)(ZR_TAPE_TSTATE_HZ / 1000u))

enum { PH_PILOT, PH_SYNC, PH_BITS, PH_TAIL, PH_PAUSE, PH_DONE, PH_RUNS, PH_GPILOT, PH_GDATA };

static void to_phase(zr_tape_gen *g, uint8_t phase) {
    g->phase = phase;
    g->i = g->j = g->k = 0;
}

void zr_tape_gen_begin(const zr_tape *t, zr_tape_gen *g, uint32_t block, uint8_t *level) {
    const zr_tape_block *b = &t->blocks[block];
    memset(g, 0, sizeof *g);
    g->block = block;
    if (b->flags & ZR_TF_LEVEL) *level = b->level & 1u;
    switch (b->kind) {
    case ZR_TB_STANDARD: case ZR_TB_TURBO: case ZR_TB_TONE: case ZR_TB_DATA: g->phase = PH_PILOT; break;
    case ZR_TB_DIRECT: g->phase = PH_BITS; break;
    case ZR_TB_PULSES: case ZR_TB_AUDIO: g->phase = PH_RUNS; break;
    case ZR_TB_GENERAL: g->phase = PH_GPILOT; break;
    case ZR_TB_PAUSE: g->phase = PH_PAUSE; break;
    default: g->phase = PH_DONE; break;
    }
}

/* A pulse at the level of the next one, which flips. */
static int pulse(zr_seg *s, uint8_t *level, uint64_t tstates) {
    s->level = *level;
    s->tstates = tstates;
    *level ^= 1u;
    return 1;
}

/* How a generalized data symbol starts: its flags say whether it begins with an edge, continues the
 * level, or forces it. */
static void symbol_start(uint8_t flags, uint8_t *level) {
    switch (flags & 3u) {
    case 1: *level ^= 1u; break;
    case 2: *level = 0; break;
    case 3: *level = 1; break;
    default: break;
    }
}

int zr_tape_gen_next(const zr_tape *t, zr_tape_gen *g, uint8_t *level, zr_seg *s) {
    if (g->block >= t->n) return 0;
    const zr_tape_block *b = &t->blocks[g->block];
    const uint8_t *arena = t->arena;
    for (unsigned guard = 0; guard < GUARD; ++guard) {
        switch (g->phase) {
        case PH_PILOT:
            if (g->i < b->u.d.pilot_count) { ++g->i; return pulse(s, level, b->u.d.pilot); }
            to_phase(g, PH_SYNC);
            break;
        case PH_SYNC:
            if (g->i < b->u.d.sync_n) { const uint32_t len = g->i ? b->u.d.sync2 : b->u.d.sync1; ++g->i; return pulse(s, level, len); }
            to_phase(g, PH_BITS);
            break;
        case PH_BITS:
            if (b->kind == ZR_TB_DIRECT) {
                /* Samples one bit each, the first in the top bit; a run of equal samples is one stretch
                 * (cut at 65536 samples, the stretches then join in the deck). */
                const uint64_t total = b->len ? (uint64_t)(b->len - 1) * 8 + b->used : 0;
                uint64_t pos = (uint64_t)g->i * 8 + g->j;
                if (g->j > 7 || pos >= total) { to_phase(g, PH_PAUSE); break; }
                const unsigned first = arena[b->off + pos / 8] >> (7 - pos % 8) & 1u;
                uint64_t n = 0;
                while (pos < total && n < 65536) {
                    if ((arena[b->off + pos / 8] >> (7 - pos % 8) & 1u) != first) break;
                    ++pos;
                    ++n;
                }
                g->i = (uint32_t)(pos / 8);
                g->j = (uint32_t)(pos % 8);
                s->level = (uint8_t)first;
                s->tstates = n * b->u.direct.sample;
                *level = (uint8_t)first;
                return 1;
            } else {
                /* i the byte, j the bit of it, k the pulse of the bit's sequence. */
                if (g->i >= b->len) { to_phase(g, PH_TAIL); break; }
                const unsigned bits = g->i + 1 == b->len ? b->used : 8u;
                if (g->j >= bits) { ++g->i; g->j = 0; g->k = 0; break; }
                const unsigned bit = arena[b->off + g->i] >> (7 - g->j) & 1u;
                const unsigned n = bit ? b->u.d.one_n : b->u.d.zero_n;
                if (g->k >= n) { ++g->j; g->k = 0; break; }
                const size_t at = b->u.d.table + 2 * ((bit ? b->u.d.zero_n : 0u) + (size_t)g->k);
                ++g->k;
                return pulse(s, level, zr_rd16(arena + at));
            }
        case PH_TAIL:
            if (g->i == 0 && b->u.d.tail) { g->i = 1; return pulse(s, level, b->u.d.tail); }
            to_phase(g, PH_PAUSE);
            break;
        case PH_PAUSE:
            if (!b->pause) { to_phase(g, PH_DONE); break; }
            if (g->i == 0) { g->i = 1; s->level = *level; s->tstates = MS; return 1; }
            if (g->i == 1) {
                g->i = 2;
                *level = 0;
                if (b->pause > 1) { s->level = 0; s->tstates = (uint64_t)(b->pause - 1) * MS; return 1; }
            }
            to_phase(g, PH_DONE);
            break;
        case PH_RUNS: {
            if (g->i >= b->u.r.count || (size_t)b->u.r.first + g->i >= t->runs_n) {
                if ((b->flags & ZR_TF_KEEP) && g->j) *level ^= 1u;
                to_phase(g, PH_PAUSE);
                break;
            }
            const zr_pulse *run = &t->runs[b->u.r.first + g->i];
            if (g->k >= run->count) { ++g->i; g->k = 0; break; }
            ++g->k;
            g->j = 1;
            return pulse(s, level, run->duration);
        }
        case PH_GPILOT: {
            /* i the entry of the pilot and sync table, k the repetition of it, j the pulse of the symbol. */
            if (g->i >= b->u.g.totp) { to_phase(g, PH_GDATA); break; }
            const uint8_t *entry = arena + b->u.g.prle + 3 * (size_t)g->i;
            if (g->k >= zr_rd16(entry + 1)) { ++g->i; g->k = 0; g->j = 0; break; }
            if (entry[0] >= b->u.g.asp) { to_phase(g, PH_DONE); break; }
            const uint8_t *def = arena + b->u.g.sym_p + (size_t)entry[0] * (1u + 2u * b->u.g.npp);
            if (g->j == 0) symbol_start(def[0], level);
            const uint32_t len = g->j < b->u.g.npp ? zr_rd16(def + 1 + 2 * (size_t)g->j) : 0;
            if (!len) { ++g->k; g->j = 0; break; }
            ++g->j;
            return pulse(s, level, len);
        }
        case PH_GDATA: {
            /* i the data symbol, j the pulse of it. The stream holds b->u.g.nb bits a symbol, top bit first. */
            if (g->i >= b->u.g.totd) { to_phase(g, PH_PAUSE); break; }
            const uint64_t at = (uint64_t)g->i * b->u.g.nb;
            uint32_t value = 0;
            for (unsigned q = 0; q < b->u.g.nb; ++q) {
                const uint64_t bit = at + q;
                value = value << 1 | (arena[b->u.g.stream + bit / 8] >> (7 - bit % 8) & 1u);
            }
            if (value >= b->u.g.asd) { to_phase(g, PH_DONE); break; }
            const uint8_t *def = arena + b->u.g.sym_d + (size_t)value * (1u + 2u * b->u.g.npd);
            if (g->j == 0) symbol_start(def[0], level);
            const uint32_t len = g->j < b->u.g.npd ? zr_rd16(def + 1 + 2 * (size_t)g->j) : 0;
            if (!len) { ++g->i; g->j = 0; break; }
            ++g->j;
            return pulse(s, level, len);
        }
        default:
            return 0;
        }
    }
    to_phase(g, PH_DONE);
    return 0;
}

/* --- which block plays next ------------------------------------------------------------------- */

#define SPIN_LIMIT (1u << 20)

static int in_range(const zr_tape *t, int64_t target) {
    return target >= 0 && target <= (int64_t)t->n;
}

/* The offset of call number `k` of a call block (the TZX block as it is in the arena). */
static int32_t call_offset(const zr_tape *t, const zr_tape_block *b, uint32_t k) {
    const uint8_t *p = t->arena + b->raw + 3 + 2 * (size_t)k;
    return (int16_t)(uint16_t)zr_rd16(p);
}

int zr_tape_flow_step(const zr_tape *t, zr_tape_flow *f, int mode, uint8_t *level, uint32_t *spin, uint32_t *block) {
    for (;;) {
        if (f->next >= t->n) return ZR_FLOW_END;
        if (++*spin > SPIN_LIMIT) return ZR_FLOW_END;
        const uint32_t index = f->next++;
        const zr_tape_block *b = &t->blocks[index];
        switch (b->kind) {
        case ZR_TB_STANDARD: case ZR_TB_TURBO: case ZR_TB_TONE: case ZR_TB_PULSES: case ZR_TB_DATA: case ZR_TB_DIRECT:
        case ZR_TB_AUDIO: case ZR_TB_GENERAL: case ZR_TB_PAUSE:
            *block = index;
            return ZR_FLOW_BLOCK;
        case ZR_TB_LEVEL:
            *level = b->level & 1u;
            break;
        case ZR_TB_STOP:
            *block = index;
            return ZR_FLOW_STOP;
        case ZR_TB_STOP48:
            if (mode == ZR_FLOW_PLAY48K) return ZR_FLOW_STOP;
            if (mode == ZR_FLOW_WALK) { *block = index; return ZR_FLOW_STOP48; }
            break;
        case ZR_TB_GROUP: case ZR_TB_TEXT:
            if (mode == ZR_FLOW_WALK) { *block = index; return ZR_FLOW_TEXT; }
            break;
        case ZR_TB_JUMP: {
            const int64_t target = (int64_t)index + b->u.f.value;
            if (!in_range(t, target) || target == (int64_t)index) return ZR_FLOW_END;
            f->next = (uint32_t)target;
            break;
        }
        case ZR_TB_LOOP:
            if (f->depth && !f->frame[f->depth - 1].call && f->frame[f->depth - 1].block == index) {
                f->frame[f->depth - 1].left = b->u.f.value > 1 ? (uint32_t)b->u.f.value - 1 : 0;
            } else {
                if (f->depth >= ZR_TAPE_STACK) return ZR_FLOW_END;
                f->frame[f->depth].block = index;
                f->frame[f->depth].left = b->u.f.value > 1 ? (uint32_t)b->u.f.value - 1 : 0;
                f->frame[f->depth].call = 0;
                ++f->depth;
            }
            break;
        case ZR_TB_LOOP_END:
            if (f->depth && !f->frame[f->depth - 1].call) {
                if (f->frame[f->depth - 1].left) { --f->frame[f->depth - 1].left; f->next = f->frame[f->depth - 1].block + 1; }
                else --f->depth;
            }
            break;
        case ZR_TB_CALL:
            if (b->u.f.value > 0) {
                const int64_t target = (int64_t)index + call_offset(t, b, 0);
                if (!in_range(t, target) || f->depth >= ZR_TAPE_STACK) return ZR_FLOW_END;
                f->frame[f->depth].block = index;
                f->frame[f->depth].left = 0;
                f->frame[f->depth].call = 1;
                ++f->depth;
                f->next = (uint32_t)target;
            }
            break;
        case ZR_TB_RETURN:
            if (f->depth && f->frame[f->depth - 1].call) {
                const uint32_t call = f->frame[f->depth - 1].block;
                const uint32_t k = ++f->frame[f->depth - 1].left;
                if (k < (uint32_t)t->blocks[call].u.f.value) {
                    const int64_t target = (int64_t)call + call_offset(t, &t->blocks[call], k);
                    if (!in_range(t, target)) return ZR_FLOW_END;
                    f->next = (uint32_t)target;
                } else {
                    --f->depth;
                    f->next = call + 1;
                }
            }
            break;
        default:
            break;
        }
    }
}

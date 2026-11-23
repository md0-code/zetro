/* Writing a tape as TAP, TZX or PZX (tape.h). ZetRo's own code, written from
 * the three format descriptions.
 *
 * TAP holds standard-speed blocks only. TZX keeps every block that was read
 * from a TZX exactly as it was (its raw bytes); the others are written as
 * the block that says the same: standard data as block 10, pure data as 14,
 * pulses as 13 and 12 (a pulse longer than 18 ms, which block 13 cannot say,
 * becomes a pause). PZX has no loops or calls, so the tape is played through
 * the way the deck would, loops unrolled, and what plays is written: data as
 * DATA blocks, pauses as PAUS, any other sound as the PULS of the pulses it
 * makes, so that what a PZX plays is what the deck played. */
#include "tape_impl.h"

#include <stdlib.h>
#include <string.h>

#define MS ((uint64_t)(ZR_TAPE_TSTATE_HZ / 1000u))

/* --- an output buffer ----------------------------------------------------------------------- */

typedef struct wbuf {
    uint8_t *p;
    size_t n, cap;
    int fail;
} wbuf;

static void w_bytes(wbuf *w, const void *data, size_t n) {
    if (w->fail) return;
    if (n > ZR_MEDIA_LIMIT || w->n > ZR_MEDIA_LIMIT - n) { w->fail = 1; return; }
    if (w->n + n > w->cap) {
        size_t cap = w->cap ? w->cap : 4096;
        while (cap < w->n + n) cap *= 2;
        uint8_t *p = realloc(w->p, cap);
        if (!p) { w->fail = 1; return; }
        w->p = p;
        w->cap = cap;
    }
    if (n) memcpy(w->p + w->n, data, n);
    w->n += n;
}

static void w8(wbuf *w, unsigned v) { const uint8_t b = (uint8_t)v; w_bytes(w, &b, 1); }
static void w16(wbuf *w, unsigned v) { const uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; w_bytes(w, b, 2); }
static void w24(wbuf *w, unsigned v) { const uint8_t b[3] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16)}; w_bytes(w, b, 3); }
static void w32(wbuf *w, uint32_t v) {
    const uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    w_bytes(w, b, 4);
}

/* --- the pulses a block makes, merged ------------------------------------------------------------ */

/* Takes the stretches of a block and gives out its pulses: stretches of one level are one pulse, and
 * the first pulse's level is told first. A stretch that changes the level without taking time is a
 * pulse of no length. */
typedef struct pulser {
    void (*start)(void *ctx, uint8_t level);
    void (*pulse)(void *ctx, uint32_t duration, uint32_t count);
    void *ctx;
    int started;
    uint8_t level;
    uint64_t length;
} pulser;

static void pulser_flush(pulser *p) {
    uint64_t length = p->length;
    while (length > 0xFFFFFFFFu) {              /* a pulse no format here can hold whole: split by a pulse of no length */
        p->pulse(p->ctx, 0xFFFFFFFFu, 1);
        p->pulse(p->ctx, 0, 1);
        length -= 0xFFFFFFFFu;
    }
    p->pulse(p->ctx, (uint32_t)length, 1);
}

static void pulser_add(pulser *p, uint8_t level, uint64_t tstates) {
    if (!p->started) {
        p->started = 1;
        p->level = level;
        p->length = tstates;
        p->start(p->ctx, level);
        return;
    }
    if (level == p->level) { p->length += tstates; return; }
    pulser_flush(p);
    p->level = level;
    p->length = tstates;
}

static void pulser_end(pulser *p) {
    if (p->started) pulser_flush(p);
    p->started = 0;
}

/* Plays one block from the generator into a pulser. `level` is the generator's level, kept up. */
static void play_block(const zr_tape *t, uint32_t index, uint8_t *level, pulser *p) {
    zr_tape_gen g;
    zr_seg s;
    zr_tape_gen_begin(t, &g, index, level);
    while (zr_tape_gen_next(t, &g, level, &s)) pulser_add(p, s.level, s.tstates);
    pulser_end(p);
}

/* --- TAP --------------------------------------------------------------------------------------- */

static int carries_sound(const zr_tape_block *b) {
    switch (b->kind) {
    case ZR_TB_STANDARD: case ZR_TB_TURBO: case ZR_TB_TONE: case ZR_TB_DATA: case ZR_TB_DIRECT: case ZR_TB_AUDIO: case ZR_TB_GENERAL:
        return 1;
    case ZR_TB_PULSES:
        return !(b->flags & ZR_TF_PAUSE);
    default:
        return 0;
    }
}

static void write_tap(const zr_tape *t, wbuf *w, unsigned *dropped) {
    for (uint32_t i = 0; i < t->n; ++i) {
        const zr_tape_block *b = &t->blocks[i];
        if (zr_tape_block_is_rom_data(t, b) && b->len <= 0xFFFF) {
            w16(w, (unsigned)b->len);
            w_bytes(w, t->arena + b->off, b->len);
        } else if (carries_sound(b)) {
            ++*dropped;
        }
    }
}

/* --- TZX --------------------------------------------------------------------------------------- */

static void tzx_pause(wbuf *w, uint64_t tstates) {
    uint64_t ms = (tstates + MS / 2) / MS;
    if (!ms) ms = 1;
    while (ms) {
        const unsigned part = ms > 65535 ? 65535u : (unsigned)ms;
        w8(w, 0x20);
        w16(w, part);
        ms -= part;
    }
}

/* Pulses as blocks 13 (up to 255 of them), 12 (a long run of one length) and, for a pulse too long
 * for either, a pause. `want` is the level the next pulse has on the tape being written; `have` the
 * level it will have when the TZX is played: a pause leaves the level low, so after one the two can
 * differ, and a level block then puts them together again (2 in `have`: not known). */
typedef struct tzx_pulses {
    wbuf *w;
    uint16_t held[255];
    unsigned held_n;
    uint8_t want, have;
} tzx_pulses;

static void tzx_flush(tzx_pulses *p) {
    if (!p->held_n) return;
    w8(p->w, 0x13);
    w8(p->w, p->held_n);
    for (unsigned i = 0; i < p->held_n; ++i) w16(p->w, p->held[i]);
    p->held_n = 0;
}

static void tzx_level(wbuf *w, unsigned level) {
    w8(w, 0x2B);
    w32(w, 1);
    w8(w, level & 1u);
}

static void tzx_pulse(void *ctx, uint32_t duration, uint32_t count) {
    tzx_pulses *p = ctx;
    if (duration > 65535) {
        tzx_flush(p);
        for (uint32_t k = 0; k < count && !p->w->fail; ++k) { tzx_pause(p->w, duration); p->want ^= 1u; }
        p->have = 0;
        return;
    }
    if (p->have != p->want) { tzx_flush(p); tzx_level(p->w, p->want); p->have = p->want; }
    p->want ^= (uint8_t)(count & 1u);
    p->have ^= (uint8_t)(count & 1u);
    if (count >= 8) {
        tzx_flush(p);
        while (count) {
            const unsigned part = count > 65535 ? 65535u : count;
            w8(p->w, 0x12);
            w16(p->w, duration);
            w16(p->w, part);
            count -= part;
        }
    } else {
        for (uint32_t k = 0; k < count; ++k) {
            p->held[p->held_n++] = (uint16_t)duration;
            if (p->held_n == 255) tzx_flush(p);
        }
    }
}

static void tzx_start(void *ctx, uint8_t level) {
    ((tzx_pulses *)ctx)->want = level;
}

static void write_tzx(const zr_tape *t, wbuf *w) {
    static const uint8_t HEADER[10] = {'Z', 'X', 'T', 'a', 'p', 'e', '!', 0x1A, 1, 20};
    w_bytes(w, HEADER, sizeof HEADER);
    uint8_t level = 0;
    tzx_pulses tp;
    tp.w = w;
    tp.held_n = 0;
    tp.want = tp.have = 0;
    for (uint32_t i = 0; i < t->n && !w->fail; ++i) {
        const zr_tape_block *b = &t->blocks[i];
        if (b->raw_len) { w_bytes(w, t->arena + b->raw, b->raw_len); tp.have = 2; continue; }
        const uint8_t *data = t->arena + b->off;
        const unsigned pause = b->pause > 65535 ? 65535u : b->pause;
        switch (b->kind) {
        case ZR_TB_STANDARD:
            if (b->len > 0xFFFF) break;
            w8(w, 0x10);
            w16(w, pause);
            w16(w, (unsigned)b->len);
            w_bytes(w, data, b->len);
            if (b->pause) tp.want = tp.have = 0;
            else { tp.want ^= (uint8_t)(b->u.d.pilot_count & 1u); if (tp.have < 2) tp.have ^= (uint8_t)(b->u.d.pilot_count & 1u); }
            break;
        case ZR_TB_PAUSE:
            if (b->pause) { w8(w, 0x20); w16(w, pause); tp.want = tp.have = 0; }
            break;
        case ZR_TB_STOP:
            w8(w, 0x20);
            w16(w, 0);
            break;
        case ZR_TB_STOP48:
            w8(w, 0x2A);
            w32(w, 0);
            break;
        case ZR_TB_LEVEL:
            tzx_level(w, b->level);
            tp.want = tp.have = b->level;
            break;
        case ZR_TB_TEXT:
            w8(w, 0x30);
            w8(w, b->len > 255 ? 255u : (unsigned)b->len);
            w_bytes(w, data, b->len > 255 ? 255 : b->len);
            break;
        case ZR_TB_DATA:
            if (b->u.d.zero_n == 2 && b->u.d.one_n == 2 && b->len <= 0xFFFFFF) {
                const uint8_t *tab = t->arena + b->u.d.table;
                if (zr_rd16(tab) == zr_rd16(tab + 2) && zr_rd16(tab + 4) == zr_rd16(tab + 6)) {
                    if (b->flags & ZR_TF_LEVEL) { tzx_level(w, b->level); tp.want = tp.have = b->level; }
                    w8(w, 0x14);
                    w16(w, zr_rd16(tab));
                    w16(w, zr_rd16(tab + 4));
                    w8(w, b->used);
                    w16(w, pause);
                    w24(w, (unsigned)b->len);
                    w_bytes(w, data, b->len);
                    if (b->u.d.tail > 0 && b->u.d.tail <= 0xFFFF) { w8(w, 0x12); w16(w, b->u.d.tail); w16(w, 1); tp.want ^= 1u; tp.have ^= 1u; }
                    break;
                }
            }
            {   /* the pulses it makes */
                pulser p = {tzx_start, tzx_pulse, &tp, 0, 0, 0};
                play_block(t, i, &level, &p);
                tzx_flush(&tp);
            }
            break;
        case ZR_TB_PULSES: case ZR_TB_AUDIO: {
            /* The runs as they are, the block's own pause as a pause block after them. */
            if (b->flags & ZR_TF_LEVEL) tp.want = b->level;
            for (uint32_t k = 0; k < b->u.r.count && b->u.r.first + k < t->runs_n; ++k)
                tzx_pulse(&tp, t->runs[b->u.r.first + k].duration, t->runs[b->u.r.first + k].count);
            tzx_flush(&tp);
            if (b->pause) { w8(w, 0x20); w16(w, pause); tp.want = tp.have = 0; }
            break;
        }
        default:
            break;
        }
    }
}

/* --- PZX --------------------------------------------------------------------------------------- */

typedef struct pzx_writer {
    wbuf *w;
    wbuf puls;                          /* the body of the PULS block being built */
    uint32_t run_duration, run_count;   /* the pulse run not yet in it */
    uint8_t level;                      /* the level of the next pulse, as the pulses of a PZX go */
} pzx_writer;

static void pzx_block(wbuf *w, const char tag[4], const uint8_t *body, size_t n) {
    w_bytes(w, tag, 4);
    w32(w, (uint32_t)n);
    w_bytes(w, body, n);
}

static void run_flush(pzx_writer *z) {
    if (!z->run_count) return;
    /* A duration of 64K or more starts with a word that would read as a repeat count, so it is given one. */
    if (z->run_count > 1 || z->run_duration > 0xFFFFu) w16(&z->puls, 0x8000u | z->run_count);
    if (z->run_duration < 0x8000u) w16(&z->puls, z->run_duration);
    else { w16(&z->puls, 0x8000u | z->run_duration >> 16); w16(&z->puls, z->run_duration & 0xFFFFu); }
    z->run_count = 0;
}

static void puls_flush(pzx_writer *z) {
    run_flush(z);
    if (z->puls.n) pzx_block(z->w, "PULS", z->puls.p, z->puls.n);
    z->puls.n = 0;
    if (z->puls.fail) z->w->fail = 1;
}

/* `count` pulses of `duration`; a pulse of 2^31 or more is cut by pulses of no length. */
static void puls_pulse(pzx_writer *z, uint32_t duration, uint32_t count) {
    while (duration > 0x7FFFFFFFu) {
        puls_pulse(z, 0x7FFFFFFFu, 1);
        puls_pulse(z, 0, 1);
        duration -= 0x7FFFFFFFu;
    }
    while (count) {
        const uint32_t part = count > 0x7FFFu ? 0x7FFFu : count;
        if (z->run_count && z->run_duration == duration && z->run_count + part <= 0x7FFFu) z->run_count += part;
        else { run_flush(z); z->run_duration = duration; z->run_count = part; }
        z->level ^= (uint8_t)(part & 1u);
        count -= part;
        if (z->puls.n > (1u << 20)) puls_flush(z);
    }
}

/* The next pulse is to be at `level`: a pulse of no length turns it round if it is not. */
static void puls_ensure(pzx_writer *z, uint8_t level) {
    if (z->level != level) puls_pulse(z, 0, 1);
}

static void pzx_pause(pzx_writer *z, uint64_t tstates) {
    puls_flush(z);
    while (tstates) {
        const uint32_t part = tstates > 0x7FFFFFFFu ? 0x7FFFFFFFu : (uint32_t)tstates;
        w_bytes(z->w, "PAUS", 4);
        w32(z->w, 4);
        w32(z->w, part);                /* at the low level */
        z->level = 1;
        tstates -= part;
    }
}

/* The silence after a block: a millisecond at the level the next pulse would have, then low. */
static void pzx_silence(pzx_writer *z, uint8_t *glv, unsigned ms, int tail_done) {
    if (!ms) return;
    if (!tail_done) { puls_ensure(z, *glv); puls_pulse(z, (uint32_t)MS, 1); }
    if (ms > 1) pzx_pause(z, (uint64_t)(ms - 1) * MS);
    *glv = 0;
}

static void pzx_start(void *ctx, uint8_t level) { puls_ensure(ctx, level); }
static void pzx_pulse(void *ctx, uint32_t duration, uint32_t count) { puls_pulse(ctx, duration, count); }

/* A data block as DATA, after the PULS of its pilot and sync pulses. */
static void pzx_data(pzx_writer *z, const zr_tape *t, const zr_tape_block *b, uint8_t *glv) {
    const uint8_t *data = t->arena + b->off;
    const int sound = b->kind == ZR_TB_STANDARD || b->kind == ZR_TB_TURBO;
    if (b->flags & ZR_TF_LEVEL) *glv = b->level & 1u;
    puls_ensure(z, *glv);
    if (sound) {
        puls_pulse(z, b->u.d.pilot, b->u.d.pilot_count);
        if (b->u.d.sync_n) { puls_pulse(z, b->u.d.sync1, 1); puls_pulse(z, b->u.d.sync2, 1); }
        *glv ^= (uint8_t)((b->u.d.pilot_count + b->u.d.sync_n) & 1u);
    }
    puls_flush(z);
    uint64_t ones = 0;
    for (size_t i = 0; i < b->len; ++i) {
        const unsigned used = i + 1 == b->len ? b->used : 8u;
        for (unsigned m = data[i] >> (8 - used); m; m &= m - 1) ++ones;
    }
    const uint64_t bits = b->len ? (uint64_t)(b->len - 1) * 8 + b->used : 0;
    const uint32_t tail = b->pause ? (uint32_t)MS : b->u.d.tail;      /* the closing millisecond of a pause, or the tail of a PZX */
    const size_t table_len = 2 * ((size_t)b->u.d.zero_n + b->u.d.one_n);
    wbuf body;
    memset(&body, 0, sizeof body);
    w32(&body, (uint32_t)bits | (uint32_t)z->level << 31);
    w16(&body, tail);
    w8(&body, b->u.d.zero_n);
    w8(&body, b->u.d.one_n);
    w_bytes(&body, t->arena + b->u.d.table, table_len);
    w_bytes(&body, data, b->len);
    if (!body.fail) pzx_block(z->w, "DATA", body.p, body.n);
    else z->w->fail = 1;
    free(body.p);
    const uint64_t pulses = (bits - ones) * b->u.d.zero_n + ones * b->u.d.one_n + (tail ? 1 : 0);
    z->level ^= (uint8_t)(pulses & 1u);
    *glv ^= (uint8_t)(pulses & 1u);
    if (b->pause) pzx_silence(z, glv, b->pause, tail != 0);
}

static void write_pzx(const zr_tape *t, wbuf *w) {
    static const char TITLE[] = "ZetRo tape";
    static const char HEADER_TAG[4] = {'P', 'Z', 'X', 'T'};
    wbuf head;
    memset(&head, 0, sizeof head);
    w8(&head, 1);
    w8(&head, 0);
    w_bytes(&head, TITLE, sizeof TITLE);
    pzx_block(w, HEADER_TAG, head.p, head.n);
    free(head.p);

    pzx_writer z;
    memset(&z, 0, sizeof z);
    z.w = w;
    zr_tape_flow flow;
    memset(&flow, 0, sizeof flow);
    uint8_t glv = 0;
    uint32_t spin = 0;
    for (unsigned steps = 0; steps < (1u << 20) && !w->fail; ++steps) {
        uint32_t index;
        const int r = zr_tape_flow_step(t, &flow, ZR_FLOW_WALK, &glv, &spin, &index);
        if (r == ZR_FLOW_END) break;
        spin = 0;
        const zr_tape_block *b = &t->blocks[index];
        if (r == ZR_FLOW_STOP || r == ZR_FLOW_STOP48) {
            puls_flush(&z);
            w_bytes(w, "STOP", 4);
            w32(w, 2);
            w16(w, r == ZR_FLOW_STOP48 ? 1 : 0);
            continue;
        }
        if (r == ZR_FLOW_TEXT) {
            if (b->len) {
                puls_flush(&z);
                pzx_block(w, "BRWS", t->arena + b->off, b->len);
            }
            continue;
        }
        switch (b->kind) {
        case ZR_TB_STANDARD: case ZR_TB_TURBO: case ZR_TB_DATA:
            pzx_data(&z, t, b, &glv);
            break;
        case ZR_TB_PAUSE:
            pzx_silence(&z, &glv, b->pause, 0);
            break;
        case ZR_TB_TONE: case ZR_TB_PULSES: case ZR_TB_DIRECT: case ZR_TB_AUDIO: case ZR_TB_GENERAL: {
            pulser p = {pzx_start, pzx_pulse, &z, 0, 0, 0};
            play_block(t, index, &glv, &p);
            break;
        }
        default:
            break;
        }
    }
    puls_flush(&z);
    free(z.puls.p);
}

/* --- the interface ---------------------------------------------------------------------------- */

int zr_tape_write(const zr_tape *t, int format, uint8_t **bytes, size_t *size, unsigned *dropped, char *error, size_t capacity) {
    wbuf w;
    memset(&w, 0, sizeof w);
    unsigned count = 0;
    switch (format) {
    case ZR_TAPE_TAP: write_tap(t, &w, &count); break;
    case ZR_TAPE_TZX: write_tzx(t, &w); break;
    case ZR_TAPE_PZX: write_pzx(t, &w); break;
    default:
        if (capacity) snprintf(error, capacity, "Tapes are written as TAP, TZX or PZX");
        return 0;
    }
    if (dropped) *dropped = count;
    if (w.fail) {
        free(w.p);
        if (capacity) snprintf(error, capacity, "The tape is too large to write");
        return 0;
    }
    if (!w.p) {
        w.p = malloc(1);
        if (!w.p) { if (capacity) snprintf(error, capacity, "Out of memory"); return 0; }
    }
    *bytes = w.p;
    *size = w.n;
    return 1;
}

int zr_tape_save_media(zr_tape *t, zr_media *media, unsigned *dropped, char *error, size_t capacity) {
    int format = ZR_TAPE_NONE;
    if (zr_media_extension_in(media->path, "tap")) format = ZR_TAPE_TAP;
    else if (zr_media_extension_in(media->path, "tzx")) format = ZR_TAPE_TZX;
    else if (zr_media_extension_in(media->path, "pzx")) format = ZR_TAPE_PZX;
    else {
        if (capacity) snprintf(error, capacity, "A tape is written as a .tap, .tzx or .pzx file");
        return 0;
    }
    uint8_t *bytes;
    size_t size;
    if (!zr_tape_write(t, format, &bytes, &size, dropped, error, capacity)) return 0;
    if (!zr_media_resize(media, size)) {
        free(bytes);
        if (capacity) snprintf(error, capacity, "Out of memory");
        return 0;
    }
    if (size) memcpy(media->data, bytes, size);
    free(bytes);
    media->dirty = 1;
    t->modified = 0;
    return 1;
}

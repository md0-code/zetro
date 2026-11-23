/* The Spectrum-compatible core (spectrum.h). ZetRo's own code, written
 * from this project's JET core (machine_core.sv: the memory map, port FE
 * and 1F, the interrupt; rp_spectrum_keyboard.sv: the matrix and the host
 * key combinations) and the 48K BASIC ROM's own keyboard (the keycaps'
 * SYMBOL SHIFT characters). */
#include "spectrum.h"
#include "dandanator.h"
#include "divide.h"
#include "divmmc.h"
#include "media/card.h"
#include "romdb.h"
#include "text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(ZR_BUS_TRACE_IN == ZR_TRACE_IO_READ && ZR_BUS_TRACE_OUT == ZR_TRACE_IO_WRITE &&
               ZR_BUS_TRACE_FETCH == ZR_TRACE_FETCH && ZR_BUS_TRACE_INTERRUPT == ZR_TRACE_INTERRUPT,
               "the bus traces in machine.h's kinds");

#define STATE_VERSION 1u

/* --- the keyboard's tables ------------------------------------------------------------------ */

#define K(name, x, y) {name, name, name, x, y, 4, 4}
const zr_key_desc zr_spectrum_keys[ZR_SPECTRUM_KEYS] = {
    /* row 0, A8: CAPS SHIFT Z X C V */
    {"CAPS SHIFT", "Left Shift", "CAPS SHIFT", 0, 12, 6, 4}, K("Z", 6, 12), K("X", 10, 12), K("C", 14, 12), K("V", 18, 12),
    /* row 1, A9: A S D F G */
    K("A", 3, 8), K("S", 7, 8), K("D", 11, 8), K("F", 15, 8), K("G", 19, 8),
    /* row 2, A10: Q W E R T */
    K("Q", 2, 4), K("W", 6, 4), K("E", 10, 4), K("R", 14, 4), K("T", 18, 4),
    /* row 3, A11: 1 2 3 4 5 */
    K("1", 0, 0), K("2", 4, 0), K("3", 8, 0), K("4", 12, 0), K("5", 16, 0),
    /* row 4, A12: 0 9 8 7 6 */
    K("0", 36, 0), K("9", 32, 0), K("8", 28, 0), K("7", 24, 0), K("6", 20, 0),
    /* row 5, A13: P O I U Y */
    K("P", 38, 4), K("O", 34, 4), K("I", 30, 4), K("U", 26, 4), K("Y", 22, 4),
    /* row 6, A14: ENTER L K J H */
    {"ENTER", "Return", "ENTER", 39, 8, 5, 4}, K("L", 35, 8), K("K", 31, 8), K("J", 27, 8), K("H", 23, 8),
    /* row 7, A15: SPACE SYMBOL SHIFT M N B */
    {"SPACE", "Space", "BREAK SPACE", 38, 12, 6, 4}, {"SYMBOL SHIFT", "Left Ctrl", "SYMBOL SHIFT", 34, 12, 4, 4},
    K("M", 30, 12), K("N", 26, 12), K("B", 22, 12),
};
#undef K

enum { CS = ZR_SPECTRUM_KEY_CAPS_SHIFT, SS = ZR_SPECTRUM_KEY_SYMBOL_SHIFT, N_ = ZR_KEY_NONE };
enum { KZ = 1, KX, KC, KV, KA, KS, KD, KF, KG, KQ, KW, KE, KR, KT, K1, K2, K3, K4, K5, K0, K9, K8, K7, K6,
       KP, KO, KI, KU, KY, KENTER, KL, KK, KJ, KH, KSPACE, KSS, KM, KN, KB };

const zr_typing_desc zr_spectrum_typing[] = {
    {'a', KA, N_}, {'b', KB, N_}, {'c', KC, N_}, {'d', KD, N_}, {'e', KE, N_}, {'f', KF, N_}, {'g', KG, N_},
    {'h', KH, N_}, {'i', KI, N_}, {'j', KJ, N_}, {'k', KK, N_}, {'l', KL, N_}, {'m', KM, N_}, {'n', KN, N_},
    {'o', KO, N_}, {'p', KP, N_}, {'q', KQ, N_}, {'r', KR, N_}, {'s', KS, N_}, {'t', KT, N_}, {'u', KU, N_},
    {'v', KV, N_}, {'w', KW, N_}, {'x', KX, N_}, {'y', KY, N_}, {'z', KZ, N_},
    {'A', KA, CS}, {'B', KB, CS}, {'C', KC, CS}, {'D', KD, CS}, {'E', KE, CS}, {'F', KF, CS}, {'G', KG, CS},
    {'H', KH, CS}, {'I', KI, CS}, {'J', KJ, CS}, {'K', KK, CS}, {'L', KL, CS}, {'M', KM, CS}, {'N', KN, CS},
    {'O', KO, CS}, {'P', KP, CS}, {'Q', KQ, CS}, {'R', KR, CS}, {'S', KS, CS}, {'T', KT, CS}, {'U', KU, CS},
    {'V', KV, CS}, {'W', KW, CS}, {'X', KX, CS}, {'Y', KY, CS}, {'Z', KZ, CS},
    {'0', K0, N_}, {'1', K1, N_}, {'2', K2, N_}, {'3', K3, N_}, {'4', K4, N_},
    {'5', K5, N_}, {'6', K6, N_}, {'7', K7, N_}, {'8', K8, N_}, {'9', K9, N_},
    {' ', KSPACE, N_}, {'\n', KENTER, N_}, {'\b', K0, CS},
    /* SYMBOL SHIFT and a key: the characters on the keycaps (the keywords and <=, >=, <> are not characters). */
    {'!', K1, SS}, {'@', K2, SS}, {'#', K3, SS}, {'$', K4, SS}, {'%', K5, SS}, {'&', K6, SS}, {'\'', K7, SS},
    {'(', K8, SS}, {')', K9, SS}, {'_', K0, SS}, {'<', KR, SS}, {'>', KT, SS}, {';', KO, SS}, {'"', KP, SS},
    {'^', KH, SS}, {'-', KJ, SS}, {'+', KK, SS}, {'=', KL, SS}, {':', KZ, SS}, {'?', KC, SS}, {'/', KV, SS},
    {'*', KB, SS}, {',', KN, SS}, {'.', KM, SS},
};

/* As rp_spectrum_keyboard.sv maps the PC keys: each punctuation key gives its
 * own character, and with the host's Shift held the character a PC keyboard
 * has above it. */
const zr_combo_desc zr_spectrum_combos[] = {
    {"Backspace", K0, CS}, {"Left", K5, CS}, {"Down", K6, CS}, {"Up", K7, CS}, {"Right", K8, CS},
    {"Escape", KSPACE, CS}, {"CapsLock", K2, CS},
    {".", KM, SS}, {",", KN, SS}, {"'", K7, SS}, {";", KO, SS}, {"-", KJ, SS}, {"=", KL, SS}, {"/", KV, SS},
    {"Shift+.", KT, SS}, {"Shift+,", KR, SS}, {"Shift+'", KP, SS}, {"Shift+;", KZ, SS}, {"Shift+-", K0, SS}, {"Shift+=", KK, SS},
    {"Shift+/", KC, SS},
    {"Left Alt", SS, N_},
};

const char *const zr_spectrum_switch[3] = {"off", "on", NULL};
const char *const zr_spectrum_sticks[ZR_SPECTRUM_JOY_KINDS + 1] = {"none", "sinclair1", "sinclair2", "cursor", "fuller", "kempston", NULL};
const char *const zr_spectrum_ays[5] = {"off", "mono", "abc", "acb", NULL};
const int zr_spectrum_ay_labels[4] = {ZR_T_SPEC_AY_OFF, ZR_T_SPEC_AY_MONO, ZR_T_SPEC_AY_ABC, ZR_T_SPEC_AY_ACB};
const char *const zr_spectrum_timings[3] = {"original", "spectrum", NULL};
const int zr_spectrum_timing_labels[2] = {ZR_T_SPEC_TIMING_ORIGINAL, ZR_T_SPEC_TIMING_SPECTRUM};
const char *const zr_spectrum_interfaces[ZR_SPECTRUM_IF_KINDS + 1] = {"none", "dandanator", "divide", "divmmc", NULL};
const int zr_spectrum_interface_labels[ZR_SPECTRUM_IF_KINDS] = {ZR_T_SPEC_IF_NONE, ZR_T_SPEC_IF_DANDANATOR, ZR_T_SPEC_IF_DIVIDE, ZR_T_SPEC_IF_DIVMMC};

void zr_spectrum_interface_config(zr_spectrum_config *c, const unsigned *options, unsigned first_slot) {
    c->interface = (uint8_t)(options[0] < ZR_SPECTRUM_IF_KINDS ? options[0] : ZR_SPECTRUM_IF_NONE);
    c->interface_program = options[1] != 0;
    c->interface_slots = ZR_SPECTRUM_INTERFACE_SLOTS;
    c->interface_slot = first_slot;
}

int zr_spectrum_interface_conflict(const zr_spectrum_config *c, int interface1, char *error, size_t capacity) {
    if (!interface1 || c->interface == ZR_SPECTRUM_IF_NONE) return 0;
    if (capacity)
        snprintf(error, capacity, "Interface 1 and the %s both take the ROM at 0000h: switch one of them off",
                 zr_t(zr_spectrum_interface_labels[c->interface]));
    return 1;
}

void zr_spectrum_default_config(zr_spectrum_config *c) {
    memset(c, 0, sizeof *c);
    c->clock_hz = 14000000u;
    c->ticks_per_tstate = 4;
    zr_ula_default_config(&c->ula);
    c->fe_mask = 0x0001u;
    c->fe_value = 0x0000u;
    c->fe_idle = 0xA0u;
    c->ram_size = 0xC000u;
    c->charset = 0x3D00u;
    c->is_48k = 1;
    c->tape_sound = 1;
    c->tape_traps = c->tape_save_traps = 1;
    c->tape_accelerate = 1;
    c->joystick = ZR_SPECTRUM_JOY_NONE;
    c->tape_slot = 0;
    c->sinclair = ZR_ULA_SINCLAIR_48K;
    c->spectrum_timing = 0;
}

/* --- the board's host: time, deadlines, lines ------------------------------------------------- */

uint64_t zr_spectrum_now(const zr_spectrum *s) {
    return s->sched.in_event ? s->sched.event_time : zr_tclock_time(&s->clock, s->z80.t);
}
static uint64_t host_now(void *ctx) { return zr_spectrum_now(ctx); }
static void host_schedule(void *ctx, void *dev, uint64_t when) {
    zr_spectrum *s = ctx;
    zr_sched_set(&s->sched, dev, when);
}
static void host_line(void *ctx, void *dev, int pin, int state) {
    zr_spectrum *s = ctx;
    if (dev == &s->ula && pin == ZR_ULA_INT) zr_z80_int(&s->z80, state);
}

/* --- the CPU's bus ------------------------------------------------------------------------------ */

static uint8_t cpu_read(void *ctx, uint16_t address, int m1) {
    (void)m1;
    return zr_bus_read(&((zr_spectrum *)ctx)->bus, address);
}
static void cpu_write(void *ctx, uint16_t address, uint8_t value) { zr_bus_write(&((zr_spectrum *)ctx)->bus, address, value); }
static uint8_t cpu_in(void *ctx, uint16_t port) { return zr_bus_in(&((zr_spectrum *)ctx)->bus, port); }
static void cpu_out(void *ctx, uint16_t port, uint8_t value) { zr_bus_out(&((zr_spectrum *)ctx)->bus, port, value); }
/* Nothing drives the data bus in the acknowledge cycle: FFh. */
static uint8_t cpu_ack(void *ctx) {
    zr_spectrum *s = ctx;
    zr_bus_trace(&s->bus, ZR_BUS_TRACE_INTERRUPT, 0xFFu, 0xFFu);
    return 0xFFu;
}

/* --- Spectrum timing ------------------------------------------------------------------------------- */

/* The T-state of the ULA's frame at the CPU's T-state t (the cycle starting now): counted on from the frame's start,
 * found again from the ULA when t has left the frame by more than one (a state loaded, a long bus request). */
static uint32_t frame_tstate(zr_spectrum *s, uint64_t t) {
    const uint32_t length = s->ula.tstates;
    uint64_t d = t - s->frame_t;
    if (d >= length) {
        if (d < 2u * (uint64_t)length) {
            s->frame_t += length;
            d -= length;
        } else {
            d = zr_ula_tstate(&s->ula, zr_tclock_time(&s->clock, t));
            s->frame_t = t - d;
        }
    }
    return (uint32_t)d;
}
static void frame_rebase(zr_spectrum *s) {
    s->frame_t = s->z80.t - zr_ula_tstate(&s->ula, zr_tclock_time(&s->clock, s->z80.t));
}

/* The CPU's cycles on the Sinclair ULA (the comp.sys.sinclair FAQ, "Contended memory" and "Contended I/O"): a memory
 * cycle or an internal T-state with a contended address on the bus waits the delay of its first T-state; a port cycle
 * by the high byte (contended or not) and bit 0 (low: the ULA's port): N:1 C:3, N:4, C:1 C:3, C:1 C:1 C:1 C:1, where
 * C:n waits the delay of the T-state it starts at, then takes n. The T-states added before the cycle. */
static unsigned sinclair_stretch(zr_spectrum *s, int kind, uint16_t address) {
    const int contended = s->contended[address >> 14];
    /* The +2A/+3's gate array contends memory cycles only (MREQ active): a port cycle and an internal T-state wait for nothing. */
    if (s->cfg.gate_array && kind != ZR_Z80_FETCH && kind != ZR_Z80_READ && kind != ZR_Z80_WRITE) return 0u;
    if (kind != ZR_Z80_IN && kind != ZR_Z80_OUT) return contended ? zr_ula_delay(&s->ula, frame_tstate(s, s->z80.t)) : 0u;
    const uint32_t start = frame_tstate(s, s->z80.t);
    uint32_t t = start;
    if (!contended) {
        if (address & 1u) return 0u;
        t += 1u;
        t += zr_ula_delay(&s->ula, t);
        t += 3u;
    } else if (!(address & 1u)) {
        t += zr_ula_delay(&s->ula, t);
        t += 1u;
        t += zr_ula_delay(&s->ula, t);
        t += 3u;
    } else
        for (unsigned i = 0; i < 4u; ++i) {
            t += zr_ula_delay(&s->ula, t);
            t += 1u;
        }
    return t - start - 4u;
}

/* The CPU's cycles lengthened: by Sinclair's contention when Spectrum timing is on, and by the board's own stretch. */
static unsigned cpu_stretch(void *ctx, int kind, uint16_t address) {
    zr_spectrum *s = ctx;
    unsigned n = s->cfg.spectrum_timing ? sinclair_stretch(s, kind, address) : 0u;
    if (s->cfg.stretch) n += s->cfg.stretch(s, kind, address);
    return n;
}

/* Whether any of the board's ports answers a read of `port`. */
static int port_answered(const zr_bus *b, uint16_t port) {
    for (unsigned i = 0; i < b->decoders; ++i) {
        const zr_bus_decoder *d = &b->dec[i];
        if (d->enabled && d->read && (port & d->mask) == d->value) return 1;
    }
    return 0;
}

/* A port read with Sinclair timing: a port no one answers gives what the ULA has on its bus, sampled in the last T-state
 * of the cycle (the Sinclair Wiki, "Floating bus"). */
static uint8_t cpu_in_sinclair(void *ctx, uint16_t port) {
    zr_spectrum *s = ctx;
    if (s->cfg.gate_array || port_answered(&s->bus, port)) return zr_bus_in(&s->bus, port);   /* the gate array has no floating bus */
    const uint8_t value = zr_ula_floating(&s->ula, zr_tclock_time(&s->clock, s->z80.t + 3u));
    zr_bus_trace(&s->bus, ZR_BUS_TRACE_IN, port, value);
    return value;
}

/* The guest time a cycle's write takes effect: with Sinclair timing a memory write lands in the cycle's second T-state
 * (WR is low from there), and the border latch takes port FE at the end of the port cycle, its fourth; the clone's
 * timing keeps the cycle's start. */
static uint64_t write_time(const zr_spectrum *s, unsigned tstates) {
    return s->cfg.spectrum_timing && !s->sched.in_event ? zr_tclock_time(&s->clock, s->z80.t + tstates) : zr_spectrum_now(s);
}

/* The video's pages: the picture is drawn up to the write before it lands. */
static void video_write(void *ctx, uint16_t address, uint8_t value) {
    zr_spectrum *s = ctx;
    (void)value;
    const uint8_t *p = zr_bus_target(&s->bus, address);
    if (p && p >= s->screen && p < s->screen + ZR_SPECTRUM_SCREEN) zr_ula_catch_up(&s->ula, write_time(s, 1u));
}
static uint8_t video_read(void *ctx, uint16_t offset) { return ((const zr_spectrum *)ctx)->screen[offset]; }

/* --- the tape recorder --------------------------------------------------------------------------- */

/* The deadline of the next edge while the tape runs: the EAR sound follows it, and the deck stops at its end. */
static void tape_arm(zr_spectrum *s) {
    uint64_t next = s->tape ? zr_tape_deck_next_edge(&s->deck) : ZR_NEVER;
    const uint64_t now = zr_spectrum_now(s);
    if (next != ZR_NEVER && next <= now) next = now + 1u;
    zr_sched_set(&s->sched, &s->tape_dev, next);
}
static void tape_event(void *dev, uint64_t now) {
    zr_spectrum *s = ((zr_dev *)dev)->host->ctx;
    const int level = zr_tape_deck_level(&s->deck, now);
    if (s->cfg.tape_sound) zr_beeper_set(&s->beeper, ZR_BEEPER_EAR, level, now);
    tape_arm(s);
}
static uint8_t no_read(void *dev, unsigned space, uint32_t offset) { (void)dev; (void)space; (void)offset; return 0xFFu; }
static void no_write(void *dev, unsigned space, uint32_t offset, uint8_t value) { (void)dev; (void)space; (void)offset; (void)value; }
static void no_reset(void *dev) { (void)dev; }
static const zr_dev_ops TAPE_OPS = {.read = no_read, .write = no_write, .event = tape_event, .reset = no_reset};

static int ear(zr_spectrum *s, uint64_t now) { return s->tape ? zr_tape_deck_level(&s->deck, now) : 0; }

/* --- the ROM tape traps' view of the recorder (hw/traps.h) ---------------------------------------- */

static uint8_t traps_read(void *ctx, uint16_t address) { return zr_bus_peek(&((zr_spectrum *)ctx)->bus, address); }
static void traps_write(void *ctx, uint16_t address, uint8_t value) { zr_bus_write(&((zr_spectrum *)ctx)->bus, address, value); }
/* SPACE, bit 0 of the row A15 selects: the key the ROM's tape routines test for BREAK. */
static int traps_break(void *ctx) { return !(zr_ula_keys(&((zr_spectrum *)ctx)->ula, 0x7Fu) & 1u); }

/* The recorder moved by a trap: its next edge, and the level the tape's sound follows. */
static void tape_moved(zr_spectrum *s) {
    const uint64_t now = zr_spectrum_now(s);
    if (s->cfg.tape_sound) zr_beeper_set(&s->beeper, ZR_BEEPER_EAR, ear(s, now), now);
    tape_arm(s);
}

/* Blocks that make no sound and take no time. */
static int silent_block(int kind) { return kind == ZR_TB_TEXT || kind == ZR_TB_INFO || kind == ZR_TB_GROUP || kind == ZR_TB_GROUP_END; }

/* The block the ROM's loader would read next: the one at the recorder's position, past blocks that make no sound; a block
 * the tape is part-way into is passed over (the loader waits for the next leader tone) unless the tape is playing in the
 * leader tone of a standard block (8063 pulses of 2168 T-states before a header, 3223 before data), which the loader would
 * lock on to. A stopped recorder counts: it is where the tape will start. */
static int traps_block_next(void *ctx, const uint8_t **bytes, size_t *length) {
    zr_spectrum *s = ctx;
    if (!s->tape || zr_tape_deck_recording(&s->deck)) return 0;
    zr_tape_deck_advance(&s->deck, zr_spectrum_now(s));
    const uint32_t count = zr_tape_count(s->tape);
    uint32_t i = zr_tape_deck_block(&s->deck);
    if (zr_tape_deck_position(&s->deck)) {
        unsigned flag = 0;
        const uint8_t *data;
        size_t size;
        const int standard = i < count && zr_tape_block_data(s->tape, i, &data, &size, &flag, NULL);
        const uint64_t leader = (uint64_t)(flag < 128u ? 8063u : 3223u) * 2168u;
        if (!(standard && zr_tape_deck_playing(&s->deck) && zr_tape_deck_position(&s->deck) < leader)) ++i;
    }
    while (i < count && silent_block(zr_tape_block_kind(s->tape, i))) ++i;
    if (i >= count || !zr_tape_block_data(s->tape, i, bytes, length, NULL, NULL)) return 0;
    s->trap_block = i;
    return 1;
}

/* The recorder is left at the start of the block after the one taken (running, if it was). */
static void traps_block_done(void *ctx) {
    zr_spectrum *s = ctx;
    zr_tape_deck_seek(&s->deck, s->trap_block + 1u, zr_spectrum_now(s));
    tape_moved(s);
}

/* The block a save trap puts on the tape: onto the tape in the slot when its file can be written back as a TAP, TZX or PZX,
 * and into the file's bytes at once (the shell writes the file); the recorder is left at the end of the tape. */
static int traps_block_put(void *ctx, const uint8_t *bytes, size_t length) {
    zr_spectrum *s = ctx;
    zr_media *media = s->tape_media;
    if (!s->tape || !media || !media->writable || !zr_media_extension_in(media->path, "tap,tzx,pzx") || length > 0xFFFFu) return 0;
    if (!zr_tape_append_standard(s->tape, bytes, length, 1000u)) return 0;
    char why[128];
    if (!zr_tape_save_media(s->tape, media, NULL, why, sizeof why)) snprintf(s->error, sizeof s->error, "%s", why);
    zr_tape_deck_seek(&s->deck, zr_tape_count(s->tape), zr_spectrum_now(s));
    tape_moved(s);
    return 1;
}

/* --- loader acceleration's view of the board (hw/loadaccel.h) -------------------------------------- */

/* With the option on, the CPU's bus goes through these: a read of port FE is a sample of the tape input, every other
 * cycle that leaves the CPU (a write, another port, an acknowledge) tells the recogniser that more than a loop ran; a
 * write, or a port other than FE (a board's paging), may also have changed the code it analysed. */
static void cpu_write_accel(void *ctx, uint16_t address, uint8_t value) {
    zr_loadaccel_write(&((zr_spectrum *)ctx)->accel, address);
    cpu_write(ctx, address, value);
}
static void cpu_out_accel(void *ctx, uint16_t port, uint8_t value) {
    zr_spectrum *s = ctx;
    if ((port & s->cfg.fe_mask) == s->cfg.fe_value) zr_loadaccel_disturb(&s->accel);
    else zr_loadaccel_changed(&s->accel);
    cpu_out(ctx, port, value);
}
static uint8_t cpu_ack_accel(void *ctx) {
    zr_loadaccel_disturb(&((zr_spectrum *)ctx)->accel);
    return cpu_ack(ctx);
}
static uint8_t cpu_in_accel(void *ctx, uint16_t port) {
    zr_spectrum *s = ctx;
    if ((port & s->cfg.fe_mask) == s->cfg.fe_value) zr_loadaccel_sample(&s->accel);
    else zr_loadaccel_disturb(&s->accel);
    return s->cfg.spectrum_timing ? cpu_in_sinclair(ctx, port) : cpu_in(ctx, port);
}

/* Plain memory: a page the bus reads directly (ROM or RAM), which nothing but a write changes. */
static int accel_peek(void *ctx, uint16_t address, uint8_t *value) {
    const zr_spectrum *s = ctx;
    const uint8_t *page = s->bus.rd[address >> ZR_BUS_PAGE_BITS];
    if (!page) return 0;
    *value = page[address & (ZR_BUS_PAGE_SIZE - 1u)];
    return 1;
}
/* The EAR level changes only at the recorder's edges, each the deadline of tape_dev (tape_arm). */
static uint64_t accel_level_until(void *ctx) {
    zr_spectrum *s = ctx;
    return s->tape ? zr_sched_get(&s->sched, &s->tape_dev) : ZR_NEVER;
}
/* Spectrum timing: a cycle is lengthened only at a T-state of the frame whose contention delay is not 0 (cpu_stretch
 * looks the delay up at T-states inside the cycle). */
static uint64_t accel_quiet(void *ctx, uint64_t t, uint64_t limit) {
    const zr_spectrum *s = ctx;
    if (t >= limit) return limit;
    uint32_t at = zr_ula_tstate(&s->ula, zr_tclock_time(&s->clock, t));
    for (; t < limit; ++t) {
        if (s->ula.contention[at]) return t;
        if (++at >= s->ula.tstates) at = 0;
    }
    return limit;
}
/* The quiet stretch for loader acceleration: Spectrum timing's and the board's. */
static uint64_t accel_quiet_all(void *ctx, uint64_t t, uint64_t limit) {
    zr_spectrum *s = ctx;
    if (s->cfg.spectrum_timing) limit = accel_quiet(ctx, t, limit);
    if (s->cfg.stretch) limit = s->cfg.stretch_quiet ? s->cfg.stretch_quiet(s, t, limit) : (t < limit ? t : limit);
    return limit;
}

/* --- the ports ---------------------------------------------------------------------------------- */

/* The keys a joystick of the Sinclair or Cursor kind holds down: bit c of row r (the matrix's rows and columns). */
static void stick_keys(unsigned kind, unsigned held, uint8_t rows[8]) {
    /* left, right, down, up, fire as row * 8 + column */
    static const uint8_t KEYS[3][5] = {
        {4 * 8 + 4, 4 * 8 + 3, 4 * 8 + 2, 4 * 8 + 1, 4 * 8 + 0},     /* Sinclair I: 6 7 8 9 0 */
        {3 * 8 + 0, 3 * 8 + 1, 3 * 8 + 2, 3 * 8 + 3, 3 * 8 + 4},     /* Sinclair II: 1 2 3 4 5 */
        {3 * 8 + 4, 4 * 8 + 2, 4 * 8 + 4, 4 * 8 + 3, 4 * 8 + 0},     /* Cursor: 5 8 6 7 0 */
    };
    static const unsigned BITS[5] = {ZR_JOY_LEFT, ZR_JOY_RIGHT, ZR_JOY_DOWN, ZR_JOY_UP, ZR_JOY_FIRE};
    if (kind < ZR_SPECTRUM_JOY_SINCLAIR1 || kind > ZR_SPECTRUM_JOY_CURSOR) return;
    const uint8_t *keys = KEYS[kind - ZR_SPECTRUM_JOY_SINCLAIR1];
    for (unsigned i = 0; i < 5u; ++i)
        if (held & BITS[i]) rows[keys[i] >> 3] |= (uint8_t)(1u << (keys[i] & 7u));
}

static uint8_t fe_read(void *ctx, uint16_t port) {
    zr_spectrum *s = ctx;
    const uint8_t high = (uint8_t)(port >> 8);
    uint8_t keys = zr_ula_keys(&s->ula, high);
    if (s->stick | s->stick2) {
        uint8_t rows[8] = {0};
        stick_keys(s->cfg.joystick, s->stick, rows);
        stick_keys(s->cfg.joystick2, s->stick2, rows);
        for (unsigned r = 0; r < 8u; ++r)
            if (!(high >> r & 1u)) keys = (uint8_t)(keys & ~rows[r]);
    }
    return (uint8_t)(s->cfg.fe_idle | (ear(s, zr_spectrum_now(s)) ? 0x40u : 0u) | keys);
}

/* Bits 0-2 the border, 3 MIC, 4 the speaker. */
static void fe_apply(zr_spectrum *s, uint8_t value, uint64_t now) {
    zr_ula_catch_up(&s->ula, now);
    zr_ula_set_border(&s->ula, value & 7u);
    zr_beeper_set(&s->beeper, ZR_BEEPER_SPEAKER, value >> 4 & 1u, now);
    zr_beeper_set(&s->beeper, ZR_BEEPER_MIC, value >> 3 & 1u, now);
    if (s->tape && zr_tape_deck_recording(&s->deck)) zr_tape_deck_output(&s->deck, value >> 3 & 1u, now);
    s->fe = (uint8_t)(value & 0x1Fu);
}
static void fe_write(void *ctx, uint16_t port, uint8_t value) {
    zr_spectrum *s = ctx;
    (void)port;
    if (s->cfg.spectrum_timing) zr_ula_catch_up(&s->ula, write_time(s, 4u));
    fe_apply(s, value, zr_spectrum_now(s));
}

/* What the sockets of a kind hold together. */
static unsigned held_of(const zr_spectrum *s, unsigned kind) {
    return (s->cfg.joystick == kind ? s->stick : 0u) | (s->cfg.joystick2 == kind ? s->stick2 : 0u);
}
/* Kempston: bit 0 right, 1 left, 2 down, 3 up, 4 fire, high when held. */
static uint8_t kempston_read(void *ctx, uint16_t port) {
    const zr_spectrum *s = ctx;
    (void)port;
    const unsigned h = held_of(s, ZR_SPECTRUM_JOY_KEMPSTON);
    return (uint8_t)((h & ZR_JOY_RIGHT ? 1u : 0u) | (h & ZR_JOY_LEFT ? 2u : 0u) | (h & ZR_JOY_DOWN ? 4u : 0u) |
                     (h & ZR_JOY_UP ? 8u : 0u) | (h & ZR_JOY_FIRE ? 16u : 0u));
}
/* Fuller: bit 0 up, 1 down, 2 left, 3 right, 7 fire, low when held. */
static uint8_t fuller_read(void *ctx, uint16_t port) {
    const zr_spectrum *s = ctx;
    (void)port;
    const unsigned h = held_of(s, ZR_SPECTRUM_JOY_FULLER);
    return (uint8_t)~((h & ZR_JOY_UP ? 1u : 0u) | (h & ZR_JOY_DOWN ? 2u : 0u) | (h & ZR_JOY_LEFT ? 4u : 0u) |
                      (h & ZR_JOY_RIGHT ? 8u : 0u) | (h & ZR_JOY_FIRE ? 0x80u : 0u));
}

/* The AY board: FFFD latches the register's address and reads it, BFFD writes it. */
static uint8_t ay_read(void *ctx, uint16_t port) {
    (void)port;
    return zr_ay_read(&((zr_spectrum *)ctx)->ay);
}
static void ay_address(void *ctx, uint16_t port, uint8_t value) {
    (void)port;
    zr_ay_select(&((zr_spectrum *)ctx)->ay, value);
}
static void ay_data(void *ctx, uint16_t port, uint8_t value) {
    (void)port;
    zr_ay_write(&((zr_spectrum *)ctx)->ay, value);
}

void zr_spectrum_ay_decode(zr_spectrum *s, uint16_t mask, uint16_t value) {
    if (!s->cfg.ay) return;
    const uint16_t m = mask ? mask : 0x8002u, v = mask ? value : 0x8000u;
    for (unsigned i = 0; i < 2u; ++i) {
        if (s->ay_dec[i] < 0) continue;
        zr_bus_decoder *d = &s->bus.dec[s->ay_dec[i]];
        d->mask = (uint16_t)(m | 0x4000u);
        d->value = (uint16_t)((i ? v & ~0x4000u : v | 0x4000u) & d->mask);
    }
}

/* --- the CPU's hooks: the tape traps' and a board's ------------------------------------------- */

/* The tables in the order they run: the interface's, the board's, the tape traps'. */
#define HOOK_TABLES 3u
static const zr_cpu_hooks *hook_table(const zr_spectrum *s, unsigned i) {
    return i == 0 ? s->iface_hooks : i == 1 ? s->board_hooks : zr_traps_hooks(&s->traps);
}
static int chain_before(void *ctx, uint16_t pc) {
    zr_spectrum *s = ctx;
    for (unsigned i = 0; i < HOOK_TABLES; ++i) {
        const zr_cpu_hooks *h = hook_table(s, i);
        if (zr_cpu_watched(h, pc) && h->before_fetch && h->before_fetch(h->ctx, pc)) return 1;
    }
    return 0;
}
static void chain_after(void *ctx, uint16_t pc, uint8_t opcode) {
    zr_spectrum *s = ctx;
    for (unsigned i = 0; i < HOOK_TABLES; ++i) {
        const zr_cpu_hooks *h = hook_table(s, i);
        if (zr_cpu_watched(h, pc) && h->after_fetch) h->after_fetch(h->ctx, pc, opcode);
    }
}
static void chain_refresh(void *ctx, uint16_t address) {
    zr_spectrum *s = ctx;
    for (unsigned i = 0; i < HOOK_TABLES; ++i) {
        const zr_cpu_hooks *h = hook_table(s, i);
        if (h && h->refresh) h->refresh(h->ctx, address);
    }
}

/* One table goes to the CPU as it is; several through the chain, watching every address one of them watches. */
static void install_hooks(zr_spectrum *s) {
    const zr_cpu_hooks *only = NULL;
    unsigned count = 0, refresh = 0;
    for (unsigned i = 0; i < HOOK_TABLES; ++i) {
        const zr_cpu_hooks *h = hook_table(s, i);
        if (!h) continue;
        only = h;
        ++count;
        refresh |= h->refresh != NULL;
    }
    if (count < 2u) {
        s->z80.hooks = only;
        return;
    }
    memset(s->watch, 0, sizeof s->watch);
    for (unsigned i = 0; i < HOOK_TABLES; ++i) {
        const zr_cpu_hooks *h = hook_table(s, i);
        if (h && h->watch)
            for (unsigned a = 0; a < sizeof s->watch; ++a) s->watch[a] |= h->watch[a];
    }
    s->hooks.ctx = s;
    s->hooks.watch = s->watch;
    s->hooks.before_fetch = chain_before;
    s->hooks.after_fetch = chain_after;
    s->hooks.refresh = refresh ? chain_refresh : NULL;
    s->z80.hooks = &s->hooks;
}

void zr_spectrum_set_hooks(zr_spectrum *s, const zr_cpu_hooks *board) {
    s->board_hooks = board;
    install_hooks(s);
}
void zr_spectrum_set_interface_hooks(zr_spectrum *s, const zr_cpu_hooks *hooks) {
    s->iface_hooks = hooks;
    install_hooks(s);
}

void zr_spectrum_traps_rescan(zr_spectrum *s, const uint8_t *memory, size_t size) {
    const zr_traps_host host = s->traps.host;
    zr_traps_init_loose(&s->traps, &s->z80, &host, memory, size);
    zr_traps_enable(&s->traps, s->cfg.tape_traps, s->cfg.tape_save_traps);
    install_hooks(s);
}

/* The +2A/+3's gate array over the 128K's raster (comp.sys.sinclair FAQ, 128K reference): the first contended T-state is 14365
 * (the 128K's is 14361), the picture's first pixel 4 T-states later with it; the delays are 1, 0, 7, 6, 5, 4, 3, 2 repeated
 * for the 128 T-states of each of the 192 picture lines (the ULA's table is made first, then replaced). */
#define GATE_ARRAY_FIRST 14365u
static void gate_array_config(zr_ula_config *c) {
    const uint32_t per_tstate = c->ticks_per_tstate / c->ticks_per_pixel;
    c->contention_start = GATE_ARRAY_FIRST;
    const uint32_t picture = (GATE_ARRAY_FIRST + 7u) * per_tstate;
    c->picture_x = picture % c->pixels_per_line;
    c->picture_y = picture / c->pixels_per_line;
    c->start_pixel = (c->picture_y + 240u) * c->pixels_per_line;
}
static void gate_array_contention(zr_ula *u) {
    static const uint8_t PATTERN[8] = {1, 0, 7, 6, 5, 4, 3, 2};
    memset(u->contention, 0, sizeof u->contention);
    for (uint32_t line = 0; line < 192u; ++line)
        for (uint32_t x = 0; x < 128u; ++x) {
            const uint32_t t = u->cfg.contention_start + line * u->tstates_per_line + x;
            if (t < u->tstates && t < ZR_ULA_SINCLAIR_TSTATES) u->contention[t] = PATTERN[x & 7u];
        }
}

/* --- life cycle ------------------------------------------------------------------------------------ */

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
static int fail(char *error, size_t capacity, const char *message, const char *what) {
    if (capacity) snprintf(error, capacity, message, what);
    return 0;
}

/* --- the added interface: the Dandanator ------------------------------------------------------------------- */

int zr_spectrum_interface_rom(const zr_spectrum *s) {
    if (s->div_mem) return zr_divmem_mapped(s->div_mem);
    return s->dan && s->dan->set && s->dan->mapped < ZR_DANDANATOR_HIDDEN;
}

/* The cartridge's reads: its pages over the board's ROM as it says, or its status reads. */
static uint8_t dan_read(void *ctx, uint16_t address) { return zr_dandanator_read(((zr_spectrum *)ctx)->dan, address); }
static void dan_map(void *ctx) {
    zr_spectrum *s = ctx;
    const zr_bus_handler status = {.read = dan_read, .ctx = s};
    for (unsigned p = 0; p < 16u; ++p) {
        const uint8_t *bytes;
        const int how = zr_dandanator_page(s->dan, p, &bytes);
        zr_bus_overlay_read(&s->bus, (uint16_t)(p * ZR_BUS_PAGE_SIZE), ZR_BUS_PAGE_SIZE, how == ZR_DANDANATOR_DIRECT ? bytes : NULL,
                            how == ZR_DANDANATOR_STATUS ? &status : NULL);
    }
    if (s->cfg.tape_accelerate) zr_loadaccel_changed(&s->accel);
}
static void dan_write(void *ctx, uint16_t address, uint8_t value) {
    zr_spectrum *s = ctx;
    zr_dandanator_write(s->dan, address, value, zr_spectrum_now(s));
}
/* The cartridge pulls the edge connector's RESET: the board resets as by its own button, the cartridge does not. */
static void dan_reset(void *ctx) {
    zr_spectrum *s = ctx;
    s->iface_resetting = 1;
    s->family->impl->reset(s, 0);
    s->iface_resetting = 0;
}
static void dan_nmi(void *ctx) { zr_z80_nmi(&((zr_spectrum *)ctx)->z80); }
static void dan_changed(void *ctx) {
    zr_spectrum *s = ctx;
    if (s->iface_media[0]) s->iface_media[0]->dirty = 1;
}
static int dan_writable(void *ctx) {
    const zr_spectrum *s = ctx;
    return s->iface_media[0] && s->iface_media[0]->writable;
}

/* --- the added interfaces: the DivIDE and the DivMMC ------------------------------------------------------- */

/* The interface's memory over the board's ROM as it says; what it shows is the overlay's read side (the RAM stores its own
 * writes through the overlay's tap). */
static void div_map(void *ctx) {
    zr_spectrum *s = ctx;
    for (unsigned p = 0; p < 16u; ++p) {
        const uint8_t *bytes = NULL;
        if (!zr_divmem_page(s->div_mem, p, &bytes)) bytes = NULL;
        zr_bus_overlay_read(&s->bus, (uint16_t)(p * ZR_BUS_PAGE_SIZE), ZR_BUS_PAGE_SIZE, bytes, NULL);
    }
    if (s->cfg.tape_accelerate) zr_loadaccel_changed(&s->accel);
}

/* The automatic mapping acts only where the board's own ROM is at 0000h: not over RAM the board maps there, and, on a board that
 * pages its ROM, only while the 48 BASIC ROM is the page at 0000h (cfg.interface_rom). */
static int div_automap_ok(void *ctx) {
    zr_spectrum *s = ctx;
    for (unsigned p = 0; p < 8u; ++p)
        if (s->bus.ov->low_rd[p] && s->bus.ov->low_rd[p] == s->bus.wr[p]) return 0;
    return !s->cfg.interface_rom || s->cfg.interface_rom(s);
}
static void div_nmi(void *ctx) { zr_z80_nmi(&((zr_spectrum *)ctx)->z80); }
static void div_write(void *ctx, uint16_t address, uint8_t value) { zr_divmem_write(((zr_spectrum *)ctx)->div_mem, address, value); }

static uint8_t divide_in(void *ctx, uint16_t port) { return zr_divide_in(((zr_spectrum *)ctx)->divide, port); }
static void divide_out(void *ctx, uint16_t port, uint8_t value) { zr_divide_out(((zr_spectrum *)ctx)->divide, port, value); }
static uint8_t divmmc_in(void *ctx, uint16_t port) { return zr_divmmc_in(((zr_spectrum *)ctx)->divmmc, port); }
static void divmmc_out(void *ctx, uint16_t port, uint8_t value) { zr_divmmc_out(((zr_spectrum *)ctx)->divmmc, port, value); }

static int div_create(zr_spectrum *s, const zr_machine_setup *setup, char *error, size_t capacity) {
    const int mmc = s->cfg.interface == ZR_SPECTRUM_IF_DIVMMC;
    const zr_known_rom *rom = zr_romdb_find(mmc ? "esxdos-mmc" : "esxdos-ide");
    const zr_rom_file *file = rom && setup->roms ? zr_rom_index_find(setup->roms, rom) : NULL;
    if (!rom || !file || !file->data || file->size != rom->size) {
        if (capacity)
            snprintf(error, capacity, "The ESXDOS firmware %s for the %s is missing: python tools/fetch_roms.py fetches it into roms/",
                     rom ? rom->file : "?", zr_t(zr_spectrum_interface_labels[s->cfg.interface]));
        return 0;
    }
    if (!(s->iface_firmware = malloc(rom->size)) || !(s->iface_overlay = malloc(sizeof *s->iface_overlay)) ||
        !(mmc ? (void *)(s->divmmc = calloc(1, sizeof *s->divmmc)) : (void *)(s->divide = calloc(1, sizeof *s->divide))))
        return fail(error, capacity, "%s", "Out of memory");
    memcpy(s->iface_firmware, file->data, rom->size);
    zr_bus_overlay_attach(&s->bus, s->iface_overlay);
    const zr_divmem_board board = {.ctx = s, .map = div_map, .automap_ok = div_automap_ok, .nmi = div_nmi};
    if (mmc) {
        zr_divmmc_init(s->divmmc, &s->host, &board, s->iface_firmware);
        s->div_mem = &s->divmmc->mem;
        zr_bus_add_io(&s->bus, ZR_DIVMMC_MASK, ZR_DIVMMC_CONTROL, NULL, divmmc_out, s);
        zr_bus_add_io(&s->bus, ZR_DIVMMC_MASK, ZR_DIVMMC_SELECT, NULL, divmmc_out, s);
        zr_bus_add_io(&s->bus, ZR_DIVMMC_MASK, ZR_DIVMMC_SPI, divmmc_in, divmmc_out, s);
    } else {
        zr_divide_init(s->divide, &s->host, &board, s->iface_firmware, s->cfg.clock_hz);
        s->div_mem = &s->divide->mem;
        zr_bus_add_io(&s->bus, ZR_DIVIDE_IDE_MASK, ZR_DIVIDE_IDE_VALUE, divide_in, divide_out, s);
        zr_bus_add_io(&s->bus, ZR_DIVIDE_CONTROL_MASK, ZR_DIVIDE_CONTROL_VALUE, NULL, divide_out, s);
    }
    const zr_bus_handler tap = {.write = div_write, .ctx = s};
    zr_bus_overlay_watch(&s->bus, 0x0000u, 0x4000u, &tap);
    zr_spectrum_set_interface_hooks(s, zr_divmem_hooks(s->div_mem));
    const size_t n = strlen(s->description);
    snprintf(s->description + n, sizeof s->description - n, ", %s", zr_t(zr_spectrum_interface_labels[s->cfg.interface]));
    return 1;
}

static int interface_create(zr_spectrum *s, const zr_machine_setup *setup, char *error, size_t capacity) {
    if (s->cfg.interface == ZR_SPECTRUM_IF_DIVIDE || s->cfg.interface == ZR_SPECTRUM_IF_DIVMMC) return div_create(s, setup, error, capacity);
    if (s->cfg.interface != ZR_SPECTRUM_IF_DANDANATOR) return 1;
    if (!(s->dan = calloc(1, sizeof *s->dan)) || !(s->iface_overlay = malloc(sizeof *s->iface_overlay)))
        return fail(error, capacity, "%s", "Out of memory");
    zr_bus_overlay_attach(&s->bus, s->iface_overlay);
    const zr_dandanator_board board = {.ctx = s, .map = dan_map, .reset = dan_reset, .nmi = dan_nmi, .changed = dan_changed,
                                       .writable = dan_writable};
    zr_dandanator_init(s->dan, &s->host, &board, s->cfg.clock_hz, s->cfg.interface_program);
    const zr_bus_handler tap = {.write = dan_write, .ctx = s};
    zr_bus_overlay_watch(&s->bus, 0x0000u, 0x4000u, &tap);
    const size_t n = strlen(s->description);
    snprintf(s->description + n, sizeof s->description - n, ", %s", zr_t(ZR_T_SPEC_IF_DANDANATOR));
    return 1;
}

/* A file in the interface's slot: the ROM set, checked by its content (an empty file becomes a blank set, written back as
 * 512 KiB of FFh). The cartridge goes in at once (with the interface on: the machine then resets, as when a cartridge is
 * plugged in); with the interface off the file is only held. */
static int card_insert(zr_spectrum *s, unsigned index, zr_media *media, char *error, size_t capacity) {
    if (s->iface_media[index]) return fail(error, capacity, "%s", "The slot is not empty");
    /* The image is used in place: the media only names it (the shell reads nothing of a card). */
    zr_card *card = zr_card_open(media->path, !media->writable, error, capacity);
    if (!card) return 0;
    if (s->divide) zr_divide_attach(s->divide, index - 1u, card);
    else if (s->divmmc && index == 1u && !zr_divmmc_attach(s->divmmc, card, error, capacity)) {
        zr_card_close(card);
        return 0;
    }
    s->card[index] = card;
    s->iface_media[index] = media;
    return 1;
}

static int card_eject(zr_spectrum *s, unsigned index) {
    if (!s->iface_media[index]) return 0;
    if (s->divide) zr_divide_attach(s->divide, index - 1u, NULL);
    else if (s->divmmc && index == 1u) zr_divmmc_attach(s->divmmc, NULL, NULL, 0);
    zr_card_flush(s->card[index]);
    zr_card_close(s->card[index]);
    s->card[index] = NULL;
    s->iface_media[index] = NULL;
    return 1;
}

static int interface_insert(zr_spectrum *s, unsigned index, zr_media *media, char *error, size_t capacity) {
    if (index) return card_insert(s, index, media, error, capacity);
    if (s->iface_media[index]) return fail(error, capacity, "%s", "The slot is not empty");
    if (!media->size) {
        if (!zr_media_resize(media, ZR_DANDANATOR_SIZE)) return fail(error, capacity, "%s", "Out of memory");
        memset(media->data, 0xFF, ZR_DANDANATOR_SIZE);
        media->dirty = 1;
    }
    if (zr_dandanator_detect(media->data, media->size) == ZR_DANDANATOR_NOT_A_SET)
        return fail(error, capacity, "%s", "Not a Dandanator ROM set (512 KiB with the menu's version at 3FE0h, or all FFh)");
    s->iface_media[index] = media;
    if (s->dan) {
        zr_dandanator_insert(s->dan, media->data);
        s->family->impl->reset(s, 0);
    }
    return 1;
}

static int interface_eject(zr_spectrum *s, unsigned index) {
    if (index) return card_eject(s, index);
    if (!s->iface_media[index]) return 0;
    s->iface_media[index] = NULL;
    if (s->dan) {
        zr_dandanator_insert(s->dan, NULL);
        s->family->impl->reset(s, 0);
    }
    return 1;
}

/* The slot's index among the interface's, or -1 when it is not one of them. */
static int interface_index(const zr_spectrum *s, unsigned slot) {
    if (!s->cfg.interface_slots || slot < s->cfg.interface_slot || slot >= s->cfg.interface_slot + ZR_SPECTRUM_INTERFACE_SLOTS) return -1;
    return (int)(slot - s->cfg.interface_slot);
}

/* The model's ROMs from the index, one after the other (the board's own list when it gives one, some of its files perhaps missing). */
static int load_roms(zr_spectrum *s, const zr_machine_setup *setup, char *error, size_t capacity) {
    const char *const *keys = s->cfg.rom_keys ? s->cfg.rom_keys : setup->family->models[setup->model].roms;
    uint32_t total = 0;
    for (unsigned i = 0; keys && keys[i]; ++i) {
        const zr_known_rom *rom = zr_romdb_find(keys[i]);
        if (!rom || i == ZR_SPECTRUM_ROMS) return fail(error, capacity, "The ROM %s cannot be used", keys[i]);
        total += rom->size;
    }
    if (total < 0x4000u) return fail(error, capacity, "%s has no 16 KiB ROM", setup->family->models[setup->model].name);
    if (!(s->rom = malloc(total))) return fail(error, capacity, "%s", "Out of memory");
    s->rom_size = total;
    uint32_t at = 0;
    for (unsigned i = 0; keys[i]; ++i) {
        const zr_known_rom *rom = zr_romdb_find(keys[i]);
        const zr_rom_file *file = setup->roms ? zr_rom_index_find(setup->roms, rom) : NULL;
        const int usable = file && file->data && file->size == rom->size;
        if (usable) memcpy(s->rom + at, file->data, rom->size);
        else if (s->cfg.rom_optional >> i & 1u) memset(s->rom + at, 0xFF, rom->size);
        else return fail(error, capacity, "The ROM %s is missing", rom->file);
        zr_sha256(s->rom + at, rom->size, s->rom_hash[i]);
        at += rom->size;
        s->rom_count = i + 1u;
    }
    return 1;
}

/* The character set's glyphs as words, for the text read-back. */
static void read_glyphs(zr_spectrum *s) {
    for (unsigned g = 0; g < 96u; ++g) {
        uint64_t word = 0;
        for (unsigned y = 0; y < 8u; ++y) {
            const uint32_t at = (uint32_t)s->cfg.charset + g * 8u + y;
            word = word << 8 | (at < s->rom_size ? s->rom[at] : 0u);
        }
        s->glyph[g] = word;
    }
}

zr_spectrum *zr_spectrum_create(const zr_spectrum_config *cfg, const zr_machine_setup *setup, size_t size,
                                char *error, size_t capacity) {
    if (size < sizeof(zr_spectrum)) size = sizeof(zr_spectrum);
    zr_spectrum *s = calloc(1, size);
    if (!s) {
        fail(error, capacity, "%s", "Out of memory");
        return NULL;
    }
    s->cfg = *cfg;
    s->family = setup->family;
    s->model = setup->model;
    if (s->cfg.ram_size < 0xC000u) s->cfg.ram_size = 0xC000u;
    if (!s->cfg.ticks_per_tstate) s->cfg.ticks_per_tstate = 1;
    /* Spectrum timing: the Sinclair ULA's raster and contention, 4000h-7FFFh contended (page 5 on a 128K, always there),
     * bits 5 and 7 of port FE high (the FAQ's 48K reference: "always one"). */
    if (s->cfg.spectrum_timing) {
        if (s->cfg.sinclair != ZR_ULA_SINCLAIR_128K) s->cfg.sinclair = ZR_ULA_SINCLAIR_48K;
        s->cfg.ula.ticks_per_tstate = s->cfg.ticks_per_tstate;
        zr_ula_sinclair_config(&s->cfg.ula, s->cfg.sinclair);
        if (s->cfg.gate_array) gate_array_config(&s->cfg.ula);
        s->cfg.fe_idle = 0xA0u;
        s->contended[1] = 1;
    }
    if (!load_roms(s, setup, error, capacity)) {
        zr_spectrum_destroy(s);
        return NULL;
    }
    if (!(s->ram = calloc(1, s->cfg.ram_size))) {
        fail(error, capacity, "%s", "Out of memory");
        zr_spectrum_destroy(s);
        return NULL;
    }
    snprintf(s->description, sizeof s->description, "%s", setup->family->models[setup->model].name);
    read_glyphs(s);
    /* The board: host, scheduler, clock. */
    s->host.ctx = s;
    s->host.now = host_now;
    s->host.schedule = host_schedule;
    s->host.line = host_line;
    zr_sched_init(&s->sched);
    zr_tclock_init(&s->clock, s->cfg.ticks_per_tstate, 0, 0);
    /* Memory: the ROM at 0000h, 48 KiB of RAM at 4000h, the screen at its start, its pages watched. */
    zr_bus_init(&s->bus);
    zr_bus_map_rom(&s->bus, 0x0000u, 0x4000u, s->rom);
    zr_bus_map_ram(&s->bus, 0x4000u, 0xC000u, s->ram);
    s->screen = s->ram;
    zr_bus_set_hook(&s->bus, video_write, s);
    zr_bus_watch(&s->bus, 0x4000u, 0x1C00u, 1);
    /* Ports. */
    zr_bus_add_io(&s->bus, s->cfg.fe_mask, s->cfg.fe_value, fe_read, fe_write, s);
    const int kempston = s->cfg.joystick == ZR_SPECTRUM_JOY_KEMPSTON || s->cfg.joystick2 == ZR_SPECTRUM_JOY_KEMPSTON;
    if (s->cfg.kempston_mask) zr_bus_add_io(&s->bus, s->cfg.kempston_mask, s->cfg.kempston_value, kempston_read, NULL, s);
    else if (kempston) zr_bus_add_io(&s->bus, 0x00FFu, 0x001Fu, kempston_read, NULL, s);
    if (s->cfg.joystick == ZR_SPECTRUM_JOY_FULLER || s->cfg.joystick2 == ZR_SPECTRUM_JOY_FULLER) {
        const uint16_t mask = s->cfg.fuller_mask ? s->cfg.fuller_mask : 0x00FFu;
        zr_bus_add_io(&s->bus, mask, s->cfg.fuller_mask ? s->cfg.fuller_value : 0x007Fu, fuller_read, NULL, s);
    }
    if (s->cfg.ay) {
        s->ay_dec[0] = zr_bus_add_io(&s->bus, 0, 0, ay_read, ay_address, s);
        s->ay_dec[1] = zr_bus_add_io(&s->bus, 0, 0, NULL, ay_data, s);
        zr_spectrum_ay_decode(s, s->cfg.ay_mask, s->cfg.ay_value);
    }
    /* The CPU. */
    s->cpu_bus.ctx = s;
    s->cpu_bus.read = cpu_read;
    s->cpu_bus.write = cpu_write;
    s->cpu_bus.in = cpu_in;
    s->cpu_bus.out = cpu_out;
    s->cpu_bus.ack = cpu_ack;
    if (s->cfg.spectrum_timing) s->cpu_bus.in = cpu_in_sinclair;
    if (s->cfg.spectrum_timing || s->cfg.stretch) s->cpu_bus.stretch = cpu_stretch;
    /* Loader acceleration: the bus through the recogniser's callbacks, whether it is on or not (the tape slot's setting
     * switches it while the machine runs). */
    {
        s->cpu_bus.write = cpu_write_accel;
        s->cpu_bus.in = cpu_in_accel;
        s->cpu_bus.out = cpu_out_accel;
        s->cpu_bus.ack = cpu_ack_accel;
        const zr_loadaccel_host accel = {.ctx = s, .peek = accel_peek, .level_until = accel_level_until,
                                         .quiet = s->cfg.spectrum_timing || s->cfg.stretch ? accel_quiet_all : NULL};
        zr_loadaccel_init(&s->accel, &accel);
    }
    zr_z80_init(&s->z80, &s->cpu_bus, NULL, s->cfg.cmos);
    /* The ROM tape traps, where the ROM has the routines and the options say so. */
    const zr_traps_host traps = {.ctx = s, .read = traps_read, .write = traps_write, .break_held = traps_break,
                                 .block_next = traps_block_next, .block_done = traps_block_done, .block_put = traps_block_put};
    const uint32_t traps_rom = s->cfg.traps_rom < s->rom_size ? s->cfg.traps_rom : 0u;
    zr_traps_init(&s->traps, &s->z80, &traps, s->rom + traps_rom, s->rom_size - traps_rom);
    zr_traps_enable(&s->traps, s->cfg.tape_traps, s->cfg.tape_save_traps);
    install_hooks(s);
    /* The chips. */
    zr_ula_init(&s->ula, &s->host, &s->cfg.ula, video_read, s);
    if (s->cfg.spectrum_timing && s->cfg.gate_array) gate_array_contention(&s->ula);
    zr_beeper_init(&s->beeper, &s->host, s->cfg.clock_hz, ZR_SPECTRUM_AUDIO_RATE);
    if (s->cfg.ay) {
        const int mode = s->cfg.ay == ZR_SPECTRUM_AY_ABC ? ZR_AY_ABC : s->cfg.ay == ZR_SPECTRUM_AY_ACB ? ZR_AY_ACB : ZR_AY_MONO;
        zr_ay_init(&s->ay, &s->host, s->cfg.clock_hz, s->cfg.clock_hz / 8u, ZR_SPECTRUM_AUDIO_RATE, mode, NULL);
    }
    zr_tape_deck_init(&s->deck, s->cfg.clock_hz);
    zr_tape_deck_set_48k(&s->deck, s->cfg.is_48k);
    s->tape_dev.ops = &TAPE_OPS;
    s->tape_dev.host = &s->host;
    s->tape_dev.name = "tape";
    if (!interface_create(s, setup, error, capacity)) {
        zr_spectrum_destroy(s);
        return NULL;
    }
    zr_spectrum_reset(s, 1);
    frame_rebase(s);
    return s;
}

void zr_spectrum_destroy(void *self) {
    zr_spectrum *s = self;
    if (!s) return;
    /* For the checks (tools/verify.py): how much loader acceleration jumped, when ZETRO_ACCEL_STATS is set. */
    if (s->cfg.tape_accelerate && getenv("ZETRO_ACCEL_STATS"))
        fprintf(stderr, "loadaccel: %llu jumps, %llu of %llu T-states jumped\n", (unsigned long long)s->accel.jumps,
                (unsigned long long)s->accel.tstates, (unsigned long long)s->z80.t);
    zr_tape_deck_release(&s->deck);
    zr_tape_close(s->tape);
    for (unsigned i = 0; i < ZR_SPECTRUM_INTERFACE_SLOTS; ++i)
        if (s->card[i]) {
            zr_card_flush(s->card[i]);
            zr_card_close(s->card[i]);
        }
    free(s->dan);
    free(s->divide);
    free(s->divmmc);
    free(s->iface_firmware);
    free(s->iface_overlay);
    free(s->rom);
    free(s->ram);
    free(s);
}

const char *zr_spectrum_description(const void *self) { return ((const zr_spectrum *)self)->description; }
const char *zr_spectrum_error(const void *self) { return ((const zr_spectrum *)self)->error; }

/* Power on: the RAM cleared, the processor as it wakes; RESET: the processor's RESET pin. Both clear the
 * port FE latch (border black, speaker and MIC low) and bring the ULA's interrupt back to its frame. */
void zr_spectrum_reset(void *self, int power_on) {
    zr_spectrum *s = self;
    const uint64_t now = zr_spectrum_now(s);
    if (power_on) {
        memset(s->ram, 0, s->cfg.ram_size);
        zr_z80_power(&s->z80);
    } else zr_z80_reset(&s->z80);
    zr_z80_int(&s->z80, 0);
    if (s->dan && !s->iface_resetting) zr_dandanator_reset(s->dan);
    if (s->div_mem) {
        if (s->divide) zr_divide_reset(s->divide, power_on);
        else zr_divmmc_reset(s->divmmc, power_on);
        div_map(s);
    }
    s->ula.dev.ops->reset(&s->ula);
    if (s->cfg.ay) s->ay.dev.ops->reset(&s->ay);
    fe_apply(s, 0, now);
}

/* The NMI key: the board's NMI button, or with the Dandanator's set in, the cartridge's button, which resets the machine
 * and the cartridge into its menu (nominal). */
void zr_spectrum_nmi(void *self) {
    zr_spectrum *s = self;
    if (s->dan && s->dan->set) s->family->impl->reset(s, 0);
    else if (s->div_mem) zr_divmem_button(s->div_mem);
    else if (s->cfg.nmi) zr_z80_nmi(&s->z80);
}

/* --- time ------------------------------------------------------------------------------------------- */

uint32_t zr_spectrum_clock(const void *self) { return ((const zr_spectrum *)self)->cfg.clock_hz; }
uint64_t zr_spectrum_time(const void *self) { return zr_spectrum_now(self); }
uint64_t zr_spectrum_frame_period(const void *self) { return zr_ula_frame_ticks(&((const zr_spectrum *)self)->ula); }

zr_run_status zr_spectrum_run(void *self, uint64_t ticks) {
    zr_spectrum *s = self;
    s->target += ticks;
    uint64_t now;
    /* Loader acceleration starts afresh with every run (whatever the shell did between runs: a state loaded, a poke, a
     * key), and stands aside while the bus is traced (the trace would miss the reads it jumps over). */
    if (s->cfg.tape_accelerate && !s->bus.trace) {
        zr_loadaccel_restart(&s->accel);
        now = zr_loadaccel_run_z80(&s->accel, &s->sched, &s->z80, &s->clock, s->target);
    } else now = zr_sched_run_z80(&s->sched, &s->z80, &s->clock, s->target);
    zr_ula_catch_up(&s->ula, now);
    if (s->tape) zr_tape_deck_advance(&s->deck, now);
    return ZR_RUN_OK;
}

/* --- the picture and the text ------------------------------------------------------------------------ */

int zr_spectrum_frame(void *self, zr_frame *frame) { return zr_ula_frame(&((zr_spectrum *)self)->ula, frame); }

/* Each 8 x 8 cell of the bitmap matched against the character set, as it is or inverted (ink and paper
 * swapped); a cell that matches no glyph is '?'. A character is its code (60h the pound sign, 7Fh the
 * copyright sign). Lines without the spaces at their ends. */
size_t zr_spectrum_text(void *self, char *out, size_t capacity) {
    const zr_spectrum *s = self;
    size_t n = 0;
    for (unsigned row = 0; row < 24u; ++row) {
        char line[32];
        unsigned last = 0;
        for (unsigned col = 0; col < 32u; ++col) {
            uint64_t cell = 0;
            for (unsigned y = 0; y < 8u; ++y) {
                const unsigned py = row * 8u + y;
                cell = cell << 8 | s->screen[(py & 0xC0u) << 5 | (py & 7u) << 8 | (py & 0x38u) << 2 | col];
            }
            char ch = '?';
            for (unsigned g = 0; g < 96u; ++g)
                if (s->glyph[g] == cell || s->glyph[g] == ~cell) { ch = (char)(0x20u + g); break; }
            line[col] = ch;
            if (ch != ' ') last = col + 1u;
        }
        for (unsigned col = 0; col < last && n + 2 < capacity; ++col) out[n++] = line[col];
        if (n + 1 < capacity) out[n++] = '\n';
    }
    if (capacity) out[n < capacity ? n : capacity - 1] = 0;
    return n;
}

/* The 64-column read-back (spectrum.h): each cell of 4 x 8 pixels against the board's glyphs. */
size_t zr_spectrum_text_narrow(void *self, const uint8_t *font, char *out, size_t capacity) {
    const zr_spectrum *s = self;
    size_t n = 0;
    for (unsigned row = 0; row < 24u; ++row) {
        char line[64];
        unsigned last = 0;
        for (unsigned col = 0; col < 64u; ++col) {
            uint8_t cell[8];
            for (unsigned y = 0; y < 8u; ++y) {
                const unsigned py = row * 8u + y;
                const uint8_t byte = s->screen[(py & 0xC0u) << 5 | (py & 7u) << 8 | (py & 0x38u) << 2 | col >> 1];
                cell[y] = col & 1u ? byte & 15u : byte >> 4;
            }
            char ch = '?';
            for (unsigned g = 0; g < 95u && ch == '?'; ++g) {
                unsigned same = 1, inverted = 1;
                for (unsigned y = 0; y < 8u; ++y) {
                    const uint8_t want = font[g * 8u + y] & 15u;
                    if (cell[y] != want) same = 0;
                    if (cell[y] != (uint8_t)(~want & 15u)) inverted = 0;
                }
                if (same || inverted) ch = (char)(0x20u + g);
            }
            line[col] = ch;
            if (ch != ' ') last = col + 1u;
        }
        for (unsigned col = 0; col < last && n + 2 < capacity; ++col) out[n++] = line[col];
        if (n + 1 < capacity) out[n++] = '\n';
    }
    if (capacity) out[n < capacity ? n : capacity - 1] = 0;
    return n;
}

/* --- sound --------------------------------------------------------------------------------------------- */

uint32_t zr_spectrum_audio_rate(const void *self) { (void)self; return ZR_SPECTRUM_AUDIO_RATE; }
size_t zr_spectrum_audio_pull(void *self, int16_t *stereo, size_t frames) {
    zr_spectrum *s = self;
    const uint64_t now = zr_spectrum_now(s);
    const size_t n = zr_beeper_render(&s->beeper, now, stereo, frames);
    if (s->cfg.ay) zr_ay_render_add(&s->ay, now, stereo, n);
    if (s->drives) zr_drivesnd_render_add(s->drives, now, stereo, n);
    return n;
}

/* --- input ----------------------------------------------------------------------------------------------- */

void zr_spectrum_key(void *self, unsigned key, int down) {
    zr_spectrum *s = self;
    if (key < ZR_SPECTRUM_KEYS) zr_ula_key(&s->ula, key / 5u, key % 5u, down);
}
void zr_spectrum_joystick(void *self, unsigned stick, unsigned held) {
    zr_spectrum *s = self;
    if (!stick) s->stick = held;
    else if (stick == 1u) s->stick2 = held;
}

/* --- media: the tape recorder --------------------------------------------------------------------------- */

int zr_spectrum_insert(void *self, unsigned slot, zr_media *media, char *error, size_t capacity) {
    zr_spectrum *s = self;
    const int index = interface_index(s, slot);
    if (index >= 0) return interface_insert(s, (unsigned)index, media, error, capacity);
    if (slot != s->cfg.tape_slot) return fail(error, capacity, "%s", "There is no such slot");
    if (s->tape_media) return fail(error, capacity, "%s", "The slot is not empty");
    zr_tape *tape = zr_tape_open(media, error, capacity);
    if (!tape) return 0;
    s->tape_media = media;
    s->tape = tape;
    zr_tape_deck_set_tape(&s->deck, tape);
    tape_arm(s);
    return 1;
}
zr_media *zr_spectrum_media(const void *self, unsigned slot) {
    const zr_spectrum *s = self;
    const int index = interface_index(s, slot);
    if (index >= 0) return s->iface_media[index];
    return slot == s->cfg.tape_slot ? s->tape_media : NULL;
}

/* A recording under way goes onto the tape, and the tape into its file. */
static void finish_recording(zr_spectrum *s) {
    if (!s->tape || !zr_tape_deck_recording(&s->deck)) return;
    char why[128];
    if (zr_tape_deck_record(&s->deck, 0, zr_spectrum_now(s)) && zr_tape_modified(s->tape))
        if (!zr_tape_save_media(s->tape, s->tape_media, NULL, why, sizeof why)) snprintf(s->error, sizeof s->error, "%s", why);
}

int zr_spectrum_eject(void *self, unsigned slot) {
    zr_spectrum *s = self;
    const int index = interface_index(s, slot);
    if (index >= 0) return interface_eject(s, (unsigned)index);
    if (slot != s->cfg.tape_slot || !s->tape_media) return 0;
    finish_recording(s);
    zr_tape_deck_set_tape(&s->deck, NULL);
    zr_tape_close(s->tape);
    s->tape = NULL;
    s->tape_media = NULL;
    tape_arm(s);
    if (s->cfg.tape_sound) zr_beeper_set(&s->beeper, ZR_BEEPER_EAR, 0, zr_spectrum_now(s));
    return 1;
}

unsigned zr_spectrum_activity(const void *self) {
    const zr_spectrum *s = self;
    unsigned bits = s->tape && (zr_tape_deck_playing(&s->deck) || zr_tape_deck_recording(&s->deck)) ? 1u << s->cfg.tape_slot : 0u;
    /* A card or a drive at work: the slot of the drive the channel has selected, or of the card. */
    if (s->divide && zr_divide_light(s->divide)) bits |= 1u << (s->cfg.interface_slot + 1u + s->divide->ata.selected);
    else if (s->divmmc && zr_divmmc_light(s->divmmc)) bits |= 1u << (s->cfg.interface_slot + 1u);
    return bits;
}

int zr_spectrum_control(void *self, unsigned slot, unsigned command, uint32_t value) {
    zr_spectrum *s = self;
    const int index = interface_index(s, slot);
    if (index > 0) {
        /* The write-protect tab of a card slot moved: the image is read-only at once, or as the file allows. */
        if (command != ZR_MEDIA_PROTECT || !s->card[index]) return 0;
        zr_card_set_protect(s->card[index], value != 0);
        return s->card[index]->writable != 0;
    }
    if (slot != s->cfg.tape_slot || !s->tape) return 0;
    const uint64_t now = zr_spectrum_now(s);
    if (command != ZR_MEDIA_RECORD) finish_recording(s);
    switch (command) {
    case ZR_MEDIA_PLAY: if (!zr_tape_deck_play(&s->deck, now)) return 0; break;
    case ZR_MEDIA_STOP: zr_tape_deck_stop(&s->deck, now); break;
    case ZR_MEDIA_REWIND: zr_tape_deck_rewind(&s->deck, now); break;
    case ZR_MEDIA_NEXT: zr_tape_deck_next(&s->deck, now); break;
    case ZR_MEDIA_PREVIOUS: zr_tape_deck_previous(&s->deck, now); break;
    case ZR_MEDIA_SEEK: zr_tape_deck_seek(&s->deck, value, now); break;
    case ZR_MEDIA_RECORD:
        if (zr_tape_deck_recording(&s->deck)) finish_recording(s);
        else if (!zr_tape_deck_record(&s->deck, 1, now)) return 0;
        else zr_tape_deck_output(&s->deck, s->fe >> 3 & 1u, now);
        break;
    default: return 0;
    }
    tape_arm(s);
    return 1;
}

unsigned zr_spectrum_tape_options(void *self, unsigned slot, unsigned bits) {
    zr_spectrum *s = self;
    if (slot != s->cfg.tape_slot) return 0;
    if (bits & ZR_TAPE_QUERY) return ZR_TAPE_SOUND | ZR_TAPE_TRAPS | ZR_TAPE_SAVE_TRAPS | ZR_TAPE_ACCELERATE;
    if (s->cfg.tape_sound && !(bits & ZR_TAPE_SOUND)) zr_beeper_set(&s->beeper, ZR_BEEPER_EAR, 0, zr_spectrum_now(s));
    s->cfg.tape_sound = (bits & ZR_TAPE_SOUND) != 0;
    s->cfg.tape_traps = (bits & ZR_TAPE_TRAPS) != 0;
    s->cfg.tape_save_traps = (bits & ZR_TAPE_SAVE_TRAPS) != 0;
    s->cfg.tape_accelerate = (bits & ZR_TAPE_ACCELERATE) != 0;
    zr_traps_enable(&s->traps, s->cfg.tape_traps, s->cfg.tape_save_traps);
    install_hooks(s);
    return ZR_TAPE_SOUND | ZR_TAPE_TRAPS | ZR_TAPE_SAVE_TRAPS | ZR_TAPE_ACCELERATE;
}

struct zr_tape_deck *zr_spectrum_tape_deck(void *self, unsigned slot) {
    zr_spectrum *s = self;
    return slot == s->cfg.tape_slot && s->tape ? &s->deck : NULL;
}

/* --- states -------------------------------------------------------------------------------------------- */

void zr_spectrum_save(const void *self, zr_state_writer *w) {
    const zr_spectrum *s = self;
    zr_sw_begin(w, "SPEC", 0);
    zr_sw_u8(w, STATE_VERSION);
    zr_tclock_save(&s->clock, w);
    zr_sw_u64(w, s->target);
    zr_sw_u8(w, s->fe);
    zr_sw_u32(w, s->stick);
    zr_sw_end(w);
    /* Spectrum timing is part of the machine's build: its states say which Sinclair machine they ran as. */
    if (s->cfg.spectrum_timing) {
        zr_sw_begin(w, "TIMG", 0);
        zr_sw_u8(w, s->cfg.sinclair);
        zr_sw_end(w);
    }
    zr_sw_begin(w, "Z80 ", 0);
    zr_z80_save(&s->z80, w);
    zr_sw_end(w);
    zr_sw_begin(w, "RAM ", 0);
    zr_sw_packed(w, s->ram, s->cfg.ram_size);
    zr_sw_end(w);
    zr_sw_begin(w, "ULA ", 0);
    s->ula.dev.ops->save(&s->ula, w);
    zr_sw_end(w);
    zr_sw_begin(w, "BEEP", 0);
    s->beeper.dev.ops->save(&s->beeper, w);
    zr_sw_end(w);
    if (s->family && s->family->joysticks > 1u) {
        zr_sw_begin(w, "JOY2", 0);
        zr_sw_u32(w, s->stick2);
        zr_sw_end(w);
    }
    if (s->cfg.ay) {
        zr_sw_begin(w, "AY  ", 0);
        s->ay.dev.ops->save(&s->ay, w);
        zr_sw_end(w);
    }
    if (s->tape) {
        zr_sw_begin(w, "TAPE", 0);
        zr_tape_deck_save(&s->deck, w);
        zr_sw_end(w);
    }
    if (s->dan) zr_dandanator_save(s->dan, w);
    if (s->divide) zr_divide_save(s->divide, w);
    if (s->divmmc) zr_divmmc_save(s->divmmc, w);
}

int zr_spectrum_load(void *self, zr_state_reader *r) {
    zr_spectrum *s = self;
    /* A state of the other timing is refused before anything changes. */
    const unsigned timing = zr_sr_chunk(r, "TIMG", 0) ? zr_sr_u8(r) : 0u;
    if (timing != (s->cfg.spectrum_timing ? s->cfg.sinclair : 0u)) return 0;
    /* So is a state of a machine with the Dandanator in one without it. */
    if (!s->dan && zr_sr_chunk(r, "DAN ", 0)) return 0;
    if (!s->divide && zr_sr_chunk(r, "DIVI", 0)) return 0;
    if (!s->divmmc && zr_sr_chunk(r, "DIVM", 0)) return 0;
    /* The clock and the CPU first: the chips' loads arm their deadlines at the time these give. */
    if (!zr_sr_chunk(r, "SPEC", 0) || zr_sr_u8(r) != STATE_VERSION) return 0;
    zr_tclock_load(&s->clock, r);
    s->target = zr_sr_u64(r);
    s->fe = zr_sr_u8(r) & 0x1Fu;
    s->stick = zr_sr_u32(r);
    if (!zr_sr_chunk(r, "Z80 ", 0)) return 0;
    zr_z80_load(&s->z80, r);
    if (!zr_sr_chunk(r, "RAM ", 0)) return 0;
    zr_sr_packed(r, s->ram, s->cfg.ram_size);
    if (!zr_sr_chunk(r, "ULA ", 0) || !s->ula.dev.ops->load(&s->ula, r)) return 0;
    if (!zr_sr_chunk(r, "BEEP", 0) || !s->beeper.dev.ops->load(&s->beeper, r)) return 0;
    s->stick2 = zr_sr_chunk(r, "JOY2", 0) ? zr_sr_u32(r) : 0u;
    if (s->cfg.ay && (!zr_sr_chunk(r, "AY  ", 0) || !s->ay.dev.ops->load(&s->ay, r))) return 0;
    /* The recorder's position, when the state has one and a tape is in. */
    if (s->tape) {
        /* A state of another tape leaves this one rewound. */
        if (!zr_sr_chunk(r, "TAPE", 0) || !zr_tape_deck_load(&s->deck, r)) zr_tape_deck_rewind(&s->deck, zr_spectrum_now(s));
    }
    if (s->dan && !zr_dandanator_load(s->dan, r)) return 0;
    if (s->divide && !zr_divide_load(s->divide, r)) return 0;
    if (s->divmmc && !zr_divmmc_load(s->divmmc, r)) return 0;
    tape_arm(s);
    frame_rebase(s);
    return zr_sr_ok(r);
}

uint32_t zr_spectrum_rom_hashes(const void *self, uint8_t *hashes, uint32_t max) {
    const zr_spectrum *s = self;
    uint32_t n = 0;
    for (; n < s->rom_count && n < max; ++n) memcpy(hashes + n * 32u, s->rom_hash[n], 32);
    return n;
}

/* --- checks ---------------------------------------------------------------------------------------------- */

void zr_spectrum_set_trace(void *self, zr_trace_fn fn, void *ctx) { zr_bus_set_trace(&((zr_spectrum *)self)->bus, fn, ctx); }
int zr_spectrum_peek(void *self, uint32_t address, uint8_t *value) {
    if (address > 0xFFFFu) return zr_spectrum_page_peek(self, address, value);
    *value = zr_bus_peek(&((zr_spectrum *)self)->bus, (uint16_t)address);
    return 1;
}
int zr_spectrum_poke(void *self, uint32_t address, uint8_t value) {
    if (address > 0xFFFFu) return zr_spectrum_page_poke(self, address, value);
    zr_bus_write(&((zr_spectrum *)self)->bus, (uint16_t)address, value);
    return 1;
}

const zr_machine_impl zr_spectrum_impl = {
    .destroy = zr_spectrum_destroy, .description = zr_spectrum_description, .error = zr_spectrum_error,
    .reset = zr_spectrum_reset, .nmi = zr_spectrum_nmi,
    .clock = zr_spectrum_clock, .run = zr_spectrum_run, .time = zr_spectrum_time, .frame_period = zr_spectrum_frame_period,
    .frame = zr_spectrum_frame, .text = zr_spectrum_text, .audio_rate = zr_spectrum_audio_rate, .audio_pull = zr_spectrum_audio_pull,
    .key = zr_spectrum_key, .joystick = zr_spectrum_joystick,
    .insert = zr_spectrum_insert, .media = zr_spectrum_media, .eject = zr_spectrum_eject, .activity = zr_spectrum_activity,
    .control = zr_spectrum_control, .tape_options = zr_spectrum_tape_options, .tape_deck = zr_spectrum_tape_deck,
    .save = zr_spectrum_save, .load = zr_spectrum_load, .rom_hashes = zr_spectrum_rom_hashes,
    .set_trace = zr_spectrum_set_trace, .peek = zr_spectrum_peek, .poke = zr_spectrum_poke,
    .snapshot_load = zr_spectrum_snapshot_load, .snapshot_save = zr_spectrum_snapshot_save,
};
